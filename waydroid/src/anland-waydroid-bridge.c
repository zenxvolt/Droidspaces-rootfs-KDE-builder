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
 * (wl_compositor, wl_shm, zwp_linux_dmabuf_v1, xdg_wm_base, wl_seat,
 * wl_output), copies every committed frame into the buffer the Anland app
 * selected, and turns Anland touch / key events back into wl_touch /
 * wl_keyboard events.
 *
 * Frames arrive as shared memory when Android draws with the CPU, or as
 * dma-bufs from the dma-buf heap when Android draws with the Adreno GPU (see
 * waydroid/gpu/). The bridge itself copies with the CPU: it needs no GL
 * driver, so the binary can be fully static and run inside the Android rootfs
 * before anything else is up. For GPU frames it starts anland-gpu-copy, a
 * small helper linked to Android's EGL, and lets the GPU do the copy instead
 * (waydroid/src/gpu-copy-protocol.h); without the helper it falls back to the
 * CPU.
 *
 * The file itself is MIT licensed like the rest of this repository. The binary
 * links Anland's libdisplay_producer (GPL-3.0), so the resulting executable is
 * distributed under GPL-3.0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wayland-server-protocol.h>

#include "xdg-shell-server-protocol.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "display_producer.h"
#include "display-geometry.h"
#include "gpu-copy-protocol.h"

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
struct bridge_dma_buf_sync { uint64_t flags; };
#define BRIDGE_DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct bridge_dma_buf_sync)

/* AHardwareBuffer formats the Anland app hands out. */
#define AHB_FORMAT_RGBA_8888    1
#define AHB_FORMAT_RGBX_8888    2
#define AHB_FORMAT_RGBA_1010102 43

#define DEFAULT_GPU_COPY_HELPER "/vendor/bin/anland-gpu-copy"

/* DRM fourcc codes of the 32-bit formats accepted from Android. The wl_shm
 * codes are the same except for ARGB8888 (0) and XRGB8888 (1). */
#define FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define DRM_FORMAT_ARGB8888 FOURCC('A', 'R', '2', '4')
#define DRM_FORMAT_XRGB8888 FOURCC('X', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888 FOURCC('A', 'B', '2', '4')
#define DRM_FORMAT_XBGR8888 FOURCC('X', 'B', '2', '4')
#define DRM_FORMAT_RGB565   FOURCC('R', 'G', '1', '6')
#define DRM_FORMAT_BGR888   FOURCC('B', 'G', '2', '4')
#define DRM_FORMAT_MOD_LINEAR  0ull
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

/* Written by /sbin/init; tells whether Android draws with the GPU. */
#define ANDROID_PROP_FILE "/vendor/waydroid.prop"

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
    char benchmark_path[PATH_MAX];
    int default_width, default_height;
    int render_scale;        /* percent of the real screen Android renders at, -1 = auto */
    int max_fps;             /* -1 = auto */
    int gpu;                 /* -1 = read ANDROID_PROP_FILE, 0 = CPU drawing, 1 = GPU */
    int gpu_copy;            /* copy GPU frames with the GPU: -1 = when gpu, 0, 1 */
    char gpu_copy_helper[PATH_MAX];
    int wait_for_display_ms; /* hold the first configure this long for a real size */
    int ui_width_dp;         /* pick the Android density so the short side is this many dp */
};

struct target {
    void *map;
    size_t len;
    int fd;
    struct buf_info info;
    uint64_t frame_serial;
    bool has_frame;
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
    uint64_t tick_due_ms;

    /* Anland side */
    display_ctx *ctx;
    bool session;
    struct target targets[MAX_BUFS];
    int target_count;
    struct wl_event_source *ready_source, *data_source;
    bool target_ready;
    bool first_frame_presented;
    uint64_t frame_serial;
    uint64_t next_present_ms;
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

    /* GPU copy helper (anland-gpu-copy) */
    pid_t gc_pid;
    int gc_sock;
    bool gc_ready, gc_failed;
    uint32_t gc_targets;     /* bit per Anland buffer the helper has imported */
    uint32_t gc_next_id;

    /* Cumulative counts, sampled only while the benchmark requests them. */
    uint64_t shm_frames, dmabuf_frames, delivered_frames, ack_count;
    uint64_t cpu_copies, gpu_copies, failed_copies;
    uint64_t last_delivered_serial, session_serial;
    uint64_t next_sample_ms, last_stats_ms, last_frames;
    int kgsl_open; /* 0 = not checked, 1 = accessible, -1 = unavailable */
};

static void maybe_present(struct bridge *b);
static void send_toplevel_configure(struct surface *s);
static void arm_tick(struct bridge *b, int ms);

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

static void sync_target(struct target *t, uint64_t phase)
{
    struct bridge_dma_buf_sync sync = { .flags = phase | DMA_BUF_SYNC_WRITE };
    ioctl(t->fd, BRIDGE_DMA_BUF_IOCTL_SYNC, &sync);
}

