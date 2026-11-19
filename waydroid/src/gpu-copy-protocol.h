/*
 * Messages between anland-waydroid-bridge and anland-gpu-copy.
 *
 * The bridge is a static program that runs before Android is up, so it cannot
 * use Android's GPU driver itself. When Android draws with the GPU, the bridge
 * starts anland-gpu-copy (built with the Android NDK, linked to Android's EGL)
 * and lets it copy every frame into the Anland buffer with the GPU.
 *
 * Transport: one SOCK_SEQPACKET socket pair, the helper's end is fd 3.
 * Every request gets exactly one reply. dma-buf descriptors travel as
 * SCM_RIGHTS: with GC_TARGET and GC_SOURCE requests (the buffer to import) and
 * with a GC_COPY reply (a render-done fence, if the driver can make one).
 */
#ifndef GPU_COPY_PROTOCOL_H
#define GPU_COPY_PROTOCOL_H

#include <stdint.h>

#define GC_VERSION 1

enum gc_type {
    GC_HELLO = 1,         /* aux = GC_VERSION; reply text = GL renderer */
    GC_TARGET = 2,        /* id = Anland buffer index; fd + layout */
    GC_SOURCE = 3,        /* id = bridge's buffer id; fd + layout */
    GC_FORGET_SOURCE = 4, /* id */
    GC_RESET_TARGETS = 5, /* drop every imported Anland buffer */
    GC_COPY = 6,          /* id = source, aux = target index */
};

struct gc_request {
    uint32_t type;
    uint32_t id;
    uint32_t aux;
    uint32_t width, height, stride, offset;
    uint32_t fourcc;      /* DRM fourcc */
    uint64_t modifier;
} __attribute__((packed));

struct gc_reply {
    uint32_t type;        /* echoes the request */
    int32_t status;       /* 0 = done, otherwise a negative errno */
    char text[200];       /* what went wrong, or the GL renderer for GC_HELLO */
} __attribute__((packed));

#endif
