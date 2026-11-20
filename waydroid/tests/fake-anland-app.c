/*
 * Test stand-in for the Anland Android app (the "consumer").
 *
 * It connects to a real Anland display daemon, offers CPU-visible buffers the
 * way the app offers its screen buffers, asks the producer for frames, saves
 * them as PPM pictures and injects a few touch and key events.
 *
 * With AUDIO_OUT and SECONDS it also announces a 48 kHz stereo speaker on the
 * audio channel, like the app does, and saves the sound it receives there.
 *
 * Usage: fake-anland-app SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX [idle | AUDIO_OUT SECONDS]
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/socket.h>

#include "display_consumer.h"

#define NBUF 3

static void connected(void *data) { *(bool *)data = true; }
static void fell_back(void *data) { *(bool *)data = true; }

static void send_audio_format(int fd, uint32_t role, uint32_t channels)
{
    struct { struct audio_msg h; struct audio_format f; } __attribute__((packed)) m = {
        .h = { .type = AUDIO_MSG_FORMAT, .size = sizeof(struct audio_format) },
        .f = { .rate = 48000, .channels = channels, .format = AUDIO_FORMAT_S16LE,
               .role = role, .quantum = 0 },
    };
    send(fd, &m, sizeof(m), MSG_NOSIGNAL);
}

/* Saves every PCM message for the given time; returns the number of bytes. */
static long record_audio(display_ctx *ctx, const char *path, double seconds)
{
    FILE *out = fopen(path, "wb");
    if (!out)
        return -1;
    static uint8_t buf[65536 + 64];
    long total = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = (now.tv_sec - start.tv_sec) + (now.tv_nsec - start.tv_nsec) / 1e9;
        if (t >= seconds)
            break;
        int fd = get_audio_fd(ctx);
        if (fd < 0) {
            usleep(10000);
            continue;
        }
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 20) <= 0)
            continue;
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n < (ssize_t)sizeof(struct audio_msg))
            continue;
        struct audio_msg h;
        memcpy(&h, buf, sizeof(h));
        if (h.type != AUDIO_MSG_PCM || h.size > n - sizeof(h))
            continue;
        fwrite(buf + sizeof(h), 1, h.size, out);
        total += h.size;
    }
    fclose(out);
    return total;
}

