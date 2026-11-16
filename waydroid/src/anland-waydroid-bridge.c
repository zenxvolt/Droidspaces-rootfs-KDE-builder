/*
 * anland-waydroid-bridge
 *
 * A very small Wayland compositor whose only job is to sit between the Waydroid
 * hardware composer (a Wayland client) and the Anland display daemon (which
 * expects a "producer" that draws into buffers owned by the Android app).
 *
 *   Android (SurfaceFlinger) -> hwcomposer.waydroid -> [this bridge] -> Anland
 *
 * It accepts exactly what the Waydroid HWC needs in its simplest mode
 * (wl_compositor, wl_shm, xdg_wm_base, wl_seat, wl_output), copies every
 * committed frame into the buffer the Anland app selected, and turns Anland
 * touch / key events back into wl_touch / wl_keyboard events.
 *
 * Rendering is a CPU copy on purpose: it needs no GL driver, so the binary can
 * be fully static and run inside the Android rootfs before anything else is up.
 *
 * The file itself is MIT licensed like the rest of this repository. The binary
 * links Anland's libdisplay_producer (GPL-3.0), so the resulting executable is
 * distributed under GPL-3.0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-server-protocol.h"
#include "display_producer.h"

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
struct bridge_dma_buf_sync { uint64_t flags; };
#define BRIDGE_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct bridge_dma_buf_sync)

/* AHardwareBuffer formats the Anland app hands out. */
#define AHB_FORMAT_RGBA_8888    1
#define AHB_FORMAT_RGBX_8888    2
#define AHB_FORMAT_RGBA_1010102 43

#define EMULATED_TOUCH_ID 31
#define BTN_LEFT_CODE 0x110

static FILE *g_log_file;
static int g_verbose;

static void logmsg(const char *level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "anland-bridge[%s] %s\n", level, line);
    if (g_log_file) {
        fprintf(g_log_file, "[%6ld.%03ld] %s: %s\n", (long)ts.tv_sec,
                ts.tv_nsec / 1000000, level, line);
        fflush(g_log_file);
    }
}

#define INFO(...) logmsg("I", __VA_ARGS__)
#define WARN(...) logmsg("W", __VA_ARGS__)
#define DBG(...)  do { if (g_verbose) logmsg("D", __VA_ARGS__); } while (0)

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

static uint64_t now_ms64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000ull + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------------- */
/* State                                                                     */
/* ------------------------------------------------------------------------- */

struct config {
    char anland_socket[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char display_name[64];
    char log_path[PATH_MAX];
    int default_width, default_height;
    int render_scale;        /* percent of the real screen Android renders at */
    int max_fps;
    int wait_for_display_ms; /* hold the first configure this long for a real size */
    int ui_width_dp;         /* pick the Android density so the short side is this many dp */
};

struct target {
    void *map;
    size_t len;
    int fd;
    struct buf_info info;
};

struct bridge;

struct surface {
    struct bridge *bridge;
    struct wl_resource *resource;
    struct wl_resource *xdg_surface;
    struct wl_resource *toplevel;
    struct wl_resource *buffer;          /* currently committed buffer */
    struct wl_listener buffer_destroy;
    struct wl_resource *pending_buffer;
    struct wl_listener pending_destroy;
    bool pending_attach;
    bool configure_sent;
    struct wl_list frame_callbacks;      /* wl_resource link */
    struct wl_list pending_callbacks;
};

struct bridge {
    struct config cfg;
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_event_source *tick;

    /* Anland side */
    display_ctx *ctx;
    bool session;
    struct target targets[MAX_BUFS];
    int target_count;
    struct wl_event_source *ready_source, *data_source;
    bool target_ready;
    bool dirty;
    uint64_t next_connect_ms;
    bool logged_no_daemon;

    /* Geometry */
    uint32_t screen_w, screen_h;   /* real Anland buffer size, 0 until known */
    uint32_t client_w, client_h;   /* size announced to Android */
    uint32_t refresh_mhz;
    bool size_from_display;
    uint64_t start_ms;

    /* Wayland side */
    struct wl_list outputs;        /* wl_output resources */
    struct wl_list keyboards;
    struct wl_list touches;
    struct surface *top;

    /* Scaling scratch */
    uint32_t *xmap;
    uint32_t xmap_src_w, xmap_dst_w;

    /* Pointer -> touch emulation */
    float pointer_x, pointer_y;
    bool pointer_down;

