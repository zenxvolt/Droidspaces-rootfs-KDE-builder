/*
 * Test stand-in for the Anland Android app (the "consumer") that offers GPU
 * buffers, the way the real app does.
 *
 * waydroid/tests/fake-anland-app.c offers plain memory, which is enough for a
 * CPU renderer. A wlroots compositor imports the buffers into OpenGL ES, so
 * this one allocates linear dmabufs with GBM on a render node, asks the
 * producer for frames, saves a few as PPM pictures and injects touch and key
 * events.
 *
 * Usage: fake-anland-gbm-app RENDER_NODE SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX
 *
 * FRAMES is how long to wait for a picture; once something other than the
 * black background shows up it touches the middle, presses A and keeps
 * receiving 40 more frames.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <unistd.h>

#include "display_consumer.h"

#define NBUF 3

static void save_ppm(const char *path, struct gbm_bo *bo, uint32_t w, uint32_t h)
{
    uint32_t stride = 0;
    void *map_data = NULL;
    const uint8_t *px = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &stride, &map_data);
    if (!px) {
        fprintf(stderr, "fake-app: cannot map buffer for reading\n");
        return;
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                fwrite(px + (size_t)y * stride + (size_t)x * 4, 1, 3, f);
        fclose(f);
    }
    gbm_bo_unmap(bo, map_data);
}

#define SENTINEL 0x55

/* Byte 3 is the alpha of the first pixel; a compositor always writes 0xff. */
static int read_sentinel(struct gbm_bo *bo)
{
    uint32_t stride = 0;
    void *map_data = NULL;
    uint8_t *px = gbm_bo_map(bo, 0, 0, 1, 1, GBM_BO_TRANSFER_READ, &stride, &map_data);
    if (!px)
        return -1;
    int value = px[3];
    gbm_bo_unmap(bo, map_data);
    return value;
}

static void write_sentinel(struct gbm_bo *bo)
{
    uint32_t stride = 0;
    void *map_data = NULL;
    uint8_t *px = gbm_bo_map(bo, 0, 0, 1, 1, GBM_BO_TRANSFER_READ_WRITE, &stride, &map_data);
    if (!px)
        return;
    px[3] = SENTINEL;
    gbm_bo_unmap(bo, map_data);
}

/* True once something other than the black background is in the middle. */
static int has_picture(struct gbm_bo *bo, uint32_t w, uint32_t h)
{
    uint32_t stride = 0;
    void *map_data = NULL;
    uint8_t *px = gbm_bo_map(bo, w / 2, h / 2, 1, 1, GBM_BO_TRANSFER_READ, &stride, &map_data);
    if (!px)
        return 0;
    int seen = px[0] || px[1] || px[2];
    gbm_bo_unmap(bo, map_data);
    return seen;
}

static void push_key(display_ctx *ctx, int code)
{
    struct InputEvent key = { .type = INPUT_TYPE_KEY };
    key.key.keycode = code;
    key.key.action = INPUT_ACTION_DOWN;
    push_input_event(ctx, &key);
    key.key.action = INPUT_ACTION_UP;
    push_input_event(ctx, &key);
}

