/*
 * Test stand-in for the Waydroid hardware composer.
 *
 * It talks to the bridge with the same sequence of Wayland requests the real
 * hwcomposer.waydroid uses in its single-surface software mode: an xdg
 * toplevel, a 1x1 probe buffer while it learns the window size, then full
 * frames in ARGB8888 shared memory without frame callbacks.
 *
 * With FAKE_HWC_DMABUF=1 it behaves like the composer in GPU mode
 * (ro.hardware.gralloc=gbm) instead: frames are single-plane linear
 * ABGR8888 linux-dmabuf buffers whose rows are padded to 64 pixels, created
 * with create_immed. A memfd stands in for the dma-buf.
 *
 * Every frame is a known gradient so the receiving side can check the picture:
 *   red = column * 255 / width, green = row * 255 / height, blue = 200.
 *
 * Usage: fake-waydroid-hwc FRAMES [HOLD_SECONDS]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"

#define DRM_FORMAT_ABGR8888 0x34324241u /* 'AB24' */
#define DRM_FORMAT_RGB565   0x36314752u /* 'RG16' */

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct xdg_wm_base *wm_base;
static struct wl_seat *seat;
static struct wl_output *output;
static int configured, req_w, req_h, out_w, out_h, out_refresh;
static int have_subcompositor, have_viewporter, have_dmabuf;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static int use_dmabuf, dmabuf_abgr_linear, use_565;

static void dmabuf_format(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t format)
{
    (void)d; (void)z; (void)format;
}
static void dmabuf_modifier(void *d, struct zwp_linux_dmabuf_v1 *z, uint32_t format,
                            uint32_t hi, uint32_t lo)
{
    (void)d; (void)z;
    if (format == DRM_FORMAT_ABGR8888 && hi == 0 && lo == 0)
        dmabuf_abgr_linear = 1;
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = { dmabuf_format, dmabuf_modifier };

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h,
                               struct wl_array *states)
{
    (void)d; (void)t; (void)states;
    if (w > 1 && h > 1) {
        if (w != req_w || h != req_h)
            printf("hwc: configure %dx%d\n", w, h);
        req_w = w;
        req_h = h;
    }
}
static void toplevel_close(void *d, struct xdg_toplevel *t) { (void)d; (void)t; }
static const struct xdg_toplevel_listener toplevel_listener = { toplevel_configure, toplevel_close };

static void xsurface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    configured = 1;
}
static const struct xdg_surface_listener xsurface_listener = { xsurface_configure };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{
    (void)d;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = { wm_ping };

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                         int32_t ph, int32_t sp, const char *mk, const char *md, int32_t tr)
{
    (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sp; (void)mk; (void)md; (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
    (void)d; (void)o; (void)flags;
    out_w = w; out_h = h; out_refresh = r;
    printf("hwc: output event %dx%d @ %d mHz\n", w, h, r);
}
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)d; (void)o; (void)s; }
static const struct wl_output_listener output_listener = { out_geometry, out_mode, out_done, out_scale };