    /* Stats */
    uint64_t frames, last_stats_ms, last_frames;
};

static void maybe_present(struct bridge *b);
static void send_toplevel_configure(struct surface *s);

/* ------------------------------------------------------------------------- */
/* Pixel copy                                                                */
/* ------------------------------------------------------------------------- */

/* All four accepted wl_shm formats are 32 bit little-endian words. Convert one
 * word to the destination layout. Alpha is forced opaque: this is a screen. */
static inline uint32_t to_rgba8888(uint32_t s, bool src_bgr_order)
{
    if (src_bgr_order) /* ABGR / XBGR: already R,G,B,A in memory */
        return s | 0xFF000000u;
    /* ARGB / XRGB: B,G,R,A in memory -> swap red and blue */
    return (s & 0x0000FF00u) | ((s >> 16) & 0xFFu) | ((s & 0xFFu) << 16) | 0xFF000000u;
}

static inline uint32_t to_rgba1010102(uint32_t s, bool src_bgr_order)
{
    uint32_t r, g, bl;
    if (src_bgr_order) {
        r = s & 0xFF; g = (s >> 8) & 0xFF; bl = (s >> 16) & 0xFF;
    } else {
        bl = s & 0xFF; g = (s >> 8) & 0xFF; r = (s >> 16) & 0xFF;
    }
    r = (r << 2) | (r >> 6);
    g = (g << 2) | (g >> 6);
    bl = (bl << 2) | (bl >> 6);
    return r | (g << 10) | (bl << 20) | (3u << 30);
}

static void convert_row(uint32_t *dst, const uint32_t *src, uint32_t n,
                        bool src_bgr_order, bool dst_10bit)
{
    if (dst_10bit) {
        for (uint32_t x = 0; x < n; x++)
            dst[x] = to_rgba1010102(src[x], src_bgr_order);
    } else if (src_bgr_order) {
        for (uint32_t x = 0; x < n; x++)
            dst[x] = src[x] | 0xFF000000u;
    } else {
        for (uint32_t x = 0; x < n; x++)
            dst[x] = to_rgba8888(src[x], false);
    }
}

static void convert_row_scaled(uint32_t *dst, const uint32_t *src, uint32_t n,
                               const uint32_t *xmap, bool src_bgr_order, bool dst_10bit)
{
    if (dst_10bit) {
        for (uint32_t x = 0; x < n; x++)
            dst[x] = to_rgba1010102(src[xmap[x]], src_bgr_order);
    } else {
        for (uint32_t x = 0; x < n; x++)
            dst[x] = to_rgba8888(src[xmap[x]], src_bgr_order);
    }
}

/* Returns false when the destination layout is one we cannot write. */
static bool copy_frame(struct bridge *b, struct wl_shm_buffer *shm, struct target *t)
{
    const uint32_t sw = (uint32_t)wl_shm_buffer_get_width(shm);
    const uint32_t sh = (uint32_t)wl_shm_buffer_get_height(shm);
    const uint32_t sstride = (uint32_t)wl_shm_buffer_get_stride(shm);
    const uint32_t fmt = wl_shm_buffer_get_format(shm);
    const uint32_t dw = t->info.width, dh = t->info.height, dstride = t->info.stride;

    bool src_bgr_order;
    switch (fmt) {
    case WL_SHM_FORMAT_ARGB8888:
    case WL_SHM_FORMAT_XRGB8888: src_bgr_order = false; break;
    case WL_SHM_FORMAT_ABGR8888:
    case WL_SHM_FORMAT_XBGR8888: src_bgr_order = true; break;
    default: return false;
    }

    bool dst_10bit;
    switch (t->info.format) {
    case AHB_FORMAT_RGBA_8888:
    case AHB_FORMAT_RGBX_8888: dst_10bit = false; break;
    case AHB_FORMAT_RGBA_1010102: dst_10bit = true; break;
    default: return false;
    }

    if (!sw || !sh || !dw || !dh || sstride < sw * 4 || dstride < dw * 4)
        return false;

    struct bridge_dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE };
    ioctl(t->fd, BRIDGE_DMA_BUF_IOCTL_SYNC, &sync);

    wl_shm_buffer_begin_access(shm);
    const uint8_t *src = wl_shm_buffer_get_data(shm);
    uint8_t *dst = (uint8_t *)t->map + t->info.offset;

    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; y++)
            convert_row((uint32_t *)(dst + (size_t)y * dstride),
                        (const uint32_t *)(src + (size_t)y * sstride),
                        dw, src_bgr_order, dst_10bit);
    } else {
        if (!b->xmap || b->xmap_src_w != sw || b->xmap_dst_w != dw) {
            free(b->xmap);
            b->xmap = malloc(sizeof(uint32_t) * dw);
            if (b->xmap) {
                for (uint32_t x = 0; x < dw; x++)
                    b->xmap[x] = (uint32_t)(((uint64_t)x * sw) / dw);
                b->xmap_src_w = sw;
                b->xmap_dst_w = dw;
            }
        }
        if (b->xmap) {
            uint32_t prev_sy = UINT32_MAX;
            for (uint32_t y = 0; y < dh; y++) {
                uint32_t sy = (uint32_t)(((uint64_t)y * sh) / dh);
                uint32_t *drow = (uint32_t *)(dst + (size_t)y * dstride);
                if (sy == prev_sy) {
                    memcpy(drow, dst + (size_t)(y - 1) * dstride, (size_t)dw * 4);
                } else {
                    convert_row_scaled(drow, (const uint32_t *)(src + (size_t)sy * sstride),
                                       dw, b->xmap, src_bgr_order, dst_10bit);
                    prev_sy = sy;
                }
            }
        }
    }
    wl_shm_buffer_end_access(shm);

    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    ioctl(t->fd, BRIDGE_DMA_BUF_IOCTL_SYNC, &sync);
    return true;
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

static uint32_t scaled_dim(uint32_t v, int percent)
{
    uint32_t r = (uint32_t)(((uint64_t)v * (uint32_t)percent + 50) / 100);
    r &= ~1u; /* keep it even, some Android paths dislike odd sizes */
    return r < 64 ? 64 : r;
}

static uint32_t advertised_refresh(struct bridge *b)
{
    uint32_t cap = (uint32_t)b->cfg.max_fps * 1000u;
    uint32_t r = b->refresh_mhz;
    if (r < 1000 || r > 1000000)
        r = 60000;
    return r > cap ? cap : r;
}