static void clear_target(struct target *t)
{
    uint32_t black = t->info.format == AHB_FORMAT_RGBA_1010102 ? 0xc0000000u : 0xff000000u;
    uint8_t *pixels = (uint8_t *)t->map + t->info.offset;
    sync_target(t, DMA_BUF_SYNC_START);
    for (uint32_t y = 0; y < t->info.height; y++) {
        uint32_t *row = (uint32_t *)(pixels + (size_t)y * t->info.stride);
        for (uint32_t x = 0; x < t->info.width; x++)
            row[x] = black;
    }
    sync_target(t, DMA_BUF_SYNC_END);
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

/* Whole-number enlargement (render_scale 50 -> every pixel becomes 2x2): no
 * lookup table, and the loop is simple enough for the compiler to vectorise. */
static void convert_row_repeat(uint32_t *dst, const uint32_t *src, uint32_t src_n,
                               uint32_t factor, bool src_bgr_order, bool dst_10bit)
{
    if (factor == 2) {
        for (uint32_t x = 0; x < src_n; x++) {
            uint32_t c = dst_10bit ? to_rgba1010102(src[x], src_bgr_order)
                                   : to_rgba8888(src[x], src_bgr_order);
            dst[2 * x] = c;
            dst[2 * x + 1] = c;
        }
        return;
    }
    for (uint32_t x = 0; x < src_n; x++) {
        uint32_t c = dst_10bit ? to_rgba1010102(src[x], src_bgr_order)
                               : to_rgba8888(src[x], src_bgr_order);
        for (uint32_t k = 0; k < factor; k++)
            *dst++ = c;
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

/* A frame from Android, whichever way it arrived. */
struct frame_src {
    const uint8_t *data;
    uint32_t width, height, stride;
    uint32_t format; /* DRM fourcc */
};

/* Returns false when the source or destination layout is one we cannot use. */
static bool copy_frame(struct bridge *b, const struct frame_src *f, struct target *t)
{
    const uint32_t sw = f->width, sh = f->height, sstride = f->stride;
    const uint32_t dw = t->info.width, dh = t->info.height, dstride = t->info.stride;

    bool src_bgr_order;
    switch (f->format) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888: src_bgr_order = false; break;
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888: src_bgr_order = true; break;
    default: return false;
    }

    bool dst_10bit = t->info.format == AHB_FORMAT_RGBA_1010102;
    if (!f->data || !sw || !sh || sstride / 4 < sw)
        return false;

    sync_target(t, DMA_BUF_SYNC_START);

    const uint8_t *src = f->data;
    uint8_t *dst = (uint8_t *)t->map + t->info.offset;

    if (sw == dw && sh == dh) {
        for (uint32_t y = 0; y < dh; y++)
            convert_row((uint32_t *)(dst + (size_t)y * dstride),
                        (const uint32_t *)(src + (size_t)y * sstride),
                        dw, src_bgr_order, dst_10bit);
    } else if (dw % sw == 0 && dh % sh == 0 && dw / sw == dh / sh && dw / sw <= 8) {
        const uint32_t factor = dw / sw;
        for (uint32_t y = 0; y < sh; y++) {
            uint8_t *first = dst + (size_t)y * factor * dstride;
            convert_row_repeat((uint32_t *)first, (const uint32_t *)(src + (size_t)y * sstride),
                               sw, factor, src_bgr_order, dst_10bit);
            for (uint32_t k = 1; k < factor; k++)
                memcpy(first + (size_t)k * dstride, first, (size_t)dw * 4);
        }
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
    sync_target(t, DMA_BUF_SYNC_END);
    return true;
}

/* ------------------------------------------------------------------------- */
/* linux-dmabuf buffers (Android drawing with the GPU)                       */
/* ------------------------------------------------------------------------- */

/* Waydroid's composer hands over the GPU's buffers as single-plane linear
 * dma-bufs allocated from the phone's dma-buf heap. They are mapped once and
 * read with the CPU, bracketed by DMA_BUF_IOCTL_SYNC so the CPU caches see
 * what the GPU wrote. */
struct dmabuf_buffer {
    struct bridge *bridge;
    struct wl_resource *resource;
    uint32_t gc_id;          /* id at the GPU copy helper, 0 = not imported */
    int fd;
    uint32_t offset, stride, width, height, format;
    uint64_t modifier;
    void *map;
    size_t map_len;
    bool map_failed;
};

struct dmabuf_params {
    struct bridge *bridge;
    struct wl_resource *resource;
    int fd;
    uint32_t offset, stride;
    uint64_t modifier;
    bool used;
};

/* Waydroid's composer asks for these for Android's RGBA/RGBX/BGRA_8888,
 * RGB_565 and RGB_888 buffers. An unsupported format would make it abort. */
static const uint32_t dmabuf_formats[] = {
    DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888,
    DRM_FORMAT_RGB565, DRM_FORMAT_BGR888,
};

static uint32_t dmabuf_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_RGB565: return 2;
    case DRM_FORMAT_BGR888: return 3;
    default:                return 4;
    }
}

/* 16 and 24 bit frames are first widened to ABGR8888 in this scratch buffer,
 * so the copy and scaling code only ever sees 32 bit pixels. They are rare:
 * Android draws its windows in 32 bit. */
static uint32_t *g_widen;
static size_t g_widen_len;

static const uint8_t *widen_to_abgr(const uint8_t *src, uint32_t w, uint32_t h,
                                    uint32_t stride, uint32_t format)
{
    size_t need = (size_t)w * h;
    if (need > g_widen_len) {
        uint32_t *p = realloc(g_widen, need * sizeof(uint32_t));
        if (!p)
            return NULL;
        g_widen = p;
        g_widen_len = need;
    }
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * stride;
        uint32_t *out = g_widen + (size_t)y * w;
        if (format == DRM_FORMAT_RGB565) {
            const uint16_t *px = (const uint16_t *)row;
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = px[x];
                uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);
                out[x] = r | (g << 8) | (b << 16) | 0xff000000u;
            }
        } else { /* BGR888: bytes R, G, B */
            for (uint32_t x = 0; x < w; x++)
                out[x] = row[3 * x] | (row[3 * x + 1] << 8) | (row[3 * x + 2] << 16) | 0xff000000u;
        }
    }
    return (const uint8_t *)g_widen;
}

