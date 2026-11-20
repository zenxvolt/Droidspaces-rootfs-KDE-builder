/*
 * Test stand-in for anland-gpu-copy.
 *
 * Speaks the same protocol (waydroid/src/gpu-copy-protocol.h) but copies with
 * the CPU, so the bridge's GPU copy path can be tested on a machine without
 * an Adreno GPU: buffer import, copy requests, forgetting buffers, and the
 * render fence handed to Anland (here an eventfd that is already signalled).
 *
 * Supports what the tests send: 32-bit sources, RGBA_8888 targets, any size
 * (nearest-neighbour scaling).
 *
 * FAKE_GPU_COPY_FAIL=hello makes start-up fail, =copy makes the third copy
 * fail, to test the bridge's fall-back to the CPU.
 *
 * Usage: fake-gpu-copy SOCKET_FD
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "gpu-copy-protocol.h"

struct image {
    bool used;
    uint32_t id;
    uint8_t *map;
    size_t len;
    struct gc_request info;
};

static struct image targets[8], sources[32];

static int map_image(struct image *img, int fd, const struct gc_request *rq)
{
    size_t len = (size_t)rq->offset + (size_t)rq->stride * rq->height;
    void *map = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED)
        return -errno;
    if (img->used)
        munmap(img->map, img->len);
    img->used = true;
    img->id = rq->id;
    img->map = map;
    img->len = len;
    img->info = *rq;
    return 0;
}

static struct image *find_source(uint32_t id)
{
    for (int i = 0; i < 32; i++)
        if (sources[i].used && sources[i].id == id)
            return &sources[i];
    return NULL;
}

static uint32_t to_rgba(uint32_t px, uint32_t fourcc)
{
    bool bgr_order = fourcc == 0x34324241u /* AB24 */ || fourcc == 0x34324258u /* XB24 */;
    if (!bgr_order)
        px = (px & 0x0000ff00u) | ((px >> 16) & 0xffu) | ((px & 0xffu) << 16);
    return px | 0xff000000u;
}

int main(int argc, char **argv)
{
    int sock = argc > 1 ? atoi(argv[1]) : 3;
    const char *fail = getenv("FAKE_GPU_COPY_FAIL");
    int copies = 0;
    for (;;) {
        struct gc_request rq;
        char cbuf[CMSG_SPACE(sizeof(int))];
        struct iovec iov = { &rq, sizeof(rq) };
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                             .msg_control = cbuf, .msg_controllen = sizeof(cbuf) };
        ssize_t n = recvmsg(sock, &mh, 0);
        if (n <= 0)
            break;
        int fd = -1;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
            if (c->cmsg_type == SCM_RIGHTS)
                memcpy(&fd, CMSG_DATA(c), sizeof(int));

        struct gc_reply rp = { .type = rq.type };
        int out_fd = -1;
        switch (rq.type) {
        case GC_HELLO:
            if (fail && !strcmp(fail, "hello")) {
                rp.status = -ENODEV;
                snprintf(rp.text, sizeof(rp.text), "pretend there is no GPU");
            } else {
                snprintf(rp.text, sizeof(rp.text), "fake GPU (CPU copy), render fences");
            }
            break;
        case GC_TARGET:
            rp.status = rq.id < 8 ? map_image(&targets[rq.id], fd, &rq) : -EINVAL;
            break;
        case GC_SOURCE: {
            struct image *img = find_source(rq.id);
            for (int i = 0; i < 32 && !img; i++)
                if (!sources[i].used)
                    img = &sources[i];
            rp.status = img ? map_image(img, fd, &rq) : -ENOSPC;
            break;
        }
        case GC_FORGET_SOURCE: {
            struct image *img = find_source(rq.id);
            if (img) {
                munmap(img->map, img->len);
                img->used = false;
            }
            printf("fake-gpu-copy: forgot source %u\n", rq.id);
            break;
        }
        case GC_RESET_TARGETS:
            for (int i = 0; i < 8; i++)
                if (targets[i].used) {
                    munmap(targets[i].map, targets[i].len);
                    targets[i].used = false;
                }
            break;
        case GC_COPY: {
            struct image *s = find_source(rq.id);
            struct image *t = rq.aux < 8 && targets[rq.aux].used ? &targets[rq.aux] : NULL;
            if (++copies == 3 && fail && !strcmp(fail, "copy")) {
                rp.status = -EIO;
                snprintf(rp.text, sizeof(rp.text), "pretend the GPU hung");
                break;
            }
            if (!s || !t) {
                rp.status = -ENOENT;
                break;
            }
            uint32_t sw = s->info.width, sh = s->info.height, dw = t->info.width, dh = t->info.height;
            for (uint32_t y = 0; y < dh; y++) {
                const uint32_t *src = (const uint32_t *)(s->map + s->info.offset +
                                                         (size_t)(y * sh / dh) * s->info.stride);
                uint32_t *dst = (uint32_t *)(t->map + t->info.offset + (size_t)y * t->info.stride);
                for (uint32_t x = 0; x < dw; x++)
                    dst[x] = to_rgba(src[x * sw / dw], s->info.fourcc);
            }
            out_fd = eventfd(1, EFD_CLOEXEC);
            break;
        }
        default:
            rp.status = -EPROTO;
        }
        if (fd >= 0)
            close(fd);

        iov.iov_base = &rp;
        iov.iov_len = sizeof(rp);
        struct msghdr out = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (out_fd >= 0) {
            memset(cbuf, 0, sizeof(cbuf));
            out.msg_control = cbuf;
            out.msg_controllen = sizeof(cbuf);
            struct cmsghdr *c = CMSG_FIRSTHDR(&out);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(c), &out_fd, sizeof(int));
        }
        sendmsg(sock, &out, MSG_NOSIGNAL);
        if (out_fd >= 0)
            close(out_fd);
        fflush(stdout);
    }
    return 0;
}