int main(int argc, char **argv)
{
    if (argc < 7) {
        fprintf(stderr, "usage: %s RENDER_NODE SOCKET WIDTH HEIGHT FRAMES OUTPUT_PREFIX\n", argv[0]);
        return 2;
    }
    const char *node = argv[1], *sock = argv[2], *prefix = argv[6];
    uint32_t w = (uint32_t)atoi(argv[3]), h = (uint32_t)atoi(argv[4]);
    int frames = atoi(argv[5]);

    int drm_fd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device *gbm = drm_fd >= 0 ? gbm_create_device(drm_fd) : NULL;
    if (!gbm) {
        fprintf(stderr, "fake-app: cannot open GBM on %s\n", node);
        return 1;
    }

    struct gbm_bo *bos[NBUF];
    int fds[NBUF];
    struct buf_info infos[NBUF];
    for (int i = 0; i < NBUF; i++) {
        /* Android's RGBA_8888 is DRM ABGR8888 in memory order R,G,B,A. */
        bos[i] = gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888,
                               GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!bos[i]) {
            fprintf(stderr, "fake-app: gbm_bo_create failed\n");
            return 1;
        }
        fds[i] = gbm_bo_get_fd(bos[i]);
        infos[i] = (struct buf_info){ .stride = gbm_bo_get_stride(bos[i]), .width = w, .height = h,
                                      .format = 1 /* RGBA_8888 */, .modifier = 0, .offset = 0 };
    }

    display_ctx *ctx = NULL;
    for (int tries = 0; tries < 100 && connect_to_deamon(&ctx, sock) < 0; tries++)
        usleep(100000);
    if (!ctx) {
        fprintf(stderr, "fake-app: cannot connect to %s\n", sock);
        return 1;
    }
    set_screen_info(ctx, w, h, 1, 60000);
    push_dmabufs(ctx, fds, infos, NBUF);
    printf("fake-app: connected, offering %d GPU buffers of %ux%u\n", NBUF, w, h);
    fflush(stdout);

    int got = 0, idle = 0, touched_at = 0;
    for (int i = 0; i < NBUF; i++)
        write_sentinel(bos[i]);
    for (int attempt = 0; attempt < 6000 && (got < frames || (touched_at && got < touched_at + 40)); attempt++) {
        if (got >= frames && !touched_at)
            break; /* the producer never showed anything */
        int idx = got % NBUF;
        if (select_dmabuf(ctx, idx) < 0) {
            usleep(20000);
            continue;
        }
        /* Without a producer the library falls back and returns at once, so
         * only a frame that overwrote the sentinel counts. */
        int fence = refresh_done(ctx);
        if (fence >= 0) {
            struct pollfd pfd = { .fd = fence, .events = POLLIN };
            poll(&pfd, 1, 2000);
            close(fence);
        }
        if (read_sentinel(bos[idx]) == SENTINEL) {
            idle++;
            usleep(20000);
            continue;
        }
        write_sentinel(bos[idx]);
        got++;
        if (!touched_at && has_picture(bos[idx], w, h))
            touched_at = got;
        if (got <= 2 || got == touched_at || (touched_at && got == touched_at + 40)) {
            char path[512];
            snprintf(path, sizeof(path), "%s-%d.ppm", prefix, got);
            save_ppm(path, bos[idx], w, h);
        }
        if (touched_at && got == touched_at + 1) {
            printf("fake-app: touching the middle of the screen\n");
            fflush(stdout);
            struct InputEvent ev = { .type = INPUT_TYPE_TOUCH };
            ev.touch.action = INPUT_ACTION_DOWN;
            ev.touch.x = w / 2.0f;
            ev.touch.y = h / 2.0f;
            ev.touch.pointer_id = 0;
            push_input_event(ctx, &ev);
            struct InputEvent fr = { .type = INPUT_TYPE_TOUCH_FRAME };
            push_input_event(ctx, &fr);
        }
        if (touched_at && got == touched_at + 2) {
            struct InputEvent ev = { .type = INPUT_TYPE_TOUCH };
            ev.touch.action = INPUT_ACTION_UP;
            ev.touch.pointer_id = 0;
            push_input_event(ctx, &ev);
            struct InputEvent fr = { .type = INPUT_TYPE_TOUCH_FRAME };
            push_input_event(ctx, &fr);
            push_key(ctx, 30); /* KEY_A */
        }
    }
    printf("fake-app: received %d frames (%d polls without a frame)\n", got, idle);
    fflush(stdout);
    usleep(300000);
    disconnect(ctx);
    if (!touched_at)
        printf("fake-app: the picture never appeared\n");
    return got >= frames && touched_at ? 0 : 1;
}