static void send_output_state(struct bridge *b, struct wl_resource *out)
{
    wl_output_send_geometry(out, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "Anland", "Droidspaces", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(out, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        (int32_t)b->client_w, (int32_t)b->client_h,
                        (int32_t)advertised_refresh(b));
    if (wl_resource_get_version(out) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(out, 1);
    if (wl_resource_get_version(out) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(out);
}

/* Called whenever the size Android should render at may have changed. */
static void set_client_size(struct bridge *b, uint32_t w, uint32_t h, bool from_display)
{
    if (from_display)
        b->size_from_display = true;
    if (w == b->client_w && h == b->client_h)
        return;
    b->client_w = w;
    b->client_h = h;
    INFO("Android display size is now %ux%u (%s)", w, h,
         from_display ? "from the Anland app" : "default, no Anland app yet");

    struct wl_resource *out;
    wl_resource_for_each(out, &b->outputs)
        send_output_state(b, out);
    if (b->top && b->top->configure_sent)
        send_toplevel_configure(b->top);
}

/* ------------------------------------------------------------------------- */
/* Anland session                                                            */
/* ------------------------------------------------------------------------- */

static void session_teardown(struct bridge *b)
{
    if (b->ready_source) { wl_event_source_remove(b->ready_source); b->ready_source = NULL; }
    if (b->data_source) { wl_event_source_remove(b->data_source); b->data_source = NULL; }
    for (int i = 0; i < b->target_count; i++) {
        if (b->targets[i].map && b->targets[i].map != MAP_FAILED)
            munmap(b->targets[i].map, b->targets[i].len);
        b->targets[i].map = NULL;
    }
    b->target_count = 0;
    b->target_ready = false;
    if (b->session)
        INFO("Anland app disconnected; Android keeps running, waiting for it to come back");
    b->session = false;
}

/* libdisplay_producer calls this right before it closes the consumer fds. */
static void on_pre_release(void *data)
{
    session_teardown(data);
}

static void touch_send_all(struct bridge *b, int action, int id, float x, float y);
static void touch_frame_all(struct bridge *b);
static void key_send_all(struct bridge *b, uint32_t key, bool pressed);

static void drain_payload(struct bridge *b, uint32_t size)
{
    if (size == 0)
        return;
    if (size > (4u << 20)) { /* never trust a huge length */
        force_fallback(b->ctx);
        return;
    }
    void *buf = malloc(size);
    if (!buf) { force_fallback(b->ctx); return; }
    uint64_t deadline = now_ms64() + 3000;
    for (;;) {
        int rc = poll_input_event_extend_data(b->ctx, buf, size, 200);
        if (rc != 0 || now_ms64() > deadline || !b->session)
            break;
    }
    free(buf);
}

static void handle_input_event(struct bridge *b, const struct InputEvent *ev)
{
    switch (ev->type) {
    case INPUT_TYPE_TOUCH:
        touch_send_all(b, ev->touch.action, ev->touch.pointer_id, ev->touch.x, ev->touch.y);
        break;
    case INPUT_TYPE_TOUCH_FRAME:
        touch_frame_all(b);
        break;
    case INPUT_TYPE_KEY:
        key_send_all(b, (uint32_t)ev->key.keycode, ev->key.action == INPUT_ACTION_DOWN);
        break;
    case INPUT_TYPE_POINTER_MOTION:
        b->pointer_x = ev->pointer_motion.x;
        b->pointer_y = ev->pointer_motion.y;
        if (b->pointer_down) {
            touch_send_all(b, INPUT_ACTION_MOVE, EMULATED_TOUCH_ID, b->pointer_x, b->pointer_y);
            touch_frame_all(b);
        }
        break;
    case INPUT_TYPE_POINTER_BUTTON:
        /* A mouse click is delivered to Android as a finger tap. */
        if (ev->pointer_button.button == BTN_LEFT_CODE || ev->pointer_button.button == 0 ||
            ev->pointer_button.button == 1) {
            bool down = ev->pointer_button.pressed != 0;
            if (down != b->pointer_down) {
                b->pointer_down = down;
                touch_send_all(b, down ? INPUT_ACTION_DOWN : INPUT_ACTION_UP,
                               EMULATED_TOUCH_ID, b->pointer_x, b->pointer_y);
                touch_frame_all(b);
            }
        }
        break;
    case INPUT_TYPE_DISPLAY_REFRESH:
        if (ev->display.refresh_mhz >= 1000 && ev->display.refresh_mhz <= 1000000 &&
            ev->display.refresh_mhz != b->refresh_mhz) {
            b->refresh_mhz = ev->display.refresh_mhz;
            DBG("Anland refresh rate: %u mHz", b->refresh_mhz);
        }
        break;
    case INPUT_TYPE_CLIPBOARD:
        drain_payload(b, ev->clipboard.size);
        break;
    case INPUT_TYPE_TEXT_INPUT:
        drain_payload(b, ev->text_input.size);
        break;
    case INPUT_TYPE_RESOURCE: {
        int fds[64], count = 0;
        if (poll_input_event_extend_fds(b->ctx, fds, 64, &count, 500) == 1)
            for (int i = 0; i < count; i++)
                close(fds[i]);
        break;
    }
    default:
        break; /* fixed-size events we do not use (tablet, actions, ...) */
    }
}

static int on_data(int fd, uint32_t mask, void *data)
{
    struct bridge *b = data;
    (void)fd;
    if (!b->session)
        return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        poll_buffer_ready(b->ctx, 0); /* notices the loss and tears the session down */
        if (b->session)
            force_fallback(b->ctx);
        return 0;
    }
    for (int i = 0; i < 256 && b->session; i++) {
        struct InputEvent ev;
        int rc = poll_input_event(b->ctx, &ev, 0);
        if (rc <= 0)
            break;
        handle_input_event(b, &ev);
    }
    return 0;
}

static int on_buffer_ready(int fd, uint32_t mask, void *data)
{
    struct bridge *b = data;
    if (!b->session)
        return 0;
    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        poll_buffer_ready(b->ctx, 0);
        return 0;
    }
    uint64_t value;
    if (read(fd, &value, sizeof(value)) < 0 && errno != EAGAIN && errno != EINTR)
        DBG("buffer-ready read: %s", strerror(errno));
    b->target_ready = true;
    maybe_present(b);
    return 0;
}

static bool session_start(struct bridge *b)
{
    int count = get_buf_count(b->ctx);
    if (count <= 0 || count > MAX_BUFS) {
        WARN("Anland app offered %d buffers, cannot use that", count);
        return false;
    }
    for (int i = 0; i < count; i++) {
        struct target *t = &b->targets[i];
        memset(t, 0, sizeof(*t));
        t->fd = get_dmabuf_fd_at(b->ctx, i);
        if (t->fd < 0 || get_dmabuf_info_at(b->ctx, i, &t->info) < 0) {
            WARN("Anland buffer %d has no descriptor", i);
            b->target_count = i;
            return false;
        }
        t->len = (size_t)t->info.offset + (size_t)t->info.stride * t->info.height;
        t->map = mmap(NULL, t->len, PROT_READ | PROT_WRITE, MAP_SHARED, t->fd, 0);
        if (t->map == MAP_FAILED) {
            WARN("cannot map Anland buffer %d (%ux%u stride %u format %u): %s. "
                 "This device's display buffers cannot be written from the CPU.",
                 i, t->info.width, t->info.height, t->info.stride, t->info.format,
                 strerror(errno));
            t->map = NULL;
            b->target_count = i;
            return false;
        }
        b->target_count = i + 1;
    }

    const struct buf_info *info = &b->targets[0].info;
    if (info->format != AHB_FORMAT_RGBA_8888 && info->format != AHB_FORMAT_RGBX_8888 &&
        info->format != AHB_FORMAT_RGBA_1010102)
        WARN("unexpected Anland pixel format %u, the picture may be wrong", info->format);

    uint32_t w = 0, h = 0, fmt = 0, refresh = 0;
    get_screen_info(b->ctx, &w, &h, &fmt, &refresh);
    if (refresh >= 1000 && refresh <= 1000000)
        b->refresh_mhz = refresh;

    b->screen_w = info->width;
    b->screen_h = info->height;
    b->session = true;
    b->target_ready = false;
    b->dirty = true; /* repaint the last Android frame into the new buffers */

    INFO("Anland app connected: %d buffers of %ux%u, stride %u, format %u, %u mHz",
         count, info->width, info->height, info->stride, info->format, b->refresh_mhz);

    b->ready_source = wl_event_loop_add_fd(b->loop, get_buffer_ready_fd(b->ctx),
                                           WL_EVENT_READABLE, on_buffer_ready, b);
    b->data_source = wl_event_loop_add_fd(b->loop, get_data_fd(b->ctx),
                                          WL_EVENT_READABLE, on_data, b);

    set_client_size(b, scaled_dim(b->screen_w, b->cfg.render_scale),
                    scaled_dim(b->screen_h, b->cfg.render_scale), true);
    return true;
}

static void anland_poll(struct bridge *b)
{
    uint64_t now = now_ms64();

    if (!b->ctx) {
        if (now < b->next_connect_ms)
            return;
        b->next_connect_ms = now + 500;
        if (connect_to_deamon_deferred(&b->ctx, b->cfg.anland_socket) != 0) {
            b->ctx = NULL;
            if (!b->logged_no_daemon) {
                WARN("cannot reach the Anland daemon at %s (%s). Enable \"Anland Display\" "
                     "for this container in Droidspaces. Retrying quietly.",
                     b->cfg.anland_socket, strerror(errno));
                b->logged_no_daemon = true;
            }
            return;
        }
        b->logged_no_daemon = false;
        INFO("connected to the Anland daemon at %s, waiting for the Anland app",
             b->cfg.anland_socket);
        set_pre_release_callback(b->ctx, on_pre_release, b);
    }

    if (is_fallback(b->ctx)) {
        if (b->session)
            session_teardown(b);
        if (!is_daemon_alive(b->ctx)) {
            INFO("Anland daemon went away, reconnecting");
            disconnect(b->ctx);
            b->ctx = NULL;
            return;
        }
        if (try_exit_fallback(b->ctx) == 0) {
            if (!session_start(b)) {
                session_teardown(b);
                force_fallback(b->ctx);
            }
        } else if (errno != EAGAIN) {
            disconnect(b->ctx);
            b->ctx = NULL;
        }
        return;
    }

    /* Live session: cheap, non-consuming check that the app is still there. */
    poll_buffer_ready(b->ctx, 0);
}

/* ------------------------------------------------------------------------- */
/* Presenting                                                                */
/* ------------------------------------------------------------------------- */

static void maybe_present(struct bridge *b)
{
    if (!b->session || !b->target_ready || !b->dirty)
        return;
    struct surface *s = b->top;
    if (!s || !s->buffer)
        return;
    struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
    if (!shm)
        return;

    int idx = get_selected_idx_raw(b->ctx);
    if (idx < 0 || idx >= b->target_count) {
        DBG("Anland selected buffer %d which we do not have", idx);
        return;
    }
    if (!copy_frame(b, shm, &b->targets[idx])) {
        WARN("cannot convert a %dx%d frame (shm format 0x%x) into Anland format %u",
             wl_shm_buffer_get_width(shm), wl_shm_buffer_get_height(shm),
             wl_shm_buffer_get_format(shm), b->targets[idx].info.format);
        b->dirty = false;
        return;
    }
    b->target_ready = false;
    b->dirty = false;
    b->frames++;
    trigger_refresh(b->ctx); /* on failure the library drops the session itself */
}

/* ------------------------------------------------------------------------- */
/* Generic "accept and ignore" dispatcher                                    */
/* ------------------------------------------------------------------------- */

/* Used for protocol objects we must accept but never act on (positioners,
 * popups, pointers, toplevel requests, ...). It honours destructors and creates
 * inert children for any new object the client asks for, so the object IDs on
 * both sides stay in step. */
static int inert_dispatch(const void *impl, void *target, uint32_t opcode,
                          const struct wl_message *msg, union wl_argument *args)
{
    struct wl_resource *resource = target;
    (void)impl; (void)opcode;

    const char *sig = msg->signature;
    int arg = 0;
    for (; *sig; sig++) {
        if (*sig == '?' || (*sig >= '0' && *sig <= '9'))
            continue;
        if (*sig == 'n' && msg->types && msg->types[arg]) {
            struct wl_resource *child = wl_resource_create(
                wl_resource_get_client(resource), msg->types[arg],
                wl_resource_get_version(resource), args[arg].n);
            if (child)
                wl_resource_set_dispatcher(child, inert_dispatch, NULL, NULL, NULL);
        }
        arg++;
    }
    if (!strcmp(msg->name, "destroy") || !strcmp(msg->name, "release"))
        wl_resource_destroy(resource);
    return 0;
}

static void unlink_resource(struct wl_resource *resource)
{
    wl_list_remove(wl_resource_get_link(resource));
}

/* ------------------------------------------------------------------------- */
/* wl_surface                                                                */
/* ------------------------------------------------------------------------- */

static void surface_set_buffer(struct surface *s, struct wl_resource *buffer)
{
    if (s->buffer == buffer)
        return;
    if (s->buffer) {
        wl_list_remove(&s->buffer_destroy.link);
        wl_buffer_send_release(s->buffer);
    }
    s->buffer = buffer;
    if (buffer)
        wl_resource_add_destroy_listener(buffer, &s->buffer_destroy);
}

static void on_buffer_destroy(struct wl_listener *l, void *data)
{
    struct surface *s = wl_container_of(l, s, buffer_destroy);
    (void)data;
    wl_list_remove(&s->buffer_destroy.link);
    s->buffer = NULL;
}

static void on_pending_destroy(struct wl_listener *l, void *data)
{
    struct surface *s = wl_container_of(l, s, pending_destroy);
    (void)data;
    wl_list_remove(&s->pending_destroy.link);
    s->pending_buffer = NULL;
}

static void surface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buffer, int32_t x, int32_t y)
{
    struct surface *s = wl_resource_get_user_data(r);
    (void)c; (void)x; (void)y;
    if (s->pending_buffer)
        wl_list_remove(&s->pending_destroy.link);
    s->pending_buffer = buffer;
    s->pending_attach = true;
    if (buffer)
        wl_resource_add_destroy_listener(buffer, &s->pending_destroy);
}

