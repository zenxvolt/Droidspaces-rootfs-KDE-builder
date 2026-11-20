/*
 * A minimal PulseAudio native-protocol server. See pulse-server.h.
 *
 * Supported: authentication (anonymous), client names and properties,
 * playback streams in U8 / S16 / S32 / FLOAT32 / S24 (little endian, any
 * channel count), corking, flushing, triggering, prebuffering, draining,
 * latency queries, volume, and the few information queries clients make while
 * connecting. Data is always sent over the socket itself; shared memory is
 * never offered. Recording is refused (no microphone path yet).
 *
 * The protocol details follow PulseAudio's src/pulsecore/protocol-native.c and
 * src/pulse/stream.c (LGPL-2.1); no code is taken from it.
 */
#define _GNU_SOURCE
#include "pulse-server.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>

/* Provided by the bridge. */
void logmsg(const char *level, const char *fmt, ...);
extern int g_verbose;
#define PINFO(...) logmsg("I", __VA_ARGS__)
#define PWARN(...) logmsg("W", __VA_ARGS__)
#define PDBG(...)  do { if (g_verbose) logmsg("D", __VA_ARGS__); } while (0)

/* ---- protocol constants ---- */

#define PROTOCOL_VERSION 35
#define PROTOCOL_VERSION_MASK 0x0000FFFFu
#define COOKIE_LENGTH 256
#define DESCRIPTOR_SIZE 20
#define MAX_FRAME (16u * 1024 * 1024)
#define CONTROL_CHANNEL 0xFFFFFFFFu
#define INVALID_INDEX 0xFFFFFFFFu
#define SEEK_MASK 0xFFu

enum {
    CMD_ERROR = 0, CMD_REPLY = 2,
    CMD_CREATE_PLAYBACK_STREAM = 3, CMD_DELETE_PLAYBACK_STREAM = 4,
    CMD_CREATE_RECORD_STREAM = 5, CMD_DELETE_RECORD_STREAM = 6,
    CMD_EXIT = 7, CMD_AUTH = 8, CMD_SET_CLIENT_NAME = 9,
    CMD_LOOKUP_SINK = 10, CMD_LOOKUP_SOURCE = 11,
    CMD_DRAIN_PLAYBACK_STREAM = 12, CMD_STAT = 13, CMD_GET_PLAYBACK_LATENCY = 14,
    CMD_GET_SERVER_INFO = 20,
    CMD_SUBSCRIBE = 35,
    CMD_SET_SINK_VOLUME = 36, CMD_SET_SINK_INPUT_VOLUME = 37,
    CMD_SET_SINK_MUTE = 39,
    CMD_CORK_PLAYBACK_STREAM = 41, CMD_FLUSH_PLAYBACK_STREAM = 42,
    CMD_TRIGGER_PLAYBACK_STREAM = 43,
    CMD_SET_PLAYBACK_STREAM_NAME = 46,
    CMD_PREBUF_PLAYBACK_STREAM = 60,
    CMD_REQUEST = 61,
    CMD_SET_SINK_INPUT_MUTE = 69,
    CMD_SET_PLAYBACK_STREAM_BUFFER_ATTR = 72,
    CMD_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE = 74,
    CMD_UPDATE_RECORD_STREAM_PROPLIST = 80, CMD_UPDATE_PLAYBACK_STREAM_PROPLIST = 81,
    CMD_UPDATE_CLIENT_PROPLIST = 82, CMD_REMOVE_RECORD_STREAM_PROPLIST = 83,
    CMD_REMOVE_PLAYBACK_STREAM_PROPLIST = 84, CMD_REMOVE_CLIENT_PROPLIST = 85,
    CMD_STARTED = 86,
};

enum {
    ERR_ACCESS = 1, ERR_COMMAND = 2, ERR_INVALID = 3, ERR_NOENTITY = 5,
    ERR_PROTOCOL = 7, ERR_VERSION = 17, ERR_NOTSUPPORTED = 19,
};

enum {
    TAG_STRING = 't', TAG_STRING_NULL = 'N', TAG_U32 = 'L', TAG_U8 = 'B', TAG_U64 = 'R',
    TAG_S64 = 'r', TAG_SAMPLE_SPEC = 'a', TAG_ARBITRARY = 'x', TAG_TRUE = '1',
    TAG_FALSE = '0', TAG_TIMEVAL = 'T', TAG_USEC = 'U', TAG_CHANNEL_MAP = 'm',
    TAG_CVOLUME = 'v', TAG_PROPLIST = 'P', TAG_VOLUME = 'V', TAG_FORMAT_INFO = 'f',
};

enum {
    SAMPLE_U8 = 0, SAMPLE_S16LE = 3, SAMPLE_FLOAT32LE = 5, SAMPLE_S32LE = 7,
    SAMPLE_S24LE = 9, SAMPLE_S24_32LE = 11,
};

#define CHANNELS_MAX 32
#define VOLUME_NORM 0x10000u
#define ENCODING_PCM 1

#define SINK_NAME "anland"
#define SINK_INDEX 0

/* ---- buffers ---- */

struct bytes {
    uint8_t *data;
    size_t len, cap;
};

static bool bytes_reserve(struct bytes *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *d = realloc(b->data, cap);
    if (!d)
        return false;
    b->data = d;
    b->cap = cap;
    return true;
}

static void bytes_put(struct bytes *b, const void *p, size_t n)
{
    if (bytes_reserve(b, n)) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
}