static void touch_down(void *d, struct wl_touch *t, uint32_t serial, uint32_t time,
                       struct wl_surface *s, int32_t id, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)t; (void)serial; (void)time;
    printf("hwc: touch down id=%d x=%.0f y=%.0f surface=%s\n", id, wl_fixed_to_double(x),
           wl_fixed_to_double(y), s ? "ok" : "none");
}
static void touch_up(void *d, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)d; (void)t; (void)serial; (void)time;
    printf("hwc: touch up id=%d\n", id);
}
static void touch_motion(void *d, struct wl_touch *t, uint32_t time, int32_t id,
                         wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)t; (void)time; (void)id; (void)x; (void)y;
}
static void touch_frame(void *d, struct wl_touch *t) { (void)d; (void)t; }
static void touch_cancel(void *d, struct wl_touch *t) { (void)d; (void)t; }
static const struct wl_touch_listener touch_listener = {
    touch_down, touch_up, touch_motion, touch_frame, touch_cancel, NULL, NULL
};

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t format, int fd, uint32_t size)
{
    (void)d; (void)k;
    printf("hwc: keymap format=%u size=%u\n", format, size);
    close(fd);
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                     struct wl_array *keys)
{
    (void)d; (void)k; (void)serial; (void)keys;
    printf("hwc: keyboard enter surface=%s\n", s ? "ok" : "none");
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
    (void)d; (void)k; (void)serial; (void)s;
}
static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
                   uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)time;
    printf("hwc: key %u %s\n", key, state ? "down" : "up");
}
static void kb_mods(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t a, uint32_t b,
                    uint32_t c, uint32_t g)
{
    (void)d; (void)k; (void)serial; (void)a; (void)b; (void)c; (void)g;
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
    (void)d; (void)k; (void)rate; (void)delay;
}
static const struct wl_keyboard_listener kb_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_mods, kb_repeat
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    printf("hwc: seat capabilities touch=%d keyboard=%d pointer=%d\n",
           !!(caps & WL_SEAT_CAPABILITY_TOUCH), !!(caps & WL_SEAT_CAPABILITY_KEYBOARD),
           !!(caps & WL_SEAT_CAPABILITY_POINTER));
    if (caps & WL_SEAT_CAPABILITY_TOUCH)
        wl_touch_add_listener(wl_seat_get_touch(s), &touch_listener, NULL);
    if (caps & WL_SEAT_CAPABILITY_KEYBOARD)
        wl_keyboard_add_listener(wl_seat_get_keyboard(s), &kb_listener, NULL);
}
static void seat_name(void *d, struct wl_seat *s, const char *name) { (void)d; (void)s; (void)name; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void global_add(void *d, struct wl_registry *reg, uint32_t id, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, "wl_compositor"))
        compositor = wl_registry_bind(reg, id, &wl_compositor_interface, ver < 5 ? ver : 5);
    else if (!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    else if (!strcmp(iface, "xdg_wm_base")) {
        wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    } else if (!strcmp(iface, "wl_seat")) {
        seat = wl_registry_bind(reg, id, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, "wl_output")) {
        output = wl_registry_bind(reg, id, &wl_output_interface, ver < 3 ? ver : 3);
        wl_output_add_listener(output, &output_listener, NULL);
    } else if (!strcmp(iface, "wl_subcompositor"))
        have_subcompositor = 1;
    else if (!strcmp(iface, "wp_viewporter"))
        have_viewporter = 1;
    else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) {
        have_dmabuf = 1;
        if (use_dmabuf && ver >= 3) {
            dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 3);
            zwp_linux_dmabuf_v1_add_listener(dmabuf, &dmabuf_listener, NULL);
        }
    }
}
static void global_remove(void *d, struct wl_registry *reg, uint32_t id) { (void)d; (void)reg; (void)id; }
static const struct wl_registry_listener registry_listener = { global_add, global_remove };

struct frame {
    struct wl_buffer *buffer;
    uint32_t *px;
    int w, h, pitch; /* pitch in pixels */
    size_t size;
    int abgr;
    int rgb565;
};

static int make_frame(struct frame *f, int w, int h, int gpu)
{
    f->w = w; f->h = h;
    f->abgr = gpu;
    f->rgb565 = gpu && use_565;
    f->pitch = gpu ? (w + 63) & ~63 : w; /* like the dma-buf gralloc */
    f->size = (size_t)f->pitch * h * (f->rgb565 ? 2 : 4);
    int fd = memfd_create("buffer", MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)f->size) < 0)
        return -1;
    f->px = mmap(NULL, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (gpu) {
        struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add(params, fd, 0, 0, (uint32_t)f->pitch * (f->rgb565 ? 2 : 4), 0, 0);
        f->buffer = zwp_linux_buffer_params_v1_create_immed(params, w, h,
                        f->rgb565 ? DRM_FORMAT_RGB565 : DRM_FORMAT_ABGR8888, 0);
        zwp_linux_buffer_params_v1_destroy(params);
    } else {
        struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)f->size);
        f->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
    }
    close(fd);
    return 0;
}