static void surface_damage(struct wl_client *c, struct wl_resource *r,
                           int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (!cb) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(cb, NULL, NULL, unlink_resource);
    wl_list_insert(s->pending_callbacks.prev, wl_resource_get_link(cb));
}

static void surface_set_region(struct wl_client *c, struct wl_resource *r,
                               struct wl_resource *region)
{
    (void)c; (void)r; (void)region;
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct bridge *b = s->bridge;
    (void)c;

    wl_list_insert_list(s->frame_callbacks.prev, &s->pending_callbacks);
    wl_list_init(&s->pending_callbacks);

    if (s->toplevel && !s->configure_sent) {
        /* First commit of the window: tell Android how big the screen is. If
         * the Anland app has not shown up yet we give it a moment so Android
         * starts at the right size straight away. */
        if (b->size_from_display ||
            now_ms64() - b->start_ms >= (uint64_t)b->cfg.wait_for_display_ms)
            send_toplevel_configure(s);
    }

    if (!s->pending_attach)
        return;
    s->pending_attach = false;
    struct wl_resource *buffer = s->pending_buffer;
    if (buffer) {
        wl_list_remove(&s->pending_destroy.link);
        s->pending_buffer = NULL;
    }

    if (s != b->top) {
        /* Cursor or helper surface: accept and hand the buffer straight back. */
        if (buffer)
            wl_buffer_send_release(buffer);
        return;
    }