static void bytes_consume(struct bytes *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

/* ---- tagstruct writer ---- */

static void put_raw_u8(struct bytes *b, uint8_t v) { bytes_put(b, &v, 1); }
static void put_raw_u32(struct bytes *b, uint32_t v)
{
    uint8_t x[4] = { v >> 24, v >> 16, v >> 8, v };
    bytes_put(b, x, 4);
}
static void put_raw_u64(struct bytes *b, uint64_t v)
{
    put_raw_u32(b, (uint32_t)(v >> 32));
    put_raw_u32(b, (uint32_t)v);
}
static void put_u32(struct bytes *b, uint32_t v) { put_raw_u8(b, TAG_U32); put_raw_u32(b, v); }
static void put_u8(struct bytes *b, uint8_t v) { put_raw_u8(b, TAG_U8); put_raw_u8(b, v); }
static void put_u64(struct bytes *b, uint64_t v) { put_raw_u8(b, TAG_U64); put_raw_u64(b, v); }
static void put_s64(struct bytes *b, int64_t v) { put_raw_u8(b, TAG_S64); put_raw_u64(b, (uint64_t)v); }
static void put_usec(struct bytes *b, uint64_t v) { put_raw_u8(b, TAG_USEC); put_raw_u64(b, v); }
static void put_bool(struct bytes *b, bool v) { put_raw_u8(b, v ? TAG_TRUE : TAG_FALSE); }
static void put_string(struct bytes *b, const char *s)
{
    if (!s) {
        put_raw_u8(b, TAG_STRING_NULL);
        return;
    }
    put_raw_u8(b, TAG_STRING);
    bytes_put(b, s, strlen(s) + 1);
}
static void put_timeval(struct bytes *b, const struct timeval *tv)
{
    put_raw_u8(b, TAG_TIMEVAL);
    put_raw_u32(b, (uint32_t)tv->tv_sec);
    put_raw_u32(b, (uint32_t)tv->tv_usec);
}
static void put_sample_spec(struct bytes *b, uint8_t format, uint8_t channels, uint32_t rate)
{
    put_raw_u8(b, TAG_SAMPLE_SPEC);
    put_raw_u8(b, format);
    put_raw_u8(b, channels);
    put_raw_u32(b, rate);
}
static void put_channel_map(struct bytes *b, uint8_t channels, const uint8_t *map)
{
    put_raw_u8(b, TAG_CHANNEL_MAP);
    put_raw_u8(b, channels);
    for (unsigned i = 0; i < channels; i++)
        put_raw_u8(b, map[i]);
}
/* One property; string values carry their terminating NUL like PulseAudio's. */
static void put_prop(struct bytes *b, const char *key, const char *value)
{
    size_t n = strlen(value) + 1;
    put_string(b, key);
    put_u32(b, (uint32_t)n);
    put_raw_u8(b, TAG_ARBITRARY);
    put_raw_u32(b, (uint32_t)n);
    bytes_put(b, value, n);
}

static const char *sample_format_name(uint8_t f)
{
    switch (f) {
    case SAMPLE_U8: return "u8";
    case SAMPLE_S16LE: return "s16le";
    case SAMPLE_FLOAT32LE: return "float32le";
    case SAMPLE_S32LE: return "s32le";
    case SAMPLE_S24LE: return "s24le";
    case SAMPLE_S24_32LE: return "s24-32le";
    default: return "s16le";
    }
}

static void put_pcm_format_info(struct bytes *b, uint8_t format, uint8_t channels, uint32_t rate)
{
    char v[64];
    put_raw_u8(b, TAG_FORMAT_INFO);
    put_u8(b, ENCODING_PCM);
    put_raw_u8(b, TAG_PROPLIST);
    snprintf(v, sizeof(v), "\"%s\"", sample_format_name(format));
    put_prop(b, "format.sample_format", v);
    snprintf(v, sizeof(v), "%u", rate);
    put_prop(b, "format.rate", v);
    snprintf(v, sizeof(v), "%u", channels);
    put_prop(b, "format.channels", v);
    put_string(b, NULL);
}

/* ---- tagstruct reader ---- */

struct reader {
    const uint8_t *p;
    size_t len, pos;
    bool bad;
};

static bool rd_have(struct reader *r, size_t n)
{
    if (r->bad || r->pos + n > r->len) {
        r->bad = true;
        return false;
    }
    return true;
}
static uint8_t rd_raw_u8(struct reader *r)
{
    return rd_have(r, 1) ? r->p[r->pos++] : 0;
}
static uint32_t rd_raw_u32(struct reader *r)
{
    if (!rd_have(r, 4))
        return 0;
    const uint8_t *x = r->p + r->pos;
    r->pos += 4;
    return (uint32_t)x[0] << 24 | (uint32_t)x[1] << 16 | (uint32_t)x[2] << 8 | x[3];
}
static bool rd_tag(struct reader *r, uint8_t tag)
{
    if (!rd_have(r, 1) || r->p[r->pos] != tag) {
        r->bad = true;
        return false;
    }
    r->pos++;
    return true;
}
static uint32_t rd_u32(struct reader *r) { return rd_tag(r, TAG_U32) ? rd_raw_u32(r) : 0; }
static uint8_t rd_u8(struct reader *r) { return rd_tag(r, TAG_U8) ? rd_raw_u8(r) : 0; }
static bool rd_bool(struct reader *r)
{
    if (!rd_have(r, 1))
        return false;
    uint8_t t = r->p[r->pos++];
    if (t == TAG_TRUE)
        return true;
    if (t != TAG_FALSE)
        r->bad = true;
    return false;
}
/* Returns a pointer into the packet, or NULL for a null string. */
static const char *rd_string(struct reader *r)
{
    if (!rd_have(r, 1))
        return NULL;
    uint8_t t = r->p[r->pos++];
    if (t == TAG_STRING_NULL)
        return NULL;
    if (t != TAG_STRING) {
        r->bad = true;
        return NULL;
    }
    const uint8_t *end = memchr(r->p + r->pos, 0, r->len - r->pos);
    if (!end) {
        r->bad = true;
        return NULL;
    }
    const char *s = (const char *)(r->p + r->pos);
    r->pos = (size_t)(end - r->p) + 1;
    return s;
}
static const void *rd_arbitrary(struct reader *r, uint32_t *len)
{
    *len = 0;
    if (!rd_tag(r, TAG_ARBITRARY))
        return NULL;
    uint32_t n = rd_raw_u32(r);
    if (!rd_have(r, n))
        return NULL;
    const void *p = r->p + r->pos;
    r->pos += n;
    *len = n;
    return p;
}
static void rd_sample_spec(struct reader *r, uint8_t *format, uint8_t *channels, uint32_t *rate)
{
    if (!rd_tag(r, TAG_SAMPLE_SPEC))
        return;
    *format = rd_raw_u8(r);
    *channels = rd_raw_u8(r);
    *rate = rd_raw_u32(r);
}
static uint8_t rd_channel_map(struct reader *r, uint8_t *map)
{
    if (!rd_tag(r, TAG_CHANNEL_MAP))
        return 0;
    uint8_t n = rd_raw_u8(r);
    if (n > CHANNELS_MAX) {
        r->bad = true;
        return 0;
    }
    for (unsigned i = 0; i < n; i++)
        map[i] = rd_raw_u8(r);
    return n;
}
/* Returns the average of the channel volumes. */
static uint32_t rd_cvolume(struct reader *r, uint8_t *channels)
{
    *channels = 0;
    if (!rd_tag(r, TAG_CVOLUME))
        return VOLUME_NORM;
    uint8_t n = rd_raw_u8(r);
    if (n > CHANNELS_MAX) {
        r->bad = true;
        return VOLUME_NORM;
    }
    uint64_t sum = 0;
    for (unsigned i = 0; i < n; i++)
        sum += rd_raw_u32(r);
    *channels = n;
    return n ? (uint32_t)(sum / n) : VOLUME_NORM;
}
static void rd_timeval(struct reader *r, struct timeval *tv)
{
    if (!rd_tag(r, TAG_TIMEVAL))
        return;
    tv->tv_sec = rd_raw_u32(r);
    tv->tv_usec = rd_raw_u32(r);
}

/* Reads a property list. When want is set, the value of that key is copied
 * (as a string) into value. */
static void rd_proplist(struct reader *r, const char *want, char *value, size_t value_size)
{
    if (!rd_tag(r, TAG_PROPLIST))
        return;
    for (int i = 0; i < 4096 && !r->bad; i++) {
        const char *key = rd_string(r);
        if (!key || r->bad)
            return;
        uint32_t declared = rd_u32(r), n = 0;
        const void *v = rd_arbitrary(r, &n);
        if (r->bad || declared != n)
            return;
        if (want && value && !strcmp(key, want)) {
            size_t c = n < value_size - 1 ? n : value_size - 1;
            memcpy(value, v, c);
            value[c] = 0;
        }
    }
}

/* A format_info: only PCM is accepted; returns false otherwise. */
static bool rd_format_info(struct reader *r, uint8_t *format, uint8_t *channels, uint32_t *rate)
{
    if (!rd_tag(r, TAG_FORMAT_INFO))
        return false;
    uint8_t encoding = rd_u8(r);
    /* The values are JSON: numbers, or strings in quotes. */
    char fmt[64] = "", ch[32] = "", rt[32] = "";
    if (!rd_tag(r, TAG_PROPLIST))
        return false;
    for (int i = 0; i < 4096 && !r->bad; i++) {
        const char *key = rd_string(r);
        if (!key || r->bad)
            break;
        uint32_t declared = rd_u32(r), n = 0;
        const char *v = rd_arbitrary(r, &n);
        if (r->bad || declared != n)
            return false;
        char *dst = !strcmp(key, "format.sample_format") ? fmt :
                    !strcmp(key, "format.channels") ? ch :
                    !strcmp(key, "format.rate") ? rt : NULL;
        size_t size = dst == fmt ? sizeof(fmt) : sizeof(ch);
        if (dst) {
            size_t c = n < size - 1 ? n : size - 1;
            memcpy(dst, v, c);
            dst[c] = 0;
        }
    }
    if (encoding != ENCODING_PCM)
        return false;
    static const struct { const char *name; uint8_t code; } names[] = {
        { "\"u8\"", SAMPLE_U8 }, { "\"s16le\"", SAMPLE_S16LE },
        { "\"float32le\"", SAMPLE_FLOAT32LE }, { "\"s32le\"", SAMPLE_S32LE },
        { "\"s24le\"", SAMPLE_S24LE }, { "\"s24-32le\"", SAMPLE_S24_32LE },
    };
    bool known = !fmt[0];   /* no sample format given: keep the sample spec's */
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(fmt, names[i].name)) {
            *format = names[i].code;
            known = true;
        }
    if (atoi(ch) > 0)
        *channels = (uint8_t)atoi(ch);
    if (atoi(rt) > 0)
        *rate = (uint32_t)atoi(rt);
    return known;
}