static bool dmabuf_format_supported(uint32_t format)
{
    for (size_t i = 0; i < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); i++)
        if (dmabuf_formats[i] == format)
            return true;
    return false;
}

static void dmabuf_sync(int fd, uint64_t flags)
{
    struct bridge_dma_buf_sync sync = { .flags = flags };
    /* Not every fd type knows this request (a memfd in the tests); that is fine. */
    while (ioctl(fd, BRIDGE_DMA_BUF_IOCTL_SYNC, &sync) < 0 && (errno == EINTR || errno == EAGAIN))
        ;
}

static void dmabuf_buffer_destroy_request(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_buffer_interface dmabuf_buffer_impl = {
    .destroy = dmabuf_buffer_destroy_request,
};

static void gc_forget_source(struct bridge *b, uint32_t id);

static void dmabuf_buffer_resource_destroy(struct wl_resource *r)
{
    struct dmabuf_buffer *d = wl_resource_get_user_data(r);
    if (d->gc_id)
        gc_forget_source(d->bridge, d->gc_id);
    if (d->map)
        munmap(d->map, d->map_len);
    close(d->fd);
    free(d);
}

static struct dmabuf_buffer *dmabuf_buffer_get(struct wl_resource *buffer)
{
    if (!buffer || !wl_resource_instance_of(buffer, &wl_buffer_interface, &dmabuf_buffer_impl))
        return NULL;
    return wl_resource_get_user_data(buffer);
}

static const uint8_t *dmabuf_buffer_pixels(struct dmabuf_buffer *d)
{
    if (!d->map && !d->map_failed) {
        size_t len = (size_t)d->offset + (size_t)d->stride * d->height;
        void *map = mmap(NULL, len, PROT_READ, MAP_SHARED, d->fd, 0);
        if (map == MAP_FAILED) {
            WARN("cannot map Android's GPU buffer (%ux%u): %s", d->width, d->height,
                 strerror(errno));
            d->map_failed = true;
        } else {
            d->map = map;
            d->map_len = len;
        }
    }
    return d->map ? (const uint8_t *)d->map + d->offset : NULL;
}

/* ------------------------------------------------------------------------- */
/* GPU copy helper                                                           */
/* ------------------------------------------------------------------------- */

static bool gc_wanted(struct bridge *b)
{
    return b->cfg.gpu_copy == 1 && !b->gc_failed;
}

static void gc_stop(struct bridge *b)
{
    if (b->gc_sock >= 0)
        close(b->gc_sock);
    b->gc_sock = -1;
    if (b->gc_pid > 0) {
        kill(b->gc_pid, SIGKILL);
        waitpid(b->gc_pid, NULL, 0);
    }
    b->gc_pid = 0;
    b->gc_ready = false;
    b->gc_targets = 0;
}

/* Gives up on the helper for good: frames are copied with the CPU from now on. */
static void gc_disable(struct bridge *b, const char *why)
{
    WARN("GPU copy turned off (%s); frames are copied with the CPU", why);
    gc_stop(b);
    b->gc_failed = true;
}

/* One request, one reply. send_fd is passed along (not closed); a descriptor in
 * the reply is stored in *recv_fd (or closed when recv_fd is NULL). */
static bool gc_call(struct bridge *b, const struct gc_request *rq, int send_fd,
                    struct gc_reply *rp, int *recv_fd, int timeout_ms)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { (void *)rq, sizeof(*rq) };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (send_fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &send_fd, sizeof(int));
    }
    ssize_t n;
    do {
        n = sendmsg(b->gc_sock, &mh, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n != (ssize_t)sizeof(*rq)) {
        gc_disable(b, "the helper stopped");
        return false;
    }

    struct pollfd pfd = { .fd = b->gc_sock, .events = POLLIN };
    int pr;
    do {
        pr = poll(&pfd, 1, timeout_ms);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0) {
        gc_disable(b, pr == 0 ? "the helper did not answer in time" : "cannot wait for the helper");
        return false;
    }

    iov.iov_base = rp;
    iov.iov_len = sizeof(*rp);
    memset(&mh, 0, sizeof(mh));
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    do {
        n = recvmsg(b->gc_sock, &mh, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    int fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); n > 0 && c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(&fd, CMSG_DATA(c), sizeof(int));
    if (n != (ssize_t)sizeof(*rp) || rp->type != rq->type) {
        if (fd >= 0)
            close(fd);
        gc_disable(b, "the helper stopped");
        return false;
    }
    rp->text[sizeof(rp->text) - 1] = 0;
    if (recv_fd)
        *recv_fd = fd;
    else if (fd >= 0)
        close(fd);
    return true;
}

/* Started on the first GPU frame: by then Android, and with it the GPU
 * driver the helper links to, is fully up. */
static bool gc_start(struct bridge *b)
{
    const char *helper = b->cfg.gpu_copy_helper;
    if (access(helper, X_OK) != 0) {
        char why[PATH_MAX + 32];
        snprintf(why, sizeof(why), "%s is missing", helper);
        gc_disable(b, why);
        return false;
    }
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0) {
        gc_disable(b, "cannot create a socket");
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(sv[0]);
        close(sv[1]);
        gc_disable(b, "cannot start the helper");
        return false;
    }
    if (pid == 0) {
        if (sv[1] == 3)
            fcntl(3, F_SETFD, 0);
        else if (dup2(sv[1], 3) < 0)
            _exit(127);
        signal(SIGPIPE, SIG_DFL);
        execl(helper, helper, "3", (char *)NULL);
        _exit(127);
    }
    close(sv[1]);
    b->gc_sock = sv[0];
    b->gc_pid = pid;

    struct gc_request rq = { .type = GC_HELLO, .aux = GC_VERSION };
    struct gc_reply rp;
    /* The first EGL start loads the driver and may compile shaders. */
    if (!gc_call(b, &rq, -1, &rp, NULL, 15000))
        return false;
    if (rp.status != 0) {
        gc_disable(b, rp.text[0] ? rp.text : "the helper cannot use the GPU");
        return false;
    }
    b->gc_ready = true;
    INFO("GPU copy: Android's frames are copied to Anland by the GPU: %s", rp.text);
    return true;
}

