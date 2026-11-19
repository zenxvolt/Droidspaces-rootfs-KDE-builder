/*
 * Test stand-in for the Anland Android app (the "consumer").
 *
 * It connects to a real Anland display daemon, offers CPU-visible buffers the
 * way the app offers its screen buffers, asks the producer for frames, saves
 * them as PPM pictures and injects a few touch and key events.
 *
 * Usage: fake-anland-app SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX [idle]
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "display_consumer.h"

#define NBUF 3

static void connected(void *data) { *(bool *)data = true; }
static void fell_back(void *data) { *(bool *)data = true; }

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
        fprintf(stderr, "usage: %s SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX [idle]\n", argv[0]);
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
    set_screen_info(ctx, w, h, 1, 60000);
    push_dmabufs(ctx, fds, infos, NBUF);
    printf("fake-app: connected, offering %d buffers of %ux%u\n", NBUF, w, h);
    fflush(stdout);

    int got = 0;
    for (int attempt = 0; attempt < 2000 && got < frames; attempt++) {
        int idx = got % NBUF;
        if (select_dmabuf(ctx, idx) < 0) {
            usleep(20000);
            continue;
        }
        int fence = refresh_done(ctx);
        if (fence >= 0)
            close(fence);
        if (timed_out) {
            fprintf(stderr, "fake-app: lost the Anland session while waiting for a frame\n");
            break;
        }
        if (maps[idx][3] != 0xff || (idle_test ? !active : maps[idx][2] != 200)) {
            /* Wait for the session, and for the Android gradient in the normal tests. */
            usleep(20000);
            continue;
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
            /* A variable-length event the bridge must skip without losing sync. */
            const char text[] = "clipboard text the bridge ignores";
            struct InputEvent clip = { .type = INPUT_TYPE_CLIPBOARD };
            clip.clipboard.size = sizeof(text);
            push_input_event_with_length(ctx, &clip, (void *)text, sizeof(text));
            key.key.action = INPUT_ACTION_DOWN;
            key.key.keycode = 48; /* KEY_B, must still arrive after the clipboard */
            push_input_event(ctx, &key);
        }
    }
    printf("fake-app: received %d frames\n", got);
    fflush(stdout);
    usleep(300000);
    disconnect(ctx);
    return got == frames ? 0 : 1;
}