/* ---- state ---- */

struct client;

struct stream {
    struct wl_list link;            /* client.streams */
    struct client *client;
    uint32_t channel, index;
    char name[64];

    uint8_t format, channels;
    uint32_t rate;
    size_t frame_size;
    uint8_t map[CHANNELS_MAX];
    float gain;
    bool muted;

    uint32_t maxlength, tlength, prebuf, minreq;
    uint8_t *ring;
    size_t ring_cap, ring_head, ring_fill;
    int64_t write_index, read_index;
    int64_t requested;              /* asked for but not yet received */

    bool corked;
    bool playing;                   /* prebuffer satisfied, data flows */
    bool draining;
    uint32_t drain_tag;

    /* Resampler state: the two input frames being interpolated between. */
    float prev[2], next[2];
    double frac;
    bool primed;

    uint64_t playing_for, underrun_for;  /* bytes, as PulseAudio reports them */
    bool logged_start;
};

struct client {
    struct wl_list link;            /* pulse_server.clients */
    struct pulse_server *srv;
    int fd;
    struct wl_event_source *source;
    uint32_t index;
    uint32_t version;
    bool authorized;
    bool closing;
    struct bytes in, out;
    struct wl_list streams;
    char name[64];
};

struct pulse_server {
    struct wl_event_loop *loop;
    int listen_fd;
    char path[PATH_MAX];
    struct wl_event_source *listen_source;
    struct wl_event_source *timer;
    bool timer_armed;
    struct wl_list clients;
    uint32_t next_index;

    pulse_output_fn output;
    void *output_data;
    uint32_t out_rate, out_channels;
    uint64_t out_latency_us;

    /* Output clock: frames produced since clock_start. */
    uint64_t clock_start_ns;
    uint64_t frames_done;
    bool clock_running;

    float *mix;
    int16_t *pcm;
    size_t mix_frames;
};

#define TICK_MS 10
#define MAX_TICK_FRAMES(rate) ((rate) / 10)  /* never catch up more than 100 ms */

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static size_t sample_size(uint8_t format)
{
    switch (format) {
    case SAMPLE_U8: return 1;
    case SAMPLE_S16LE: return 2;
    case SAMPLE_S24LE: return 3;
    case SAMPLE_FLOAT32LE: case SAMPLE_S32LE: case SAMPLE_S24_32LE: return 4;
    default: return 0;
    }
}

static size_t usec_to_bytes(const struct stream *s, uint64_t usec)
{
    return (size_t)(usec * s->rate / 1000000ull) * s->frame_size;
}

/* ---- sending ---- */

static void client_close(struct client *c);