static void gc_forget_source(struct bridge *b, uint32_t id)
{
    if (!b->gc_ready)
        return;
    struct gc_request rq = { .type = GC_FORGET_SOURCE, .id = id };
    struct gc_reply rp;
    gc_call(b, &rq, -1, &rp, NULL, 1000);
}

/* The Anland buffers are gone (app disconnected); the helper drops its copies. */
static void gc_reset_targets(struct bridge *b)
{
    if (!b->gc_ready || !b->gc_targets) {
        b->gc_targets = 0;
        return;
    }
    b->gc_targets = 0;
    struct gc_request rq = { .type = GC_RESET_TARGETS };
    struct gc_reply rp;
    gc_call(b, &rq, -1, &rp, NULL, 1000);
}

static uint32_t ahb_to_fourcc(uint32_t format)
{
    switch (format) {
    case AHB_FORMAT_RGBA_8888:    return DRM_FORMAT_ABGR8888;
    case AHB_FORMAT_RGBX_8888:    return DRM_FORMAT_XBGR8888;
    case AHB_FORMAT_RGBA_1010102: return FOURCC('A', 'B', '3', '0');
    default:                      return 0;
    }
}

/* Copies a GPU frame into Anland buffer idx with the GPU. Returns false when
 * the CPU has to do it; *fence is the render-done fence or -1. */
static bool gc_copy(struct bridge *b, struct dmabuf_buffer *d, struct target *t, int idx, int *fence)
{
    *fence = -1;
    if (!gc_wanted(b) || idx < 0 || idx >= 32)
        return false;
    if (!b->gc_ready && !gc_start(b))
        return false;

    struct gc_reply rp;
    if (!(b->gc_targets & (1u << idx))) {
        struct gc_request rq = {
            .type = GC_TARGET, .id = (uint32_t)idx,
            .width = t->info.width, .height = t->info.height,
            .stride = t->info.stride, .offset = t->info.offset,
            .fourcc = ahb_to_fourcc(t->info.format), .modifier = t->info.modifier,
        };
        if (!gc_call(b, &rq, t->fd, &rp, NULL, 3000))
            return false;
        if (rp.status != 0) {
            gc_disable(b, rp.text);
            return false;
        }
        b->gc_targets |= 1u << idx;
    }
    if (!d->gc_id) {
        uint32_t id = ++b->gc_next_id;
        if (!id)
            id = ++b->gc_next_id;
        struct gc_request rq = {
            .type = GC_SOURCE, .id = id,
            .width = d->width, .height = d->height,
            .stride = d->stride, .offset = d->offset,
            .fourcc = d->format, .modifier = d->modifier,
        };
        if (!gc_call(b, &rq, d->fd, &rp, NULL, 3000))
            return false;
        if (rp.status != 0) {
            gc_disable(b, rp.text);
            return false;
        }
        d->gc_id = id;
    }
    struct gc_request rq = { .type = GC_COPY, .id = d->gc_id, .aux = (uint32_t)idx };
    if (!gc_call(b, &rq, -1, &rp, fence, 2000))
        return false;
    if (rp.status != 0) {
        if (*fence >= 0)
            close(*fence);
        *fence = -1;
        gc_disable(b, rp.text);
        return false;
    }
    return true;
}

/* Copies a committed buffer of either kind. */
static bool copy_buffer(struct bridge *b, struct wl_resource *buffer, struct target *t, int idx,
                        int *fence, bool *by_gpu)
{
    *fence = -1;
    *by_gpu = false;
    struct frame_src f = { 0 };
    struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
    if (shm) {
        uint32_t fmt = wl_shm_buffer_get_format(shm);
        f.width = (uint32_t)wl_shm_buffer_get_width(shm);
        f.height = (uint32_t)wl_shm_buffer_get_height(shm);
        f.stride = (uint32_t)wl_shm_buffer_get_stride(shm);
        f.format = fmt == WL_SHM_FORMAT_ARGB8888 ? DRM_FORMAT_ARGB8888
                 : fmt == WL_SHM_FORMAT_XRGB8888 ? DRM_FORMAT_XRGB8888 : fmt;
        wl_shm_buffer_begin_access(shm);
        f.data = wl_shm_buffer_get_data(shm);
        bool ok = copy_frame(b, &f, t);
        wl_shm_buffer_end_access(shm);
        if (!ok)
            WARN("cannot convert Android frame (shm format 0x%x)", fmt);
        return ok;
    }
    struct dmabuf_buffer *d = dmabuf_buffer_get(buffer);
    if (!d)
        return false;
    if (gc_copy(b, d, t, idx, fence)) {
        *by_gpu = true;
        return true;
    }
    f.width = d->width;
    f.height = d->height;
    f.stride = d->stride;
    f.format = d->format;
    f.data = dmabuf_buffer_pixels(d);
    if (!f.data)
        return false;
    dmabuf_sync(d->fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
    if (dmabuf_bytes_per_pixel(d->format) != 4) {
        f.data = widen_to_abgr(f.data, d->width, d->height, d->stride, d->format);
        f.stride = d->width * 4;
        f.format = DRM_FORMAT_ABGR8888;
    }
    bool ok = copy_frame(b, &f, t);
    dmabuf_sync(d->fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    if (!ok)
        WARN("cannot convert Android frame (dmabuf format 0x%08x)", d->format);
    return ok;
}

static void params_destroy_request(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void params_resource_destroy(struct wl_resource *r)
{
    struct dmabuf_params *p = wl_resource_get_user_data(r);
    if (p->fd >= 0)
        close(p->fd);
    free(p);
}

static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane_idx,
                       uint32_t offset, uint32_t stride, uint32_t mod_hi, uint32_t mod_lo)
{
    struct dmabuf_params *p = wl_resource_get_user_data(r);
    (void)c;
    if (p->used) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED,
                               "params was already used to create a wl_buffer");
        return;
    }
    if (plane_idx != 0) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                               "only single-plane formats are supported");
        return;
    }
    if (p->fd >= 0) {
        close(fd);
        wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
                               "plane 0 was already set");
        return;
    }
    p->fd = fd;
    p->offset = offset;
    p->stride = stride;
    p->modifier = ((uint64_t)mod_hi << 32) | mod_lo;
}