    surface_set_buffer(s, buffer);
    if (!buffer)
        return;
    struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
    if (!shm) {
        WARN("Android sent a buffer that is not shared memory; only software "
             "rendering (ro.hardware.gralloc=default) is supported by this bridge");
        return;
    }
    if (wl_shm_buffer_get_width(shm) < 8 || wl_shm_buffer_get_height(shm) < 8)
        return; /* the 1x1 probe buffer the HWC uses while it measures the window */
    b->dirty = true;
    maybe_present(b);
}

static void surface_set_buffer_transform(struct wl_client *c, struct wl_resource *r, int32_t t)
{
    (void)c; (void)r; (void)t;
}

static void surface_set_buffer_scale(struct wl_client *c, struct wl_resource *r, int32_t sc)
{
    (void)c; (void)r; (void)sc;
}

static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
    (void)c; (void)r; (void)x; (void)y;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_request,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_region,
    .set_input_region = surface_set_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage,
    .offset = surface_offset,
};

static void destroy_callback_list(struct wl_list *list)
{
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, list)
        wl_resource_destroy(cb);
}

static void surface_resource_destroy(struct wl_resource *r)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct bridge *b = s->bridge;
    if (s->buffer)
        wl_list_remove(&s->buffer_destroy.link);
    if (s->pending_buffer)
        wl_list_remove(&s->pending_destroy.link);
    destroy_callback_list(&s->frame_callbacks);
    destroy_callback_list(&s->pending_callbacks);
    if (s->xdg_surface)
        wl_resource_set_user_data(s->xdg_surface, NULL);
    if (s->toplevel)
        wl_resource_set_user_data(s->toplevel, NULL);
    if (b->top == s) {
        b->top = NULL;
        b->pointer_down = false;
        INFO("Android closed its display window");
    }
    free(s);
}

static void region_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void region_rect(struct wl_client *c, struct wl_resource *r,
                        int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy,
    .add = region_rect,
    .subtract = region_rect,
};

static void compositor_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct bridge *b = wl_resource_get_user_data(r);
    struct surface *s = calloc(1, sizeof(*s));
    if (!s) { wl_client_post_no_memory(c); return; }
    s->bridge = b;
    s->buffer_destroy.notify = on_buffer_destroy;
    s->pending_destroy.notify = on_pending_destroy;
    wl_list_init(&s->frame_callbacks);
    wl_list_init(&s->pending_callbacks);
    s->resource = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    if (!s->resource) { free(s); wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(s->resource, &surface_impl, s, surface_resource_destroy);
}

static void compositor_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *region = wl_resource_create(c, &wl_region_interface, 1, id);
    (void)r;
    if (!region) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(region, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void bind_compositor(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &compositor_impl, data, NULL);
}

/* ------------------------------------------------------------------------- */
/* xdg-shell                                                                 */
/* ------------------------------------------------------------------------- */

static void keyboard_enter_all(struct bridge *b);

/* Minimal client for Android's property service (the same wire format
 * `setprop` uses). Only works while running inside Android; elsewhere the
 * socket does not exist and this quietly does nothing. */
static bool android_setprop(const char *name, const char *value)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "/dev/socket/property_service");
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool ok = false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        uint8_t msg[512];
        uint32_t cmd = 0x00020001; /* PROP_MSG_SETPROP2 */
        uint32_t nlen = (uint32_t)strlen(name), vlen = (uint32_t)strlen(value);
        size_t off = 0;
        if (nlen + vlen + 12 <= sizeof(msg)) {
            memcpy(msg + off, &cmd, 4); off += 4;
            memcpy(msg + off, &nlen, 4); off += 4;
            memcpy(msg + off, name, nlen); off += nlen;
            memcpy(msg + off, &vlen, 4); off += 4;
            memcpy(msg + off, value, vlen); off += vlen;
            int32_t result = -1;
            if (send(fd, msg, off, MSG_NOSIGNAL) == (ssize_t)off &&
                recv(fd, &result, sizeof(result), MSG_WAITALL) == (ssize_t)sizeof(result))
                ok = result == 0;
        }
    }
    close(fd);
    return ok;
}