static void client_flush(struct client *c)
{
    while (c->out.len) {
        ssize_t n = send(c->fd, c->out.data, c->out.len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            bytes_consume(&c->out, (size_t)n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            break;
        c->closing = true;
        return;
    }
    if (c->out.len > 4u * 1024 * 1024) {
        PWARN("pulse: client %u does not read its replies, dropping it", c->index);
        c->closing = true;
        return;
    }
    wl_event_source_fd_update(c->source, WL_EVENT_READABLE |
                              (c->out.len ? WL_EVENT_WRITABLE : 0));
}

static void send_packet(struct client *c, const struct bytes *payload)
{
    uint8_t d[DESCRIPTOR_SIZE] = { 0 };
    uint32_t len = (uint32_t)payload->len;
    d[0] = len >> 24; d[1] = len >> 16; d[2] = len >> 8; d[3] = len;
    memset(d + 4, 0xFF, 4); /* control channel */
    bytes_put(&c->out, d, sizeof(d));
    bytes_put(&c->out, payload->data, payload->len);
    client_flush(c);
}

static void reply_start(struct bytes *b, uint32_t tag)
{
    b->len = 0;
    put_u32(b, CMD_REPLY);
    put_u32(b, tag);
}

static void send_simple_reply(struct client *c, uint32_t tag)
{
    struct bytes b = { 0 };
    reply_start(&b, tag);
    send_packet(c, &b);
    free(b.data);
}

static void send_error(struct client *c, uint32_t tag, uint32_t error)
{
    struct bytes b = { 0 };
    put_u32(&b, CMD_ERROR);
    put_u32(&b, tag);
    put_u32(&b, error);
    send_packet(c, &b);
    free(b.data);
}

static void send_request(struct stream *s, uint32_t bytes)
{
    struct bytes b = { 0 };
    put_u32(&b, CMD_REQUEST);
    put_u32(&b, INVALID_INDEX);
    put_u32(&b, s->channel);
    put_u32(&b, bytes);
    send_packet(s->client, &b);
    free(b.data);
}

static void send_started(struct stream *s)
{
    if (s->client->version < 13)
        return;
    struct bytes b = { 0 };
    put_u32(&b, CMD_STARTED);
    put_u32(&b, INVALID_INDEX);
    put_u32(&b, s->channel);
    send_packet(s->client, &b);
    free(b.data);
}

/* Ask the client for more data once at least minreq bytes are free. */
static void stream_request(struct stream *s)
{
    int64_t queued = (int64_t)s->ring_fill;
    int64_t missing = (int64_t)s->tlength - queued - s->requested;
    if (missing <= 0 || missing < (int64_t)s->minreq)
        return;
    s->requested += missing;
    send_request(s, (uint32_t)missing);
}

/* ---- the output clock and mixer ---- */

static void server_arm_timer(struct pulse_server *srv);

static bool stream_active(const struct stream *s)
{
    return !s->corked && s->playing;
}

static bool any_stream_active(struct pulse_server *srv)
{
    struct client *c;
    wl_list_for_each(c, &srv->clients, link) {
        struct stream *s;
        wl_list_for_each(s, &c->streams, link)
            if (stream_active(s))
                return true;
    }
    return false;
}

static void stream_maybe_start(struct stream *s)
{
    if (s->playing || s->corked)
        return;
    if (s->ring_fill == 0 || s->ring_fill < s->prebuf)
        return;
    s->playing = true;
    s->primed = false;
    if (!s->logged_start) {
        PINFO("pulse: \"%s\" plays %s %u Hz x%u", s->name[0] ? s->name : s->client->name,
              sample_format_name(s->format), s->rate, s->channels);
        s->logged_start = true;
    }
    send_started(s);
    server_arm_timer(s->client->srv);
}

/* Takes one frame from the ring as up to two float channels. */
static bool stream_pop_frame(struct stream *s, float out[2])
{
    if (s->ring_fill < s->frame_size)
        return false;
    uint8_t frame[CHANNELS_MAX * 4];
    for (size_t i = 0; i < s->frame_size; i++)
        frame[i] = s->ring[(s->ring_head + i) % s->ring_cap];
    s->ring_head = (s->ring_head + s->frame_size) % s->ring_cap;
    s->ring_fill -= s->frame_size;
    s->read_index += (int64_t)s->frame_size;

    size_t ss = sample_size(s->format);
    float v[2] = { 0, 0 };
    unsigned use = s->channels < 2 ? s->channels : 2;
    for (unsigned ch = 0; ch < use; ch++) {
        const uint8_t *p = frame + ch * ss;
        switch (s->format) {
        case SAMPLE_U8: v[ch] = ((int)p[0] - 128) / 128.0f; break;
        case SAMPLE_S16LE: v[ch] = (int16_t)(p[0] | p[1] << 8) / 32768.0f; break;
        case SAMPLE_S24LE: {
            int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            v[ch] = (float)(x / 2147483648.0);
            break;
        }
        case SAMPLE_S24_32LE: {
            int32_t x = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
            v[ch] = (float)(x / 2147483648.0);
            break;
        }
        case SAMPLE_S32LE: {
            int32_t x = (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                                  (uint32_t)p[3] << 24);
            v[ch] = (float)(x / 2147483648.0);
            break;
        }
        case SAMPLE_FLOAT32LE: {
            uint32_t x = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
                         (uint32_t)p[3] << 24;
            memcpy(&v[ch], &x, 4);
            if (!isfinite(v[ch]))
                v[ch] = 0;
            break;
        }
        }
    }
    if (s->channels == 1)
        v[1] = v[0];
    out[0] = v[0];
    out[1] = v[1];
    return true;
}

/* Adds this stream's next n output frames to mix. Returns false on underrun. */
static bool stream_mix(struct stream *s, float *mix, size_t n, uint32_t out_rate,
                       uint32_t out_ch)
{
    double step = (double)s->rate / out_rate;
    float gain = s->muted ? 0.0f : s->gain;
    size_t before = s->ring_fill;
    bool ok = true;

    if (!s->primed) {
        if (!stream_pop_frame(s, s->next))
            return false;
        s->prev[0] = s->next[0];
        s->prev[1] = s->next[1];
        s->frac = 1.0;
        s->primed = true;
    }
    for (size_t i = 0; i < n; i++) {
        while (s->frac >= 1.0) {
            s->prev[0] = s->next[0];
            s->prev[1] = s->next[1];
            if (!stream_pop_frame(s, s->next)) {
                ok = false;
                break;
            }
            s->frac -= 1.0;
        }
        if (!ok)
            break;
        float t = (float)s->frac;
        float l = s->prev[0] + (s->next[0] - s->prev[0]) * t;
        float r = s->prev[1] + (s->next[1] - s->prev[1]) * t;
        if (out_ch == 1) {
            mix[i] += (l + r) * 0.5f * gain;
        } else {
            mix[i * out_ch] += l * gain;
            mix[i * out_ch + 1] += r * gain;
        }
        s->frac += step;
    }
    s->playing_for += before - s->ring_fill;
    return ok;
}

static void stream_after_consume(struct stream *s, bool underrun)
{
    if (underrun) {
        s->playing = false;   /* wait for the prebuffer again, like PulseAudio */
        s->primed = false;
    }
    if (s->draining && s->ring_fill == 0) {
        s->draining = false;
        send_simple_reply(s->client, s->drain_tag);
    }
    stream_request(s);
}

static int on_timer(void *data)
{
    struct pulse_server *srv = data;
    srv->timer_armed = false;

    uint64_t now = mono_ns();
    if (!srv->clock_running) {
        srv->clock_running = true;
        srv->clock_start_ns = now;
        srv->frames_done = 0;
    }
    uint64_t target = (now - srv->clock_start_ns) * srv->out_rate / 1000000000ull;
    if (target <= srv->frames_done) {
        server_arm_timer(srv);
        return 0;
    }
    /* After a stall, skip ahead instead of playing a burst to catch up. */
    if (target - srv->frames_done > MAX_TICK_FRAMES(srv->out_rate))
        srv->frames_done = target - MAX_TICK_FRAMES(srv->out_rate);
    size_t n = (size_t)(target - srv->frames_done);
    srv->frames_done = target;

    size_t samples = n * srv->out_channels;
    if (n > srv->mix_frames) {
        float *m = realloc(srv->mix, samples * sizeof(float));
        int16_t *p = realloc(srv->pcm, samples * sizeof(int16_t));
        if (m)
            srv->mix = m;
        if (p)
            srv->pcm = p;
        if (!m || !p)
            return 0;
        srv->mix_frames = n;
    }
    memset(srv->mix, 0, samples * sizeof(float));

    bool mixed = false;
    struct client *c, *ctmp;
    wl_list_for_each_safe(c, ctmp, &srv->clients, link) {
        struct stream *s, *stmp;
        wl_list_for_each_safe(s, stmp, &c->streams, link) {
            if (!stream_active(s))
                continue;
            bool ok = stream_mix(s, srv->mix, n, srv->out_rate, srv->out_channels);
            mixed = true;
            stream_after_consume(s, !ok);
        }
    }
    for (size_t i = 0; i < samples; i++) {
        float v = srv->mix[i] * 32767.0f;
        srv->pcm[i] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)lrintf(v);
    }
    if (mixed && srv->output)
        srv->output(srv->output_data, srv->pcm, n, srv->out_channels);

    wl_list_for_each_safe(c, ctmp, &srv->clients, link)
        if (c->closing)
            client_close(c);

    if (any_stream_active(srv)) {
        server_arm_timer(srv);
    } else {
        srv->clock_running = false;
    }
    return 0;
}

static void server_arm_timer(struct pulse_server *srv)
{
    if (srv->timer_armed)
        return;
    srv->timer_armed = true;
    wl_event_source_timer_update(srv->timer, TICK_MS);
}

/* ---- streams ---- */

static struct stream *find_stream(struct client *c, uint32_t index)
{
    struct stream *s;
    wl_list_for_each(s, &c->streams, link)
        if (s->index == index)
            return s;
    return NULL;
}

static struct stream *find_stream_by_channel(struct client *c, uint32_t channel)
{
    struct stream *s;
    wl_list_for_each(s, &c->streams, link)
        if (s->channel == channel)
            return s;
    return NULL;
}

static void stream_free(struct stream *s)
{
    wl_list_remove(&s->link);
    free(s->ring);
    free(s);
}

/* Fills in PulseAudio's defaults for -1 values and keeps the rest sane. */
static void stream_fix_attr(struct stream *s)
{
    const uint32_t max_ring = 4u * 1024 * 1024;
    size_t fs = s->frame_size;
    if (s->maxlength == UINT32_MAX || s->maxlength > max_ring)
        s->maxlength = max_ring;
    if (s->tlength == UINT32_MAX)
        s->tlength = (uint32_t)usec_to_bytes(s, 2000000);
    if (s->tlength > s->maxlength)
        s->tlength = s->maxlength;
    if (s->tlength < usec_to_bytes(s, 10000))
        s->tlength = (uint32_t)usec_to_bytes(s, 10000);
    if (s->minreq == UINT32_MAX)
        s->minreq = (uint32_t)usec_to_bytes(s, 20000);
    if (s->minreq > s->tlength / 2)
        s->minreq = s->tlength / 2;
    if (s->minreq < fs)
        s->minreq = (uint32_t)fs;
    if (s->prebuf == UINT32_MAX || s->prebuf > s->tlength - s->minreq)
        s->prebuf = s->tlength - s->minreq;
    s->maxlength -= s->maxlength % fs;
    s->tlength -= s->tlength % fs;
    s->minreq -= s->minreq % fs;
    s->prebuf -= s->prebuf % fs;
}

static bool stream_alloc_ring(struct stream *s)
{
    s->ring_cap = s->maxlength;
    s->ring = calloc(1, s->ring_cap);
    return s->ring != NULL;
}

static void stream_write(struct stream *s, int64_t offset, uint32_t seek, const uint8_t *p,
                         size_t n)
{
    /* Only appending is supported; other seek modes are treated as relative.
     * The ALSA plugin and ordinary clients always append. */
    (void)offset;
    (void)seek;
    s->requested -= (int64_t)n;
    if (s->requested < 0)
        s->requested = 0;
    s->write_index += (int64_t)n;
    size_t room = s->ring_cap - s->ring_fill;
    if (n > room) {
        PDBG("pulse: stream %u overflow, dropping %zu bytes", s->index, n - room);
        n = room;
    }
    size_t tail = (s->ring_head + s->ring_fill) % s->ring_cap;
    size_t first = n < s->ring_cap - tail ? n : s->ring_cap - tail;
    memcpy(s->ring + tail, p, first);
    memcpy(s->ring, p + first, n - first);
    s->ring_fill += n;
    stream_maybe_start(s);
}

static void stream_flush(struct stream *s)
{
    s->read_index += (int64_t)s->ring_fill;
    s->ring_fill = 0;
    s->ring_head = 0;
    s->playing = false;
    s->primed = false;
    stream_request(s);
}

/* ---- commands ---- */

static void cmd_auth(struct client *c, uint32_t tag, struct reader *r)
{
    uint32_t version = rd_u32(r), n = 0;
    rd_arbitrary(r, &n);
    if (r->bad) {
        c->closing = true;
        return;
    }
    if ((version & PROTOCOL_VERSION_MASK) >= 13)
        version &= PROTOCOL_VERSION_MASK;
    if (version < 13) {
        send_error(c, tag, ERR_VERSION);
        return;
    }
    c->version = version < PROTOCOL_VERSION ? version : PROTOCOL_VERSION;
    c->authorized = true;
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_u32(&b, c->version); /* no shared memory, no memfd */
    send_packet(c, &b);
    free(b.data);
}

static void cmd_set_client_name(struct client *c, uint32_t tag, struct reader *r)
{
    rd_proplist(r, "application.name", c->name, sizeof(c->name));
    if (r->bad) {
        c->closing = true;
        return;
    }
    PDBG("pulse: client %u is \"%s\"", c->index, c->name);
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_u32(&b, c->index);
    send_packet(c, &b);
    free(b.data);
}

static void cmd_create_playback_stream(struct client *c, uint32_t tag, struct reader *r)
{
    struct stream *s = calloc(1, sizeof(*s));
    if (!s) {
        send_error(c, tag, ERR_NOTSUPPORTED);
        return;
    }
    s->client = c;
    uint8_t map_channels, vol_channels;
    rd_sample_spec(r, &s->format, &s->channels, &s->rate);
    map_channels = rd_channel_map(r, s->map);
    uint32_t sink_index = rd_u32(r);
    const char *sink_name = rd_string(r);
    s->maxlength = rd_u32(r);
    s->corked = rd_bool(r);
    s->tlength = rd_u32(r);
    s->prebuf = rd_u32(r);
    s->minreq = rd_u32(r);
    rd_u32(r); /* sync id */
    uint32_t volume = rd_cvolume(r, &vol_channels);
    bool muted, volume_set = true;
    for (int i = 0; i < 7; i++)  /* no_remap ... variable_rate */
        rd_bool(r);
    muted = rd_bool(r);
    rd_bool(r);                  /* adjust_latency */
    rd_proplist(r, "media.name", s->name, sizeof(s->name));
    if (c->version >= 14) {
        volume_set = rd_bool(r);
        rd_bool(r);              /* early_requests */
    }
    if (c->version >= 15) {
        rd_bool(r);              /* muted_set */
        rd_bool(r);              /* dont_inhibit_auto_suspend */
        rd_bool(r);              /* fail_on_suspend */
    }
    s->muted = muted;
    if (c->version >= 17)
        rd_bool(r);              /* relative_volume */
    if (c->version >= 18)
        rd_bool(r);              /* passthrough */
    uint8_t n_formats = 0;
    bool format_ok = false;
    if (c->version >= 21) {
        n_formats = rd_u8(r);
        for (unsigned i = 0; i < n_formats && !r->bad; i++) {
            uint8_t f = s->format, ch = s->channels;
            uint32_t rate = s->rate;
            if (rd_format_info(r, &f, &ch, &rate) && !format_ok) {
                s->format = f;
                s->channels = ch;
                s->rate = rate;
                format_ok = true;
            }
        }
    }
    (void)sink_index;
    (void)sink_name;
    if (r->bad || r->pos != r->len) {
        free(s);
        c->closing = true;
        return;
    }
    if (n_formats && !format_ok) {
        free(s);
        send_error(c, tag, ERR_NOTSUPPORTED);
        return;
    }
    size_t ss = sample_size(s->format);
    if (!ss || s->channels == 0 || s->channels > CHANNELS_MAX || s->rate < 1000 ||
        s->rate > 384000) {
        PWARN("pulse: refusing a stream with format %u, %u channels, %u Hz", s->format,
              s->channels, s->rate);
        free(s);
        send_error(c, tag, n_formats ? ERR_NOTSUPPORTED : ERR_INVALID);
        return;
    }
    if (map_channels != s->channels) {
        /* Format-API streams and a few clients send no usable map. */
        for (unsigned i = 0; i < s->channels; i++)
            s->map[i] = s->channels == 1 ? 0 : (uint8_t)(i + 1);
    }
    s->frame_size = ss * s->channels;
    double g = volume_set && vol_channels ? (double)volume / VOLUME_NORM : 1.0;
    s->gain = (float)(g * g * g);
    stream_fix_attr(s);
    if (!stream_alloc_ring(s)) {
        free(s);
        send_error(c, tag, ERR_NOTSUPPORTED);
        return;
    }
    s->index = c->srv->next_index++;
    s->channel = s->index;
    wl_list_insert(c->streams.prev, &s->link);

    s->requested = s->tlength;
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_u32(&b, s->channel);
    put_u32(&b, s->index);
    put_u32(&b, s->tlength);              /* initial request */
    put_u32(&b, s->maxlength);
    put_u32(&b, s->tlength);
    put_u32(&b, s->prebuf);
    put_u32(&b, s->minreq);
    put_sample_spec(&b, s->format, s->channels, s->rate);
    put_channel_map(&b, s->channels, s->map);
    put_u32(&b, SINK_INDEX);
    put_string(&b, SINK_NAME);
    put_bool(&b, false);                  /* sink suspended */
    put_usec(&b, c->srv->out_latency_us); /* configured sink latency */
    if (c->version >= 21)
        put_pcm_format_info(&b, s->format, s->channels, s->rate);
    send_packet(c, &b);
    free(b.data);
    PDBG("pulse: stream %u created: %s %u Hz x%u, tlength %u prebuf %u minreq %u%s",
         s->index, sample_format_name(s->format), s->rate, s->channels, s->tlength,
         s->prebuf, s->minreq, s->corked ? " (corked)" : "");
}

static void cmd_get_playback_latency(struct client *c, uint32_t tag, struct reader *r)
{
    uint32_t index = rd_u32(r);
    struct timeval tv = { 0 }, now;
    rd_timeval(r, &tv);
    if (r->bad) {
        c->closing = true;
        return;
    }
    struct stream *s = find_stream(c, index);
    if (!s) {
        send_error(c, tag, ERR_NOENTITY);
        return;
    }
    gettimeofday(&now, NULL);
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_usec(&b, c->srv->out_latency_us);
    put_usec(&b, 0);
    put_bool(&b, stream_active(s) && s->playing_for > 0);
    put_timeval(&b, &tv);
    put_timeval(&b, &now);
    put_s64(&b, s->write_index);
    put_s64(&b, s->read_index);
    put_u64(&b, s->underrun_for);
    put_u64(&b, s->playing_for);
    send_packet(c, &b);
    free(b.data);
}

static void cmd_stream_simple(struct client *c, uint32_t cmd, uint32_t tag, struct reader *r)
{
    uint32_t index = rd_u32(r);
    bool flag = false;
    if (cmd == CMD_CORK_PLAYBACK_STREAM)
        flag = rd_bool(r);
    if (r->bad) {
        c->closing = true;
        return;
    }
    struct stream *s = find_stream(c, index);
    if (!s) {
        send_error(c, tag, ERR_NOENTITY);
        return;
    }
    switch (cmd) {
    case CMD_CORK_PLAYBACK_STREAM:
        s->corked = flag;
        if (!flag)
            stream_maybe_start(s);
        break;
    case CMD_FLUSH_PLAYBACK_STREAM:
        stream_flush(s);
        break;
    case CMD_TRIGGER_PLAYBACK_STREAM:
        if (!s->playing && s->ring_fill) {
            uint32_t prebuf = s->prebuf;
            s->prebuf = 0;
            stream_maybe_start(s);
            s->prebuf = prebuf;
        }
        break;
    case CMD_PREBUF_PLAYBACK_STREAM:
        s->playing = false;
        s->primed = false;
        stream_maybe_start(s);
        break;
    case CMD_DELETE_PLAYBACK_STREAM:
        stream_free(s);
        break;
    case CMD_DRAIN_PLAYBACK_STREAM:
        if (s->ring_fill == 0) {
            send_simple_reply(c, tag);
        } else {
            s->draining = true;
            s->drain_tag = tag;
            /* Play out what is left even if it is less than the prebuffer. */
            if (!s->playing) {
                uint32_t prebuf = s->prebuf;
                s->prebuf = 0;
                stream_maybe_start(s);
                s->prebuf = prebuf;
            }
        }
        return;
    }
    send_simple_reply(c, tag);
}

static void cmd_set_buffer_attr(struct client *c, uint32_t tag, struct reader *r)
{
    uint32_t index = rd_u32(r);
    struct stream *s = find_stream(c, index);
    uint32_t maxlength = rd_u32(r), tlength = rd_u32(r), prebuf = rd_u32(r), minreq = rd_u32(r);
    if (r->bad || !s) {
        if (r->bad)
            c->closing = true;
        else
            send_error(c, tag, ERR_NOENTITY);
        return;
    }
    /* The ring keeps its size; only the request behaviour changes. */
    uint32_t cap = (uint32_t)s->ring_cap;
    s->maxlength = maxlength;
    s->tlength = tlength;
    s->prebuf = prebuf;
    s->minreq = minreq;
    stream_fix_attr(s);
    if (s->maxlength > cap)
        s->maxlength = cap;
    if (s->tlength > cap)
        s->tlength = cap;
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_u32(&b, s->maxlength);
    put_u32(&b, s->tlength);
    put_u32(&b, s->prebuf);
    put_u32(&b, s->minreq);
    put_usec(&b, c->srv->out_latency_us);
    send_packet(c, &b);
    free(b.data);
    stream_request(s);
}

static void cmd_set_sink_input_volume(struct client *c, uint32_t cmd, uint32_t tag,
                                      struct reader *r)
{
    uint32_t index = rd_u32(r);
    uint8_t ch = 0;
    uint32_t volume = VOLUME_NORM;
    bool mute = false;
    if (cmd == CMD_SET_SINK_INPUT_VOLUME)
        volume = rd_cvolume(r, &ch);
    else
        mute = rd_bool(r);
    if (r->bad) {
        c->closing = true;
        return;
    }
    /* Sink inputs are numbered like our streams; look in every client. */
    struct client *o;
    wl_list_for_each(o, &c->srv->clients, link) {
        struct stream *s = find_stream(o, index);
        if (!s)
            continue;
        if (cmd == CMD_SET_SINK_INPUT_VOLUME) {
            double g = (double)volume / VOLUME_NORM;
            s->gain = (float)(g * g * g);
        } else {
            s->muted = mute;
        }
        send_simple_reply(c, tag);
        return;
    }
    send_error(c, tag, ERR_NOENTITY);
}

static void cmd_get_server_info(struct client *c, uint32_t tag)
{
    static const uint8_t stereo[2] = { 1, 2 };
    struct bytes b = { 0 };
    reply_start(&b, tag);
    put_string(&b, "pulseaudio");
    put_string(&b, "16.1");
    put_string(&b, "android");
    put_string(&b, "waydroid");
    put_sample_spec(&b, SAMPLE_S16LE, 2, c->srv->out_rate);
    put_string(&b, SINK_NAME);
    put_string(&b, NULL);
    put_u32(&b, 0x414e4c44); /* cookie */
    if (c->version >= 15)
        put_channel_map(&b, 2, stereo);
    send_packet(c, &b);
    free(b.data);
}

static void handle_control(struct client *c, const uint8_t *p, size_t len)
{
    struct reader r = { .p = p, .len = len };
    uint32_t cmd = rd_u32(&r), tag = rd_u32(&r);
    if (r.bad) {
        c->closing = true;
        return;
    }
    if (!c->authorized && cmd != CMD_AUTH) {
        send_error(c, tag, ERR_ACCESS);
        return;
    }
    switch (cmd) {
    case CMD_AUTH:
        cmd_auth(c, tag, &r);
        break;
    case CMD_SET_CLIENT_NAME:
        cmd_set_client_name(c, tag, &r);
        break;
    case CMD_CREATE_PLAYBACK_STREAM:
        cmd_create_playback_stream(c, tag, &r);
        break;
    case CMD_CREATE_RECORD_STREAM:
        PINFO("pulse: \"%s\" wants to record; there is no microphone path, refusing", c->name);
        send_error(c, tag, ERR_NOTSUPPORTED);
        break;
    case CMD_DELETE_PLAYBACK_STREAM:
    case CMD_CORK_PLAYBACK_STREAM:
    case CMD_FLUSH_PLAYBACK_STREAM:
    case CMD_TRIGGER_PLAYBACK_STREAM:
    case CMD_PREBUF_PLAYBACK_STREAM:
    case CMD_DRAIN_PLAYBACK_STREAM:
        cmd_stream_simple(c, cmd, tag, &r);
        break;
    case CMD_GET_PLAYBACK_LATENCY:
        cmd_get_playback_latency(c, tag, &r);
        break;
    case CMD_SET_PLAYBACK_STREAM_BUFFER_ATTR:
        cmd_set_buffer_attr(c, tag, &r);
        break;
    case CMD_SET_SINK_INPUT_VOLUME:
    case CMD_SET_SINK_INPUT_MUTE:
        cmd_set_sink_input_volume(c, cmd, tag, &r);
        break;
    case CMD_GET_SERVER_INFO:
        cmd_get_server_info(c, tag);
        break;
    case CMD_LOOKUP_SINK: {
        struct bytes b = { 0 };
        reply_start(&b, tag);
        put_u32(&b, SINK_INDEX);
        send_packet(c, &b);
        free(b.data);
        break;
    }
    case CMD_UPDATE_PLAYBACK_STREAM_SAMPLE_RATE: {
        uint32_t index = rd_u32(&r), rate = rd_u32(&r);
        struct stream *s = find_stream(c, index);
        if (s && !r.bad && rate >= 1000 && rate <= 384000) {
            s->rate = rate;
            send_simple_reply(c, tag);
        } else {
            send_error(c, tag, s ? ERR_INVALID : ERR_NOENTITY);
        }
        break;
    }
    /* Accepted and ignored: names, properties, subscriptions, sink volume. */
    case CMD_SET_PLAYBACK_STREAM_NAME:
    case CMD_UPDATE_PLAYBACK_STREAM_PROPLIST:
    case CMD_UPDATE_CLIENT_PROPLIST:
    case CMD_REMOVE_PLAYBACK_STREAM_PROPLIST:
    case CMD_REMOVE_CLIENT_PROPLIST:
    case CMD_SUBSCRIBE:
    case CMD_SET_SINK_VOLUME:
    case CMD_SET_SINK_MUTE:
        send_simple_reply(c, tag);
        break;
    case CMD_EXIT:
        send_error(c, tag, ERR_ACCESS);
        break;
    default:
        PDBG("pulse: unsupported command %u from \"%s\"", cmd, c->name);
        send_error(c, tag, ERR_COMMAND);
        break;
    }
}

/* ---- connections ---- */

static void client_close(struct client *c)
{
    struct stream *s, *tmp;
    wl_list_for_each_safe(s, tmp, &c->streams, link)
        stream_free(s);
    if (c->source)
        wl_event_source_remove(c->source);
    close(c->fd);
    wl_list_remove(&c->link);
    PDBG("pulse: client %u disconnected", c->index);
    free(c->in.data);
    free(c->out.data);
    free(c);
}

static void client_process(struct client *c)
{
    while (!c->closing && c->in.len >= DESCRIPTOR_SIZE) {
        const uint8_t *d = c->in.data;
        uint32_t len = (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3];
        uint32_t channel = (uint32_t)d[4] << 24 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 8 | d[7];
        uint32_t off_hi = (uint32_t)d[8] << 24 | (uint32_t)d[9] << 16 | (uint32_t)d[10] << 8 | d[11];
        uint32_t off_lo = (uint32_t)d[12] << 24 | (uint32_t)d[13] << 16 | (uint32_t)d[14] << 8 | d[15];
        uint32_t flags = (uint32_t)d[16] << 24 | (uint32_t)d[17] << 16 | (uint32_t)d[18] << 8 | d[19];
        if (len > MAX_FRAME || (flags & 0xFF000000u)) {
            /* Shared-memory references were never offered. */
            PWARN("pulse: malformed packet from \"%s\", closing", c->name);
            c->closing = true;
            return;
        }
        if (c->in.len < DESCRIPTOR_SIZE + (size_t)len)
            return;
        const uint8_t *payload = c->in.data + DESCRIPTOR_SIZE;
        if (channel == CONTROL_CHANNEL) {
            handle_control(c, payload, len);
        } else {
            struct stream *s = find_stream_by_channel(c, channel);
            if (s && len)
                stream_write(s, (int64_t)((uint64_t)off_hi << 32 | off_lo), flags & SEEK_MASK,
                             payload, len);
        }
        bytes_consume(&c->in, DESCRIPTOR_SIZE + (size_t)len);
    }
}

static int on_client(int fd, uint32_t mask, void *data)
{
    struct client *c = data;
    if (mask & WL_EVENT_WRITABLE)
        client_flush(c);
    if (mask & WL_EVENT_READABLE) {
        for (;;) {
            if (!bytes_reserve(&c->in, 65536)) {
                c->closing = true;
                break;
            }
            ssize_t n = recv(fd, c->in.data + c->in.len, c->in.cap - c->in.len, MSG_DONTWAIT);
            if (n > 0) {
                c->in.len += (size_t)n;
                client_process(c);
                if (c->closing)
                    break;
                continue;
            }
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                break;
            c->closing = true;  /* closed by the client, or an error */
            break;
        }
    }
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR))
        c->closing = true;
    if (c->closing)
        client_close(c);
    return 0;
}