/* Returns the new buffer, or NULL after having told the client why. */
static struct wl_resource *params_build(struct wl_client *c, struct dmabuf_params *p,
                                        uint32_t buffer_id, int32_t width, int32_t height,
                                        uint32_t format, bool immediate)
{
    const char *why = NULL;
    uint32_t code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER;
    if (p->used) {
        why = "params was already used";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED;
    } else if (p->fd < 0) {
        why = "no plane was added";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE;
    } else if (width <= 0 || height <= 0) {
        why = "invalid size";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS;
    } else if (!dmabuf_format_supported(format)) {
        why = "unsupported format";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
    } else if (p->modifier != DRM_FORMAT_MOD_LINEAR && p->modifier != DRM_FORMAT_MOD_INVALID) {
        why = "only linear buffers can be read by the CPU";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT;
    } else if (p->stride / dmabuf_bytes_per_pixel(format) < (uint32_t)width) {
        why = "stride is smaller than a row";
        code = ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS;
    }
    p->used = true;

    struct dmabuf_buffer *d = NULL;
    struct wl_resource *res = NULL;
    if (!why) {
        d = calloc(1, sizeof(*d));
        res = d ? wl_resource_create(c, &wl_buffer_interface, 1, buffer_id) : NULL;
        if (!res) {
            free(d);
            wl_client_post_no_memory(c);
            return NULL;
        }
        d->bridge = p->bridge;
        d->resource = res;
        d->fd = p->fd;
        p->fd = -1;
        d->offset = p->offset;
        d->stride = p->stride;
        d->width = (uint32_t)width;
        d->height = (uint32_t)height;
        d->format = format;
        d->modifier = p->modifier;
        wl_resource_set_implementation(res, &dmabuf_buffer_impl, d, dmabuf_buffer_resource_destroy);
        return res;
    }

    WARN("refusing Android's GPU buffer %dx%d format 0x%08x: %s", width, height, format, why);
    if (immediate)
        wl_resource_post_error(p->resource, code, "%s", why);
    else
        zwp_linux_buffer_params_v1_send_failed(p->resource);
    return NULL;
}

static void params_create(struct wl_client *c, struct wl_resource *r, int32_t width,
                          int32_t height, uint32_t format, uint32_t flags)
{
    struct dmabuf_params *p = wl_resource_get_user_data(r);
    (void)flags;
    struct wl_resource *buffer = params_build(c, p, 0, width, height, format, false);
    if (buffer)
        zwp_linux_buffer_params_v1_send_created(r, buffer);
}

static void params_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t buffer_id,
                                int32_t width, int32_t height, uint32_t format, uint32_t flags)
{
    struct dmabuf_params *p = wl_resource_get_user_data(r);
    (void)flags;
    params_build(c, p, buffer_id, width, height, format, true);
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy_request,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void linux_dmabuf_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void linux_dmabuf_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct dmabuf_params *p = calloc(1, sizeof(*p));
    struct wl_resource *res = p ? wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
                                                     wl_resource_get_version(r), id) : NULL;
    if (!res) {
        free(p);
        wl_client_post_no_memory(c);
        return;
    }
    p->bridge = wl_resource_get_user_data(r);
    p->resource = res;
    p->fd = -1;
    wl_resource_set_implementation(res, &params_impl, p, params_resource_destroy);
}

static const struct zwp_linux_dmabuf_v1_interface linux_dmabuf_impl = {
    .destroy = linux_dmabuf_destroy,
    .create_params = linux_dmabuf_create_params,
    /* The feedback requests belong to version 4; only 3 is advertised. */
    .get_default_feedback = NULL,
    .get_surface_feedback = NULL,
};