/* Android reads its screen density once, when SurfaceFlinger starts, which is
 * right after the composer learns the window size from us. Choosing it here
 * keeps the interface the same physical size whatever resolution (or
 * render_scale) is in use. A density set in the config file wins, because a
 * read-only property can only be set once. */
static void choose_density(struct bridge *b)
{
    if (b->cfg.ui_width_dp <= 0)
        return;
    uint32_t shorter = b->client_w < b->client_h ? b->client_w : b->client_h;
    int density = (int)((shorter * 160u + (uint32_t)b->cfg.ui_width_dp / 2) /
                        (uint32_t)b->cfg.ui_width_dp);
    if (density < 120) density = 120;
    if (density > 640) density = 640;
    char value[16];
    snprintf(value, sizeof(value), "%d", density);
    if (android_setprop("ro.sf.lcd_density", value))
        INFO("Android screen density set to %d dpi", density);
    else
        DBG("density %d not applied (already set in the config, or not inside Android)", density);
}

static void send_toplevel_configure(struct surface *s)
{
    struct bridge *b = s->bridge;
    if (!s->toplevel || !s->xdg_surface)
        return;
    if (!s->configure_sent)
        choose_density(b);
    struct wl_array states;
    wl_array_init(&states);
    uint32_t *st = wl_array_add(&states, sizeof(uint32_t) * 2);
    if (st) {
        st[0] = XDG_TOPLEVEL_STATE_MAXIMIZED;
        st[1] = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    xdg_toplevel_send_configure(s->toplevel, (int32_t)b->client_w, (int32_t)b->client_h, &states);
    wl_array_release(&states);
    xdg_surface_send_configure(s->xdg_surface, wl_display_next_serial(b->display));
    if (!s->configure_sent) {
        s->configure_sent = true;
        keyboard_enter_all(b);
    }
}

static void toplevel_resource_destroy(struct wl_resource *r)
{
    struct surface *s = wl_resource_get_user_data(r);
    if (!s)
        return;
    s->toplevel = NULL;
    s->configure_sent = false;
    if (s->bridge->top == s)
        s->bridge->top = NULL;
}

static void xdg_surface_destroy_request(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void xdg_surface_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct surface *s = wl_resource_get_user_data(r);
    struct wl_resource *top = wl_resource_create(c, &xdg_toplevel_interface,
                                                 wl_resource_get_version(r), id);
    if (!top) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(top, inert_dispatch, NULL, s, toplevel_resource_destroy);
    if (!s)
        return;
    s->toplevel = top;
    s->configure_sent = false;
    if (!s->bridge->top) {
        s->bridge->top = s;
        INFO("Android opened its display window");
    } else if (s->bridge->top != s) {
        DBG("extra toplevel ignored (only the first window is shown)");
    }
}

static void xdg_surface_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                                  struct wl_resource *parent, struct wl_resource *positioner)
{
    struct wl_resource *popup = wl_resource_create(c, &xdg_popup_interface,
                                                   wl_resource_get_version(r), id);
    (void)parent; (void)positioner;
    if (!popup) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(popup, inert_dispatch, NULL, NULL, NULL);
}

static void xdg_surface_set_window_geometry(struct wl_client *c, struct wl_resource *r,
                                            int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)r; (void)x; (void)y; (void)w; (void)h;
}

static void xdg_surface_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{
    (void)c; (void)r; (void)serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy_request,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_resource_destroy(struct wl_resource *r)
{
    struct surface *s = wl_resource_get_user_data(r);
    if (s)
        s->xdg_surface = NULL;
}

static void wm_base_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void wm_base_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *p = wl_resource_create(c, &xdg_positioner_interface,
                                               wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(p, inert_dispatch, NULL, NULL, NULL);
}

static void wm_base_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id,
                                    struct wl_resource *surface_res)
{
    struct surface *s = wl_resource_get_user_data(surface_res);
    struct wl_resource *xs = wl_resource_create(c, &xdg_surface_interface,
                                                wl_resource_get_version(r), id);
    if (!xs) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(xs, &xdg_surface_impl, s, xdg_surface_resource_destroy);
    s->xdg_surface = xs;
}

static void wm_base_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{
    (void)c; (void)r; (void)serial;
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void bind_wm_base(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &wm_base_impl, data, NULL);
}

/* ------------------------------------------------------------------------- */
/* wl_output                                                                 */
/* ------------------------------------------------------------------------- */