static int on_listen(int fd, uint32_t mask, void *data)
{
    (void)mask;
    struct pulse_server *srv = data;
    for (;;) {
        int cfd = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (cfd < 0)
            break;
        struct client *c = calloc(1, sizeof(*c));
        if (!c) {
            close(cfd);
            continue;
        }
        c->srv = srv;
        c->fd = cfd;
        c->index = srv->next_index++;
        snprintf(c->name, sizeof(c->name), "client %u", c->index);
        wl_list_init(&c->streams);
        c->source = wl_event_loop_add_fd(srv->loop, cfd, WL_EVENT_READABLE, on_client, c);
        if (!c->source) {
            close(cfd);
            free(c);
            continue;
        }
        wl_list_insert(&srv->clients, &c->link);
        PDBG("pulse: client %u connected", c->index);
    }
    return 0;
}

static int mkdir_parents(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    char *slash = strrchr(tmp, '/');
    if (!slash || slash == tmp)
        return 0;
    *slash = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, 0755) == 0)
            chmod(tmp, 0755);   /* the umask may have taken bits away */
        *p = '/';
    }
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    /* Android's init starts services with umask 077, which would leave the
     * directory 0700 and keep the audio HAL (user audioserver) out of it.
     * The directory is ours either way, so set its mode explicitly. */
    return chmod(tmp, 0755);
}