static void paint(struct frame *f)
{
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++) {
            uint32_t r = (uint32_t)(x * 255 / f->w), g = (uint32_t)(y * 255 / f->h), b = 200;
            if (f->rgb565) {
                /* 5/6/5 bits; the receiving side allows for the lost precision */
                ((uint16_t *)f->px)[y * f->pitch + x] =
                    (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
                continue;
            }
            /* alpha deliberately 0: the bridge must still show an opaque screen */
            f->px[y * f->pitch + x] = f->abgr ? (b << 16) | (g << 8) | r : (r << 16) | (g << 8) | b;
        }
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    int hold_seconds = argc > 2 ? atoi(argv[2]) : 0;
    setvbuf(stdout, NULL, _IOLBF, 0);
    use_dmabuf = getenv("FAKE_HWC_DMABUF") && !strcmp(getenv("FAKE_HWC_DMABUF"), "1");
    use_565 = getenv("FAKE_HWC_FORMAT") && !strcmp(getenv("FAKE_HWC_FORMAT"), "565");

    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) {
        fprintf(stderr, "hwc: cannot connect to the Wayland display\n");
        return 1;
    }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!compositor || !shm || !wm_base || !seat || !output) {
        fprintf(stderr, "hwc: a required global is missing\n");
        return 1;
    }
    printf("hwc: globals ok; subcompositor=%d viewporter=%d dmabuf=%d\n",
           have_subcompositor, have_viewporter, have_dmabuf);
    if (use_dmabuf) {
        if (!dmabuf || !dmabuf_abgr_linear) {
            fprintf(stderr, "hwc: linear ABGR8888 dmabuf buffers are not offered\n");
            return 1;
        }
        printf("hwc: GPU mode, sending linear ABGR8888 dmabufs\n");
    }
    printf("hwc: output mode %dx%d @ %d mHz\n", out_w, out_h, out_refresh);

    struct wl_surface *surface = wl_compositor_create_surface(compositor);
    struct xdg_surface *xs = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xs, &xsurface_listener, NULL);
    struct xdg_toplevel *top = xdg_surface_get_toplevel(xs);
    xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
    xdg_toplevel_set_maximized(top);
    xdg_toplevel_set_title(top, "Waydroid");
    xdg_toplevel_set_app_id(top, "Waydroid");
    wl_surface_commit(surface);
    do {
        wl_display_roundtrip(dpy);
    } while (!configured);

    /* 1x1 probe buffer, exactly like the HWC's "calibration" step. */
    struct frame probe;
    make_frame(&probe, 1, 1, 0);
    wl_surface_attach(surface, probe.buffer, 0, 0);
    wl_surface_commit(surface);
    wl_display_roundtrip(dpy);
    if (!req_w || !req_h) { req_w = out_w; req_h = out_h; }
    xdg_surface_set_window_geometry(xs, 0, 0, req_w, req_h);
    struct wl_region *region = wl_compositor_create_region(compositor);
    wl_region_add(region, 0, 0, req_w, req_h);
    wl_surface_set_opaque_region(surface, region);
    wl_region_destroy(region);
    wl_surface_commit(surface);

    struct frame f = { 0 };
    for (int i = 0; i < frames; i++) {
        if (!f.buffer || f.w != req_w || f.h != req_h) {
            if (f.buffer) {
                wl_buffer_destroy(f.buffer);
                munmap(f.px, f.size);
            }
            make_frame(&f, req_w, req_h, use_dmabuf);
            printf("hwc: drawing at %dx%d\n", req_w, req_h);
        }
        paint(&f);
        wl_surface_attach(surface, f.buffer, 0, 0);
        wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
        wl_surface_commit(surface);
        wl_display_flush(dpy);
        usleep(16000);
        wl_display_roundtrip(dpy);
    }
    /* Leave the last committed buffer on screen without producing new frames. */
    sleep(hold_seconds);
    printf("hwc: done\n");
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xs);
    wl_surface_destroy(surface);
    wl_display_roundtrip(dpy);
    wl_display_disconnect(dpy);
    return 0;
}