static void bind_output(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    struct bridge *b = data;
    struct wl_resource *r = wl_resource_create(c, &wl_output_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(r, inert_dispatch, NULL, b, unlink_resource);
    wl_list_insert(&b->outputs, wl_resource_get_link(r));
    send_output_state(b, r);
}

/* ------------------------------------------------------------------------- */
/* wl_seat                                                                   */
/* ------------------------------------------------------------------------- */

static void keyboard_enter_one(struct bridge *b, struct wl_resource *kbd)
{
    struct surface *s = b->top;
    if (!s || !s->configure_sent)
        return;
    if (wl_resource_get_client(kbd) != wl_resource_get_client(s->resource))
        return;
    struct wl_array keys;
    wl_array_init(&keys);
    wl_keyboard_send_enter(kbd, wl_display_next_serial(b->display), s->resource, &keys);
    wl_array_release(&keys);
    wl_keyboard_send_modifiers(kbd, wl_display_next_serial(b->display), 0, 0, 0, 0);
}

static void keyboard_enter_all(struct bridge *b)
{
    struct wl_resource *kbd;
    wl_resource_for_each(kbd, &b->keyboards)
        keyboard_enter_one(b, kbd);
}

static void key_send_all(struct bridge *b, uint32_t key, bool pressed)
{
    struct wl_resource *kbd;
    uint32_t t = now_ms();
    wl_resource_for_each(kbd, &b->keyboards)
        wl_keyboard_send_key(kbd, wl_display_next_serial(b->display), t, key,
                             pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
                                     : WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Anland reports positions in real screen pixels; Android wants them in the
 * coordinate space of the frame it rendered. */
static void map_point(struct bridge *b, float x, float y, wl_fixed_t *fx, wl_fixed_t *fy)
{
    double sx = x, sy = y;
    if (b->screen_w && b->screen_h && b->client_w && b->client_h) {
        sx = (double)x * b->client_w / b->screen_w;
        sy = (double)y * b->client_h / b->screen_h;
    }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (b->client_w && sx > b->client_w - 1) sx = b->client_w - 1;
    if (b->client_h && sy > b->client_h - 1) sy = b->client_h - 1;
    *fx = wl_fixed_from_double(sx);
    *fy = wl_fixed_from_double(sy);
}

static void touch_send_all(struct bridge *b, int action, int id, float x, float y)
{
    struct surface *s = b->top;
    if (!s)
        return;
    struct wl_client *client = wl_resource_get_client(s->resource);
    wl_fixed_t fx, fy;
    map_point(b, x, y, &fx, &fy);
    uint32_t t = now_ms();
    struct wl_resource *touch;
    wl_resource_for_each(touch, &b->touches) {
        if (wl_resource_get_client(touch) != client)
            continue;
        switch (action) {
        case INPUT_ACTION_DOWN:
            wl_touch_send_down(touch, wl_display_next_serial(b->display), t, s->resource,
                               id, fx, fy);
            break;
        case INPUT_ACTION_UP:
            wl_touch_send_up(touch, wl_display_next_serial(b->display), t, id);
            break;
        case INPUT_ACTION_MOVE:
            wl_touch_send_motion(touch, t, id, fx, fy);
            break;
        default:
            break;
        }
    }
}

static void touch_frame_all(struct bridge *b)
{
    struct wl_resource *touch;
    wl_resource_for_each(touch, &b->touches)
        wl_touch_send_frame(touch);
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *p = wl_resource_create(c, &wl_pointer_interface,
                                               wl_resource_get_version(r), id);
    if (!p) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(p, inert_dispatch, NULL, NULL, NULL);
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct bridge *b = wl_resource_get_user_data(r);
    struct wl_resource *k = wl_resource_create(c, &wl_keyboard_interface,
                                               wl_resource_get_version(r), id);
    if (!k) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(k, inert_dispatch, NULL, b, unlink_resource);
    wl_list_insert(&b->keyboards, wl_resource_get_link(k));

    /* Raw evdev key codes are forwarded as they are; no keymap is needed. */
    int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        wl_keyboard_send_keymap(k, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
        close(fd);
    }
    if (wl_resource_get_version(k) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(k, 0, 0);
    keyboard_enter_one(b, k);
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct bridge *b = wl_resource_get_user_data(r);
    struct wl_resource *t = wl_resource_create(c, &wl_touch_interface,
                                               wl_resource_get_version(r), id);
    if (!t) { wl_client_post_no_memory(c); return; }
    wl_resource_set_dispatcher(t, inert_dispatch, NULL, b, unlink_resource);
    wl_list_insert(&b->touches, wl_resource_get_link(t));
}

static void seat_release(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void bind_seat(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)version, id);
    if (!r) { wl_client_post_no_memory(c); return; }
    wl_resource_set_implementation(r, &seat_impl, data, NULL);
    if (version >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(r, "anland");
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_TOUCH | WL_SEAT_CAPABILITY_KEYBOARD);
}

/* ------------------------------------------------------------------------- */
/* Main loop                                                                 */
/* ------------------------------------------------------------------------- */

static void fire_frame_callbacks(struct bridge *b)
{
    struct surface *s = b->top;
    if (!s || wl_list_empty(&s->frame_callbacks))
        return;
    uint32_t t = now_ms();
    struct wl_resource *cb, *tmp;
    wl_resource_for_each_safe(cb, tmp, &s->frame_callbacks) {
        wl_callback_send_done(cb, t);
        wl_resource_destroy(cb);
    }
}

static int on_tick(void *data)
{
    struct bridge *b = data;

    anland_poll(b);

    /* A window that is still waiting for its first configure gets it as soon
     * as the real size is known or the grace period ran out. */
    struct surface *s = b->top;
    if (s && s->toplevel && !s->configure_sent &&
        (b->size_from_display ||
         now_ms64() - b->start_ms >= (uint64_t)b->cfg.wait_for_display_ms))
        send_toplevel_configure(s);

    maybe_present(b);
    fire_frame_callbacks(b);

    uint64_t now = now_ms64();
    if (g_verbose && now - b->last_stats_ms >= 5000) {
        DBG("%.1f frames/s shown, session=%d", (b->frames - b->last_frames) * 1000.0 /
            (double)(now - b->last_stats_ms), b->session);
        b->last_stats_ms = now;
        b->last_frames = b->frames;
    }

    wl_event_source_timer_update(b->tick, 16);
    return 0;
}

static int on_signal(int signo, void *data)
{
    struct bridge *b = data;
    INFO("signal %d, shutting down", signo);
    wl_display_terminate(b->display);
    return 0;
}

static int mkdir_p(const char *path, mode_t mode)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        if (mkdir(tmp, mode) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(tmp, mode) < 0 && errno != EEXIST)
        return -1;
    return chmod(tmp, mode);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

static void config_set(struct config *c, const char *key, const char *value)
{
    if (!strcmp(key, "anland_socket"))
        snprintf(c->anland_socket, sizeof(c->anland_socket), "%s", value);
    else if (!strcmp(key, "runtime_dir"))
        snprintf(c->runtime_dir, sizeof(c->runtime_dir), "%s", value);
    else if (!strcmp(key, "wayland_display"))
        snprintf(c->display_name, sizeof(c->display_name), "%s", value);
    else if (!strcmp(key, "log"))
        snprintf(c->log_path, sizeof(c->log_path), "%s", value);
    else if (!strcmp(key, "default_width"))
        c->default_width = atoi(value);
    else if (!strcmp(key, "default_height"))
        c->default_height = atoi(value);
    else if (!strcmp(key, "render_scale"))
        c->render_scale = atoi(value);
    else if (!strcmp(key, "max_fps"))
        c->max_fps = atoi(value);
    else if (!strcmp(key, "wait_for_display"))
        c->wait_for_display_ms = atoi(value) * 1000;
    else if (!strcmp(key, "ui_width_dp"))
        c->ui_width_dp = atoi(value);
    else if (!strcmp(key, "verbose"))
        g_verbose = atoi(value);
}

/* The shared config file keeps bridge settings under a "bridge." prefix; every
 * other line in it belongs to someone else (Android properties, the init). */
static void config_load(struct config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '#' || strncmp(s, "bridge.", 7) != 0)
            continue;
        char *eq = strchr(s, '=');
        if (!eq)
            continue;
        *eq = 0;
        config_set(c, trim(s + 7), trim(eq + 1));
    }
    fclose(f);
}

static void config_clamp(struct config *c)
{
    if (c->default_width < 64 || c->default_width > 16384) c->default_width = 1080;
    if (c->default_height < 64 || c->default_height > 16384) c->default_height = 1920;
    if (c->render_scale < 10 || c->render_scale > 100) c->render_scale = 100;
    if (c->max_fps < 1 || c->max_fps > 240) c->max_fps = 60;
    if (c->wait_for_display_ms < 0 || c->wait_for_display_ms > 600000)
        c->wait_for_display_ms = 20000;
    if (c->ui_width_dp < 0 || c->ui_width_dp > 2000) c->ui_width_dp = 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [--config FILE] [--key value ...]\n"
        "Keys (also accepted as bridge.<key>=value lines in the config file):\n"
        "  --anland_socket PATH   Anland daemon socket (default /run/display.sock)\n"
        "  --runtime_dir DIR      where the Wayland socket is created (default /run/xdg)\n"
        "  --wayland_display NAME Wayland socket name (default wayland-0)\n"
        "  --default_width N --default_height N   size used until the Anland app connects\n"
        "  --render_scale PCT     Android renders at this percentage of the screen (10-100)\n"
        "  --max_fps N            upper limit for the refresh rate reported to Android\n"
        "  --wait_for_display S   seconds to wait for the Anland app before using the default size\n"
        "  --ui_width_dp N        choose Android's density so the short screen side is N dp (0 = off)\n"
        "  --log FILE             also write messages to FILE\n"
        "  --verbose 1            extra messages\n", argv0);
}