struct pulse_server *pulse_server_new(struct wl_event_loop *loop, const char *socket_path,
                                      pulse_output_fn output, void *data)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(socket_path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    struct pulse_server *srv = calloc(1, sizeof(*srv));
    if (!srv)
        return NULL;
    srv->loop = loop;
    srv->output = output;
    srv->output_data = data;
    srv->out_rate = 48000;
    srv->out_channels = 2;
    srv->out_latency_us = 50000;
    srv->next_index = 1;
    wl_list_init(&srv->clients);
    snprintf(srv->path, sizeof(srv->path), "%s", socket_path);
    strcpy(addr.sun_path, socket_path);

    mkdir_parents(socket_path);
    /* A file left by an earlier boot (or a mount of someone else's socket). */
    umount2(socket_path, MNT_DETACH);
    unlink(socket_path);
    srv->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0 ||
        bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(srv->listen_fd, 16) < 0) {
        int e = errno;
        if (srv->listen_fd >= 0)
            close(srv->listen_fd);
        free(srv);
        errno = e;
        return NULL;
    }
    /* Android's audio server runs as its own user. */
    chmod(socket_path, 0777);
    srv->listen_source = wl_event_loop_add_fd(loop, srv->listen_fd, WL_EVENT_READABLE,
                                              on_listen, srv);
    srv->timer = wl_event_loop_add_timer(loop, on_timer, srv);
    return srv;
}

void pulse_server_set_output_format(struct pulse_server *srv, uint32_t rate, uint32_t channels)
{
    if (!srv || rate < 8000 || rate > 384000 || channels < 1 || channels > 2)
        return;
    if (rate == srv->out_rate && channels == srv->out_channels)
        return;
    srv->out_rate = rate;
    srv->out_channels = channels;
    srv->clock_running = false; /* restart the clock at the new rate */
}

void pulse_server_set_output_latency_us(struct pulse_server *srv, uint64_t usec)
{
    if (srv)
        srv->out_latency_us = usec;
}

void pulse_server_destroy(struct pulse_server *srv)
{
    if (!srv)
        return;
    struct client *c, *tmp;
    wl_list_for_each_safe(c, tmp, &srv->clients, link)
        client_close(c);
    if (srv->listen_source)
        wl_event_source_remove(srv->listen_source);
    if (srv->timer)
        wl_event_source_remove(srv->timer);
    close(srv->listen_fd);
    unlink(srv->path);
    free(srv->mix);
    free(srv->pcm);
    free(srv);
}