static void bind_linux_dmabuf(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, (int)version, id);
    if (!r) {
        wl_client_post_no_memory(c);
        return;
    }
    wl_resource_set_implementation(r, &linux_dmabuf_impl, data, NULL);
    for (size_t i = 0; i < sizeof(dmabuf_formats) / sizeof(dmabuf_formats[0]); i++) {
        if (version >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION) {
            zwp_linux_dmabuf_v1_send_modifier(r, dmabuf_formats[i],
                                              (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32),
                                              (uint32_t)(DRM_FORMAT_MOD_LINEAR & 0xffffffffu));
        } else {
            zwp_linux_dmabuf_v1_send_format(r, dmabuf_formats[i]);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Geometry                                                                  */
/* ------------------------------------------------------------------------- */

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
    gc_reset_targets(b);
    b->target_ready = false;
    b->first_frame_presented = false;
    b->last_delivered_serial = 0;
    b->next_present_ms = 0;
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
    if (read(fd, &value, sizeof(value)) == sizeof(value)) {
        b->target_ready = true;
        maybe_present(b);
    } else if (errno != EAGAIN && errno != EINTR) {
        DBG("buffer-ready read: %s", strerror(errno));
    }
    return 0;
}

static bool target_layout_valid(const struct buf_info *info)
{
    return info->width > 0 && info->width <= 16384 && info->height > 0 && info->height <= 16384 &&
           info->stride >= info->width * 4 && info->stride % 4 == 0 && info->offset % 4 == 0 &&
           info->modifier == 0 &&
           (info->format == AHB_FORMAT_RGBA_8888 || info->format == AHB_FORMAT_RGBX_8888 ||
            info->format == AHB_FORMAT_RGBA_1010102);
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
            return false;
        }
        if (!target_layout_valid(&t->info)) {
            WARN("unsupported Anland buffer %d layout (%ux%u stride %u format %u modifier 0x%llx offset %u)",
                 i, t->info.width, t->info.height, t->info.stride, t->info.format,
                 (unsigned long long)t->info.modifier, t->info.offset);
            return false;
        }
        uint64_t len = t->info.offset + (uint64_t)t->info.stride * (t->info.height - 1) + t->info.width * 4;
        /* Linux DMA-BUFs and the test's memfds both expose their size via SEEK_END. */
        off_t size = lseek(t->fd, 0, SEEK_END);
        if (len > SIZE_MAX || (size >= 0 && len > (uint64_t)size)) {
            WARN("Anland buffer %d is too small for the reported layout", i);
            return false;
        }
        t->len = (size_t)len;
        t->map = mmap(NULL, t->len, PROT_READ | PROT_WRITE, MAP_SHARED, t->fd, 0);
        if (t->map == MAP_FAILED) {
            WARN("cannot map Anland buffer %d (%ux%u stride %u format %u): %s. "
                 "This device's display buffers cannot be written from the CPU.",
                 i, t->info.width, t->info.height, t->info.stride, t->info.format,
                 strerror(errno));
            t->map = NULL;
            return false;
        }
        b->target_count = i + 1;
    }

    const struct buf_info *info = &b->targets[0].info;
    uint32_t w = 0, h = 0, fmt = 0, refresh = 0;
    get_screen_info(b->ctx, &w, &h, &fmt, &refresh);
    if (refresh >= 1000 && refresh <= 1000000)
        b->refresh_mhz = refresh;

    b->screen_w = info->width;
    b->screen_h = info->height;
    b->session = true;
    b->target_ready = false;

    b->ready_source = wl_event_loop_add_fd(b->loop, get_buffer_ready_fd(b->ctx),
                                           WL_EVENT_READABLE, on_buffer_ready, b);
    b->data_source = wl_event_loop_add_fd(b->loop, get_data_fd(b->ctx),
                                          WL_EVENT_READABLE, on_data, b);
    if (!b->ready_source || !b->data_source) {
        WARN("cannot watch the Anland app's descriptors: %s", strerror(errno));
        return false;
    }
    INFO("Anland app connected: %d buffers of %ux%u, stride %u, format %u, %u mHz",
         count, info->width, info->height, info->stride, info->format, b->refresh_mhz);

    set_client_size(b, scaled_dim(b->screen_w, b->cfg.render_scale),
                    scaled_dim(b->screen_h, b->cfg.render_scale), true);
    b->session_serial++;
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
    if (!b->session || !b->target_ready)
        return;

    uint64_t now = now_ms64();
    if (now < b->next_present_ms) {
        arm_tick(b, (int)(b->next_present_ms - now));
        return;
    }

    int idx = get_selected_idx_raw(b->ctx);
    if (idx < 0 || idx >= b->target_count) {
        WARN("Anland selected buffer %d which we do not have", idx);
        force_fallback(b->ctx);
        return;
    }
    struct target *target = &b->targets[idx];
    bool copied = false, by_gpu = false;
    int fence = -1;
    if (target->frame_serial != b->frame_serial) {
        if (b->top && b->top->buffer) {
            copied = copy_buffer(b, b->top->buffer, target, idx, &fence, &by_gpu);
            if (!copied)
                b->failed_copies++;
            else if (by_gpu)
                b->gpu_copies++;
            else
                b->cpu_copies++;
        }
        if (!copied)
            clear_target(target);
        target->frame_serial = b->frame_serial;
        target->has_frame = copied;
    }
    /* Anland waits for the GPU's copy on the GPU, not here. */
    if (fence >= 0)
        set_render_fence(b->ctx, fence);

    /* The app waits for one render-done message per selection, even when
     * Android hasn't started drawing or the selected buffer is up to date. */
    b->target_ready = false;
    if (trigger_refresh(b->ctx) < 0)
        return;
    b->ack_count++;
    if (target->has_frame && target->frame_serial != b->last_delivered_serial) {
        b->last_delivered_serial = target->frame_serial;
        b->delivered_frames++;
    }
    uint32_t refresh = advertised_refresh(b);
    b->next_present_ms = now + (1000000u + refresh - 1) / refresh;
    if (copied && !b->first_frame_presented) {
        INFO("presented the first Android frame to Anland (copied by the %s)",
             by_gpu ? "GPU" : "CPU");
        b->first_frame_presented = true;
    }
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
    if (s->bridge->top == s)
        s->bridge->frame_serial++;
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

    if (!wl_list_empty(&s->pending_callbacks))
        arm_tick(b, 16);
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

    if (!buffer) {
        if (s->buffer) {
            surface_set_buffer(s, NULL);
            b->frame_serial++;
        }
        return;
    }
    struct wl_shm_buffer *shm = wl_shm_buffer_get(buffer);
    struct dmabuf_buffer *dmabuf = shm ? NULL : dmabuf_buffer_get(buffer);
    if (!shm && !dmabuf) {
        WARN("Android sent a buffer of an unknown kind; ignoring it");
        wl_buffer_send_release(buffer);
        return;
    }
    int32_t bw = shm ? wl_shm_buffer_get_width(shm) : (int32_t)dmabuf->width;
    int32_t bh = shm ? wl_shm_buffer_get_height(shm) : (int32_t)dmabuf->height;
    if (bw < 8 || bh < 8) {
        wl_buffer_send_release(buffer);
        return; /* the 1x1 probe buffer the HWC uses while it measures the window */
    }
    if (!s->buffer) {
        if (shm)
            INFO("received Android frame (%dx%d, shm format 0x%x, drawn by the CPU)", bw, bh,
                 wl_shm_buffer_get_format(shm));
        else
            INFO("received Android frame (%dx%d, dmabuf format 0x%08x, drawn by the GPU)", bw, bh,
                 dmabuf->format);
    }
    surface_set_buffer(s, buffer);
    if (shm)
        b->shm_frames++;
    else
        b->dmabuf_frames++;
    b->frame_serial++;
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
        b->frame_serial++;
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