int main(int argc, char **argv)
{
    struct bridge *b = calloc(1, sizeof(*b));
    if (!b)
        return 1;
    struct config *c = &b->cfg;
    snprintf(c->anland_socket, sizeof(c->anland_socket), "/run/display.sock");
    snprintf(c->runtime_dir, sizeof(c->runtime_dir), "/run/xdg");
    snprintf(c->display_name, sizeof(c->display_name), "wayland-0");
    c->default_width = 1080;
    c->default_height = 1920;
    c->render_scale = 100;
    c->max_fps = 60;
    c->wait_for_display_ms = 20000;
    c->ui_width_dp = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
        if (strncmp(argv[i], "--", 2) != 0 || i + 1 >= argc) {
            usage(argv[0]);
            return 2;
        }
        if (!strcmp(argv[i], "--config"))
            config_load(c, argv[++i]);
        else {
            const char *key = argv[i] + 2;
            config_set(c, key, argv[++i]);
        }
    }
    config_clamp(c);

    if (c->log_path[0]) {
        g_log_file = fopen(c->log_path, "w");
        if (g_log_file)
            fchmod(fileno(g_log_file), 0644);
    }
    signal(SIGPIPE, SIG_IGN);

    wl_list_init(&b->outputs);
    wl_list_init(&b->keyboards);
    wl_list_init(&b->touches);
    b->start_ms = now_ms64();
    b->last_stats_ms = b->start_ms;
    b->refresh_mhz = 60000;
    b->client_w = (uint32_t)c->default_width;
    b->client_h = (uint32_t)c->default_height;

    if (mkdir_p(c->runtime_dir, 0777) < 0)
        WARN("cannot prepare %s: %s", c->runtime_dir, strerror(errno));
    setenv("XDG_RUNTIME_DIR", c->runtime_dir, 1);

    /* A previous run may have left its socket behind (the directory can live on
     * a persistent disk); libwayland refuses to reuse a locked name otherwise. */
    char path[PATH_MAX + 80];
    snprintf(path, sizeof(path), "%s/%s", c->runtime_dir, c->display_name);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%s.lock", c->runtime_dir, c->display_name);
    unlink(path);

    b->display = wl_display_create();
    if (!b->display) {
        WARN("cannot create the Wayland display");
        return 1;
    }
    b->loop = wl_display_get_event_loop(b->display);
    if (wl_display_add_socket(b->display, c->display_name) < 0) {
        WARN("cannot create Wayland socket %s/%s: %s", c->runtime_dir, c->display_name,
             strerror(errno));
        return 1;
    }
    /* The Android composer runs as an unprivileged user and must reach it. */
    snprintf(path, sizeof(path), "%s/%s", c->runtime_dir, c->display_name);
    chmod(path, 0777);

    wl_display_init_shm(b->display);
    wl_display_add_shm_format(b->display, WL_SHM_FORMAT_ABGR8888);
    wl_display_add_shm_format(b->display, WL_SHM_FORMAT_XBGR8888);

    if (!wl_global_create(b->display, &wl_compositor_interface, 4, b, bind_compositor) ||
        !wl_global_create(b->display, &xdg_wm_base_interface, 1, b, bind_wm_base) ||
        !wl_global_create(b->display, &wl_seat_interface, 5, b, bind_seat) ||
        !wl_global_create(b->display, &wl_output_interface, 3, b, bind_output)) {
        WARN("cannot register Wayland globals");
        return 1;
    }

    wl_event_loop_add_signal(b->loop, SIGTERM, on_signal, b);
    wl_event_loop_add_signal(b->loop, SIGINT, on_signal, b);
    b->tick = wl_event_loop_add_timer(b->loop, on_tick, b);
    wl_event_source_timer_update(b->tick, 1);

    INFO("ready: Wayland socket %s, Anland socket %s, default %dx%d, scale %d%%, max %d fps",
         path, c->anland_socket, c->default_width, c->default_height, c->render_scale,
         c->max_fps);

    wl_display_run(b->display);

    if (b->ctx) {
        session_teardown(b);
        disconnect(b->ctx);
    }
    wl_display_destroy_clients(b->display);
    wl_display_destroy(b->display);
    free(b->xmap);
    free(b);
    return 0;
}