static void save_ppm(const char *path, const uint8_t *px, uint32_t w, uint32_t h, uint32_t stride)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++)
            fwrite(px + (size_t)y * stride + (size_t)x * 4, 1, 3, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "usage: %s SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX [idle | AUDIO_OUT SECONDS]\n",
                argv[0]);
        return 2;
    }
    const char *sock = argv[1];
    uint32_t w = (uint32_t)atoi(argv[2]), h = (uint32_t)atoi(argv[3]);
    int frames = atoi(argv[4]);
    const char *prefix = argv[5];
    bool idle_test = argc > 6 && !strcmp(argv[6], "idle");

    /* A stride wider than the picture, like real graphics buffers often have. */
    uint32_t stride = (w + 16) * 4;
    size_t size = (size_t)stride * h;
    int fds[NBUF];
    uint8_t *maps[NBUF];
    struct buf_info infos[NBUF];
    for (int i = 0; i < NBUF; i++) {
        fds[i] = memfd_create("fake-anland-buffer", 0);
        if (fds[i] < 0 || ftruncate(fds[i], (off_t)size) < 0) {
            perror("memfd");
            return 1;
        }
        maps[i] = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fds[i], 0);
        memset(maps[i], 0x55, size);
        infos[i] = (struct buf_info){ .stride = stride, .width = w, .height = h,
                                      .format = 1 /* RGBA_8888 */, .modifier = 0, .offset = 0 };
    }

    display_ctx *ctx = NULL;
    for (int tries = 0; tries < 100 && connect_to_deamon(&ctx, sock) < 0; tries++)
        usleep(100000);
    if (!ctx) {
        fprintf(stderr, "fake-app: cannot connect to %s\n", sock);
        return 1;
    }
    bool active = false, timed_out = false;
    if (idle_test) {
        set_exit_fallback_callback(ctx, connected, &active);
        set_fallback_callback(ctx, fell_back, &timed_out);
    }
    const char *refresh_mhz = getenv("FAKE_ANLAND_REFRESH_MHZ");
    set_screen_info(ctx, w, h, 1, refresh_mhz ? (uint32_t)atoi(refresh_mhz) : 60000);
    push_dmabufs(ctx, fds, infos, NBUF);
    printf("fake-app: connected, offering %d buffers of %ux%u\n", NBUF, w, h);
    fflush(stdout);

    if (argc >= 8) {
        /* Asking for a frame makes the app leave its fallback state; from then
         * on the audio channel exists. No frame is waited for: without a
         * composer none comes, and waiting would put the app back in fallback. */
        for (int i = 0; i < 200 && get_audio_fd(ctx) < 0; i++) {
            select_dmabuf(ctx, 0);
            if (get_audio_fd(ctx) < 0)
                usleep(10000);
        }
        if (get_audio_fd(ctx) < 0) {
            fprintf(stderr, "fake-app: no audio channel\n");
            return 1;
        }
        send_audio_format(get_audio_fd(ctx), AUDIO_ROLE_PLAYBACK, 2);
        send_audio_format(get_audio_fd(ctx), AUDIO_ROLE_CAPTURE, 1);
        printf("fake-app: speaker announced, listening\n");
        fflush(stdout);
        long bytes = record_audio(ctx, argv[6], atof(argv[7]));
        printf("fake-app: received %ld bytes of sound\n", bytes);
        disconnect(ctx);
        return bytes > 0 ? 0 : 1;
    }

    int got = 0, fences = 0;
    bool copied = false;
    for (int attempt = 0; attempt < 2000 && got < frames; attempt++) {
        int idx = got % NBUF;
        if (select_dmabuf(ctx, idx) < 0) {
            usleep(20000);
            continue;
        }
        int fence = refresh_done(ctx);
        if (fence >= 0) {
            fences++;
            close(fence);
        }
        if (timed_out) {
            fprintf(stderr, "fake-app: lost the Anland session while waiting for a frame\n");
            break;
        }
        if (maps[idx][3] != 0xff || (idle_test ? !active : (maps[idx][2] < 192 || maps[idx][2] > 208))) {
            /* Wait for the session, and for the Android gradient in the normal tests. */
            usleep(20000);
            continue;
        }
        struct OutputEvent output;
        if (!idle_test && poll_output_event(ctx, &output, 0) == 1 &&
            output.type == OUTPUT_TYPE_CLIPBOARD) {
            const char expected[] = "Salinan dari Waydroid";
            char text[sizeof(expected) - 1];
            if (output.clipboard.size != sizeof(text) ||
                poll_output_event_extend_data(ctx, text, sizeof(text), 1000) != 1 ||
                memcmp(text, expected, sizeof(text)) != 0) {
                fprintf(stderr, "fake-app: unexpected clipboard text\n");
                return 1;
            }
            copied = true;
        }
        got++;
        if (got <= 3 || got == frames || (idle_test && got > frames - NBUF)) {
            char path[512];
            snprintf(path, sizeof(path), "%s-%d.ppm", prefix, got);
            save_ppm(path, maps[idx], w, h, stride);
        }
        if (idle_test) {
            usleep(20000);
            continue;
        }
        if (got == 2) {
            struct InputEvent ev = { .type = INPUT_TYPE_TOUCH };
            ev.touch.action = INPUT_ACTION_DOWN;
            ev.touch.x = w / 2.0f;
            ev.touch.y = h / 4.0f;
            ev.touch.pointer_id = 0;
            push_input_event(ctx, &ev);
            struct InputEvent fr = { .type = INPUT_TYPE_TOUCH_FRAME };
            push_input_event(ctx, &fr);
        }
        if (got == 3) {
            struct InputEvent ev = { .type = INPUT_TYPE_TOUCH };
            ev.touch.action = INPUT_ACTION_UP;
            ev.touch.pointer_id = 0;
            push_input_event(ctx, &ev);
            struct InputEvent key = { .type = INPUT_TYPE_KEY };
            key.key.action = INPUT_ACTION_DOWN;
            key.key.keycode = 30; /* KEY_A */
            push_input_event(ctx, &key);
            key.key.action = INPUT_ACTION_UP;
            push_input_event(ctx, &key);
            /* Clipboard traffic must preserve framing for the following key event. */
            char text[] = "Tempel dari AnLand: é";
            struct InputEvent clip = { .type = INPUT_TYPE_CLIPBOARD };
            clip.clipboard.size = sizeof(text) - 1;
            push_input_event_with_length(ctx, &clip, text, sizeof(text) - 1);
            key.key.action = INPUT_ACTION_DOWN;
            key.key.keycode = 48; /* KEY_B, must still arrive after the clipboard */
            push_input_event(ctx, &key);
        }
    }
    printf("fake-app: received %d frames, %d with a render fence\n", got, fences);
    if (copied)
        puts("fake-app: received the Android clipboard");
    fflush(stdout);
    usleep(300000);
    disconnect(ctx);
    return got == frames ? 0 : 1;
}