/* For older rootfs starters that leave the property unset, advertise a
 * density to the composer. New starters set it before SurfaceFlinger starts. */
static void choose_density(struct bridge *b)
{
    if (b->cfg.ui_width_dp <= 0)
        return;
    int density = display_density(b->client_w, b->client_h, b->cfg.ui_width_dp);
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
    if (s->bridge->top == s) {
        s->bridge->top = NULL;
        s->bridge->frame_serial++;
    }
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
    arm_tick(s->bridge, 10);
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

static void publish_benchmark(struct bridge *b, uint64_t now)
{
    if (!b->cfg.benchmark_path[0] || now < b->next_sample_ms)
        return;
    b->next_sample_ms = now + 500;

    char request[PATH_MAX + 16];
    snprintf(request, sizeof(request), "%s.request", b->cfg.benchmark_path);
    struct stat st;
    time_t wall = time(NULL);
    if (lstat(request, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_mtime > wall || wall - st.st_mtime > 3)
        return;

    if (!b->kgsl_open) {
        int gpu_fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
        b->kgsl_open = gpu_fd >= 0 ? 1 : -1;
        if (gpu_fd >= 0)
            close(gpu_fd);
    }
    char snapshot[PATH_MAX + 16], temporary[PATH_MAX + 16];
    snprintf(snapshot, sizeof(snapshot), "%s.stats", b->cfg.benchmark_path);
    snprintf(temporary, sizeof(temporary), "%s.stats.XXXXXX", b->cfg.benchmark_path);
    int fd = mkostemp(temporary, O_CLOEXEC);
    if (fd < 0)
        return;
    int written = dprintf(fd,
        "1 %d %" PRIu64 " %" PRIu64 " %" PRIu64 " %d %d %d %u %u %u %u %u %u"
        " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
        " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
        (int)getpid(), b->start_ms, b->session_serial, now, b->session,
        b->cfg.gpu, b->kgsl_open, b->client_w, b->client_h, b->screen_w, b->screen_h,
        b->refresh_mhz, advertised_refresh(b), b->shm_frames, b->dmabuf_frames,
        b->delivered_frames, b->ack_count, b->cpu_copies, b->gpu_copies, b->failed_copies);
    if (close(fd) < 0 || written < 0 || rename(temporary, snapshot) < 0)
        unlink(temporary);
}

static int on_tick(void *data)
{
    struct bridge *b = data;
    b->tick_due_ms = 0;

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
    publish_benchmark(b, now);
    if (g_verbose && now - b->last_stats_ms >= 5000) {
        DBG("%.1f new frames/s sent, session=%d", (b->delivered_frames - b->last_frames) * 1000.0 /
            (double)(now - b->last_stats_ms), b->session);
        b->last_stats_ms = now;
        b->last_frames = b->delivered_frames;
    }

    /* Frames and input arrive as events on their own descriptors, so with a
     * live session this timer is only a safety net and can be slow. Waking up
     * 60 times a second for nothing would keep the phone's CPU from idling. */
    int next;
    s = b->top;
    if (s && !wl_list_empty(&s->frame_callbacks))
        next = 16;
    else if (s && s->toplevel && !s->configure_sent)
        next = 50;
    else if (!b->session)
        next = 100;
    else
        next = 500;
    arm_tick(b, next);
    return 0;
}

/* Make sure the timer fires within ms from now; never postpones it. */
static void arm_tick(struct bridge *b, int ms)
{
    uint64_t due = now_ms64() + (uint64_t)ms;
    if (b->tick_due_ms && b->tick_due_ms <= due)
        return;
    b->tick_due_ms = due;
    wl_event_source_timer_update(b->tick, ms);
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
    else if (!strcmp(key, "benchmark_path"))
        snprintf(c->benchmark_path, sizeof(c->benchmark_path), "%s", value);
    else if (!strcmp(key, "default_width"))
        c->default_width = atoi(value);
    else if (!strcmp(key, "default_height"))
        c->default_height = atoi(value);
    else if (!strcmp(key, "render_scale"))
        c->render_scale = !strcmp(value, "auto") ? -1 : atoi(value);
    else if (!strcmp(key, "max_fps"))
        c->max_fps = !strcmp(value, "auto") ? -1 : atoi(value);
    else if (!strcmp(key, "gpu"))
        c->gpu = !strcmp(value, "auto") ? -1 : atoi(value) != 0;
    else if (!strcmp(key, "gpu_copy"))
        c->gpu_copy = !strcmp(value, "auto") ? -1 : atoi(value) != 0;
    else if (!strcmp(key, "gpu_copy_helper"))
        snprintf(c->gpu_copy_helper, sizeof(c->gpu_copy_helper), "%s", value);
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

/* Whether /sbin/init chose the Adreno GPU driver for Android this boot. */
static bool android_uses_gpu(void)
{
    FILE *f = fopen(ANDROID_PROP_FILE, "r");
    if (!f)
        return false;
    char line[512];
    bool gpu = false;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!strncmp(s, "ro.hardware.egl=", 16))
            gpu = !strcmp(s + 16, "kgsl");
    }
    fclose(f);
    return gpu;
}

/* "auto" picks what suits how Android draws: with the GPU the full resolution
 * at 60 frames per second; with the CPU a quarter of the pixels at 30. */
static void config_resolve_auto(struct config *c)
{
    if (c->gpu < 0)
        c->gpu = android_uses_gpu();
    bool gpu = c->gpu == 1;
    if (c->gpu_copy < 0)
        c->gpu_copy = gpu;
    if (c->render_scale < 0)
        c->render_scale = gpu ? 100 : 50;
    if (c->max_fps < 0)
        c->max_fps = gpu ? 60 : 30;
}

static void config_clamp(struct config *c)
{
    if (c->default_width < 64 || c->default_width > 16384) c->default_width = DEFAULT_DISPLAY_WIDTH;
    if (c->default_height < 64 || c->default_height > 16384) c->default_height = DEFAULT_DISPLAY_HEIGHT;
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
        "  --render_scale PCT     Android renders at this percentage of the screen (10-100,\n"
        "                         auto = 100 when Android draws with the GPU, else 50)\n"
        "  --max_fps N            upper limit for the refresh rate reported to Android\n"
        "                         (auto = 60 with the GPU, else 30)\n"
        "  --gpu auto|0|1         whether Android draws with the GPU, for the auto values\n"
        "                         (auto reads ro.hardware.egl from " ANDROID_PROP_FILE ")\n"
        "  --gpu_copy auto|0|1    copy GPU frames to Anland with the GPU (auto = when --gpu)\n"
        "  --gpu_copy_helper PATH the helper that does it (default " DEFAULT_GPU_COPY_HELPER ")\n"
        "  --wait_for_display S   seconds to wait for the Anland app before using the default size\n"
        "  --ui_width_dp N        choose Android's density so the short screen side is N dp (0 = off)\n"
        "  --log FILE             also write messages to FILE\n"
        "  --benchmark_path PATH  prefix for the on-demand benchmark files\n"
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
    snprintf(c->benchmark_path, sizeof(c->benchmark_path), "/ds-waydroid-benchmark");
    c->default_width = DEFAULT_DISPLAY_WIDTH;
    c->default_height = DEFAULT_DISPLAY_HEIGHT;
    c->render_scale = -1;
    c->max_fps = -1;
    c->gpu = -1;
    c->gpu_copy = -1;
    snprintf(c->gpu_copy_helper, sizeof(c->gpu_copy_helper), "%s", DEFAULT_GPU_COPY_HELPER);
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
    config_resolve_auto(c);
    config_clamp(c);

    if (c->log_path[0]) {
        /* Keep the report of the start before (a failed GPU start is often
         * followed by a start in CPU mode). */
        char previous[PATH_MAX + 16];
        snprintf(previous, sizeof(previous), "%s.previous", c->log_path);
        rename(c->log_path, previous);
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
    b->frame_serial = 1; /* Newly mapped Anland buffers start unwritten (serial 0). */
    b->refresh_mhz = 60000;
    b->client_w = scaled_dim((uint32_t)c->default_width, c->render_scale);
    b->client_h = scaled_dim((uint32_t)c->default_height, c->render_scale);
    b->gc_sock = -1;

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
        !wl_global_create(b->display, &wl_output_interface, 3, b, bind_output) ||
        !wl_global_create(b->display, &zwp_linux_dmabuf_v1_interface, 3, b, bind_linux_dmabuf)) {
        WARN("cannot register Wayland globals");
        return 1;
    }

    wl_event_loop_add_signal(b->loop, SIGTERM, on_signal, b);
    wl_event_loop_add_signal(b->loop, SIGINT, on_signal, b);
    b->tick = wl_event_loop_add_timer(b->loop, on_tick, b);
    arm_tick(b, 1);

    INFO("ready: Wayland socket %s, Anland socket %s, default %dx%d, scale %d%%, max %d fps, "
         "Android draws with the %s",
         path, c->anland_socket, c->default_width, c->default_height, c->render_scale,
         c->max_fps, c->gpu == 1 ? "GPU" : "CPU");

    wl_display_run(b->display);

    if (b->ctx) {
        session_teardown(b);
        disconnect(b->ctx);
    }
    wl_display_destroy_clients(b->display);
    wl_display_destroy(b->display);
    gc_stop(b);
    free(b->xmap);
    free(b);
    return 0;
}
