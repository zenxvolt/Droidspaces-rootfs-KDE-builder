/*
 * gralloc.gbm.so for the Droidspaces Waydroid rootfs (GPU mode)
 *
 * Waydroid's own gralloc.gbm needs a DRM render node (/dev/dri/renderD*).
 * Phones do not have one: their Adreno GPU is driven through /dev/kgsl-3d0.
 * This module is a drop-in replacement that allocates every graphics buffer
 * from the kernel's dma-buf heap instead, which both sides can use:
 *
 *   - Mesa's freedreno driver imports the dma-buf into KGSL and renders into
 *     it with the GPU (the same heap freedreno uses for its own shared buffers);
 *   - Waydroid's hardware composer passes it on as a linux-dmabuf Wayland
 *     buffer, and the Anland bridge maps it and copies it to the screen.
 *
 * The buffer handle has exactly the layout of libdrm's
 * android/gralloc_handle.h ("GBM Memory Allocator"), because that is the
 * layout Waydroid's hardware composer and Mesa's u_gralloc expect when
 * ro.hardware.gralloc=gbm. All buffers are linear.
 *
 * Built with the Android NDK against Mesa's Android stub headers; it only needs
 * libc, libdl and liblog at run time.
 */
#define LOG_TAG "gralloc.dmaheap"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <android/log.h>
#include <hardware/gralloc.h>
#include <system/graphics.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

/* ------------------------------------------------------------------------- */
/* Handle layout (libdrm android/gralloc_handle.h, version 4)                */
/* ------------------------------------------------------------------------- */

struct gralloc_handle_t {
    native_handle_t base;
    int prime_fd;
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint32_t stride;   /* bytes */
    int data_owner;    /* pid that filled in "data" */
    uint64_t modifier __attribute__((aligned(8)));
    union {
        void *data;    /* here: this process's CPU mapping */
        uint64_t reserved;
    } __attribute__((aligned(8)));
};

#define GRALLOC_HANDLE_VERSION 4
#define GRALLOC_HANDLE_MAGIC 0x60585350
#define GRALLOC_HANDLE_NUM_FDS 1
#define GRALLOC_HANDLE_NUM_INTS \
    (((sizeof(struct gralloc_handle_t) - sizeof(native_handle_t)) / sizeof(int)) - GRALLOC_HANDLE_NUM_FDS)

_Static_assert(offsetof(struct gralloc_handle_t, prime_fd) == sizeof(native_handle_t),
               "the dma-buf fd must follow the native handle header");
_Static_assert(sizeof(struct gralloc_handle_t) == 64, "unexpected gralloc_handle_t size");

#define DRM_FORMAT_MOD_LINEAR 0ULL

/* Waydroid's gralloc.gbm answered these; keep the numbers. */
#define GRALLOC_MODULE_PERFORM_GET_DRM_FD 0x80000002
#define CROS_GRALLOC_DRM_GET_BUFFER_INFO 4

#define fourcc_code(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

struct cros_gralloc0_buffer_info {
    uint32_t drm_fourcc;
    int num_fds;
    int fds[4];
    uint64_t modifier;
    int offset[4];
    int stride[4];
};

/* ------------------------------------------------------------------------- */
/* dma-buf heap                                                              */
/* ------------------------------------------------------------------------- */

struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC _IOWR('H', 0x0, struct dma_heap_allocation_data)

struct ion_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
#define ION_IOC_ALLOC _IOWR('I', 0, struct ion_allocation_data)
#define ION_FLAG_CACHED 1

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ  (1 << 0)
#define DMA_BUF_SYNC_WRITE (2 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END   (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)
#define DMA_BUF_SET_NAME   _IOW('b', 1, const char *)

/* The heaps freedreno itself uses on KGSL, in the same order. */
static const char *const heap_paths[] = {
    "/dev/dma_heap/system",
    "/dev/dma_heap/qcom,system",
    NULL,
};

static int alloc_dmabuf(size_t size)
{
    for (int i = 0; heap_paths[i]; i++) {
        int heap = open(heap_paths[i], O_RDONLY | O_CLOEXEC);
        if (heap < 0)
            continue;
        struct dma_heap_allocation_data req = {
            .len = size,
            .fd_flags = O_RDWR | O_CLOEXEC,
        };
        int rc = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &req);
        int err = errno;
        close(heap);
        if (rc == 0)
            return (int)req.fd;
        LOGE("%s: allocating %zu bytes failed: %s", heap_paths[i], size, strerror(err));
    }

    /* Kernels older than 5.10 only have ION. */
    int ion = open("/dev/ion", O_RDONLY | O_CLOEXEC);
    if (ion >= 0) {
        struct ion_allocation_data req = {
            .len = size,
            /* ION_HEAP_SYSTEM (legacy id 0) and the Qualcomm system heap (25) */
            .heap_id_mask = (1u << 0) | (1u << 25),
            .flags = ION_FLAG_CACHED,
        };
        int rc = ioctl(ion, ION_IOC_ALLOC, &req);
        int err = errno;
        close(ion);
        if (rc == 0)
            return (int)req.fd;
        LOGE("/dev/ion: allocating %zu bytes failed: %s", size, strerror(err));
    }
    errno = ENOMEM;
    return -1;
}

static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    while (ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) < 0 && (errno == EINTR || errno == EAGAIN))
        ;
}

/* ------------------------------------------------------------------------- */
/* Formats and layout                                                        */
/* ------------------------------------------------------------------------- */

static int align_up(int v, int a)
{
    return (v + a - 1) & ~(a - 1);
}

static bool is_yuv420(int format)
{
    return format == HAL_PIXEL_FORMAT_YV12 || format == HAL_PIXEL_FORMAT_YCrCb_420_SP ||
           format == HAL_PIXEL_FORMAT_YCbCr_420_888;
}

static int bytes_per_pixel(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
    case HAL_PIXEL_FORMAT_RGBX_8888:
    case HAL_PIXEL_FORMAT_BGRA_8888:
    case HAL_PIXEL_FORMAT_RGBA_1010102:
        return 4;
    case HAL_PIXEL_FORMAT_RGBA_FP16:
        return 8;
    case HAL_PIXEL_FORMAT_RGB_888:
        return 3;
    case HAL_PIXEL_FORMAT_RGB_565:
        return 2;
    case HAL_PIXEL_FORMAT_BLOB:
    case HAL_PIXEL_FORMAT_YV12:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
    case HAL_PIXEL_FORMAT_Y8:
        return 1;
    case HAL_PIXEL_FORMAT_Y16:
    case HAL_PIXEL_FORMAT_RAW16:
        return 2;
    default:
        return 0;
    }
}

static uint32_t drm_fourcc(int format)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:    return fourcc_code('A', 'B', '2', '4');
    case HAL_PIXEL_FORMAT_RGBX_8888:    return fourcc_code('X', 'B', '2', '4');
    case HAL_PIXEL_FORMAT_BGRA_8888:    return fourcc_code('A', 'R', '2', '4');
    case HAL_PIXEL_FORMAT_RGB_888:      return fourcc_code('B', 'G', '2', '4');
    case HAL_PIXEL_FORMAT_RGB_565:      return fourcc_code('R', 'G', '1', '6');
    case HAL_PIXEL_FORMAT_RGBA_1010102: return fourcc_code('A', 'B', '3', '0');
    case HAL_PIXEL_FORMAT_RGBA_FP16:    return fourcc_code('A', 'B', '4', 'H');
    case HAL_PIXEL_FORMAT_YV12:         return fourcc_code('Y', 'V', '1', '2');
    case HAL_PIXEL_FORMAT_YCrCb_420_SP: return fourcc_code('N', 'V', '2', '1');
    case HAL_PIXEL_FORMAT_YCbCr_420_888: return fourcc_code('N', 'V', '1', '2');
    default:                            return 0;
    }
}

/* Adreno renders to linear surfaces whose pitch is a multiple of 64 pixels
 * (freedreno: pitchalign = cpp_shift + 6). Every buffer follows that rule so
 * any of them can become a render target. */
#define PITCH_ALIGN_PIXELS 64

/* Chroma stride of YV12 as Android defines it. */
static int yv12_c_stride(int y_stride)
{
    return align_up(y_stride / 2, 16);
}

static size_t buffer_size(int format, int width, int height, int stride_bytes)
{
    switch (format) {
    case HAL_PIXEL_FORMAT_YV12:
        return (size_t)stride_bytes * height + 2 * (size_t)yv12_c_stride(stride_bytes) * ((height + 1) / 2);
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
        return (size_t)stride_bytes * height + (size_t)stride_bytes * ((height + 1) / 2);
    case HAL_PIXEL_FORMAT_BLOB:
        return (size_t)width;
    default: {
        /* Freedreno's A6xx and later import layout includes complete 16x4
         * GMEM blit blocks. Back the final block without changing the logical
         * height recorded in the handle or exposed to Android. */
        size_t rows = ((size_t)height + 3) & ~(size_t)3;
        return (size_t)stride_bytes * rows;
    }
    }
}

static int resolve_format(int format, int usage)
{
    if (format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED) {
        if (usage & (GRALLOC_USAGE_HW_VIDEO_ENCODER | GRALLOC_USAGE_HW_CAMERA_WRITE))
            return HAL_PIXEL_FORMAT_YCbCr_420_888;
        return HAL_PIXEL_FORMAT_RGBX_8888;
    }
    return format;
}

/* ------------------------------------------------------------------------- */
/* Handles                                                                   */
/* ------------------------------------------------------------------------- */

static struct gralloc_handle_t *to_handle(buffer_handle_t h)
{
    struct gralloc_handle_t *hnd = (struct gralloc_handle_t *)h;
    if (!hnd || hnd->base.version != sizeof(native_handle_t) ||
        hnd->base.numFds != GRALLOC_HANDLE_NUM_FDS ||
        hnd->base.numInts != (int)GRALLOC_HANDLE_NUM_INTS ||
        hnd->magic != GRALLOC_HANDLE_MAGIC)
        return NULL;
    return hnd;
}

static size_t handle_size(const struct gralloc_handle_t *hnd)
{
    size_t size = buffer_size((int)hnd->format, (int)hnd->width, (int)hnd->height, (int)hnd->stride);
    long page = sysconf(_SC_PAGESIZE);
    return (size + (size_t)page - 1) & ~((size_t)page - 1);
}

/* Each process maps a buffer once, on its first lock. The mapping lives in the
 * handle's "data" slot; data_owner tells whether it belongs to this process
 * (a handle received from another process carries that process's values). */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void *mapping_of(struct gralloc_handle_t *hnd)
{
    if (hnd->data_owner == getpid() && hnd->data)
        return hnd->data;
    size_t size = handle_size(hnd);
    void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, hnd->prime_fd, 0);
    if (map == MAP_FAILED) {
        map = mmap(NULL, size, PROT_READ, MAP_SHARED, hnd->prime_fd, 0);
        if (map == MAP_FAILED) {
            LOGE("cannot map buffer %ux%u: %s", hnd->width, hnd->height, strerror(errno));
            return NULL;
        }
    }
    hnd->data = map;
    hnd->data_owner = getpid();
    return map;
}

static void unmap_handle(struct gralloc_handle_t *hnd)
{
    if (hnd->data_owner == getpid() && hnd->data)
        munmap(hnd->data, handle_size(hnd));
    hnd->data = NULL;
    hnd->data_owner = 0;
}

/* ------------------------------------------------------------------------- */
/* gralloc_module_t                                                          */
/* ------------------------------------------------------------------------- */

static int mod_register_buffer(const gralloc_module_t *mod, buffer_handle_t handle)
{
    (void)mod;
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd)
        return -EINVAL;
    pthread_mutex_lock(&g_lock);
    if (hnd->data_owner != getpid()) {
        hnd->data = NULL;
        hnd->data_owner = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static int mod_unregister_buffer(const gralloc_module_t *mod, buffer_handle_t handle)
{
    (void)mod;
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd)
        return -EINVAL;
    pthread_mutex_lock(&g_lock);
    unmap_handle(hnd);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

static uint64_t sync_flags(int usage)
{
    uint64_t flags = 0;
    if (usage & GRALLOC_USAGE_SW_READ_MASK)
        flags |= DMA_BUF_SYNC_READ;
    if (usage & GRALLOC_USAGE_SW_WRITE_MASK)
        flags |= DMA_BUF_SYNC_WRITE;
    return flags ? flags : DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE;
}

static int mod_lock(const gralloc_module_t *mod, buffer_handle_t handle, int usage,
                    int x, int y, int w, int h, void **vaddr)
{
    (void)mod; (void)x; (void)y; (void)w; (void)h;
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd || !vaddr)
        return -EINVAL;
    pthread_mutex_lock(&g_lock);
    void *map = mapping_of(hnd);
    pthread_mutex_unlock(&g_lock);
    if (!map)
        return -ENOMEM;
    dmabuf_sync(hnd->prime_fd, DMA_BUF_SYNC_START | sync_flags(usage));
    hnd->usage = (hnd->usage & ~0xFFu) | ((uint32_t)usage & 0xFFu); /* remember for unlock */
    *vaddr = map;
    return 0;
}

static int mod_unlock(const gralloc_module_t *mod, buffer_handle_t handle)
{
    (void)mod;
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd)
        return -EINVAL;
    dmabuf_sync(hnd->prime_fd, DMA_BUF_SYNC_END | sync_flags((int)(hnd->usage & 0xFFu)));
    return 0;
}

static int mod_lock_ycbcr(const gralloc_module_t *mod, buffer_handle_t handle, int usage,
                          int x, int y, int w, int h, struct android_ycbcr *ycbcr)
{
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd || !ycbcr || !is_yuv420((int)hnd->format))
        return -EINVAL;
    void *map = NULL;
    int rc = mod_lock(mod, handle, usage, x, y, w, h, &map);
    if (rc)
        return rc;
    uint8_t *base = map;
    size_t y_size = (size_t)hnd->stride * hnd->height;
    memset(ycbcr, 0, sizeof(*ycbcr));
    ycbcr->y = base;
    ycbcr->ystride = hnd->stride;
    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_YV12: {
        size_t c_stride = (size_t)yv12_c_stride((int)hnd->stride);
        ycbcr->cstride = c_stride;
        ycbcr->chroma_step = 1;
        ycbcr->cr = base + y_size;
        ycbcr->cb = base + y_size + c_stride * ((hnd->height + 1) / 2);
        break;
    }
    case HAL_PIXEL_FORMAT_YCrCb_420_SP: /* NV21 */
        ycbcr->cstride = hnd->stride;
        ycbcr->chroma_step = 2;
        ycbcr->cr = base + y_size;
        ycbcr->cb = base + y_size + 1;
        break;
    default: /* YCbCr_420_888, stored as NV12 */
        ycbcr->cstride = hnd->stride;
        ycbcr->chroma_step = 2;
        ycbcr->cb = base + y_size;
        ycbcr->cr = base + y_size + 1;
        break;
    }
    return 0;
}

static int mod_perform(const struct gralloc_module_t *mod, int op, ...)
{
    (void)mod;
    int err = -EINVAL;
    va_list args;
    va_start(args, op);
    if ((uint32_t)op == CROS_GRALLOC_DRM_GET_BUFFER_INFO) {
        buffer_handle_t handle = va_arg(args, buffer_handle_t);
        struct cros_gralloc0_buffer_info *info = va_arg(args, struct cros_gralloc0_buffer_info *);
        struct gralloc_handle_t *hnd = to_handle(handle);
        if (hnd && info) {
            memset(info, 0, sizeof(*info));
            info->drm_fourcc = drm_fourcc((int)hnd->format);
            info->num_fds = 1;
            info->fds[0] = hnd->prime_fd;
            info->modifier = hnd->modifier;
            info->stride[0] = (int)hnd->stride;
            err = 0;
        }
    }
    /* GRALLOC_MODULE_PERFORM_GET_DRM_FD: there is no DRM device. */
    va_end(args);
    return err;
}

/* ------------------------------------------------------------------------- */
/* alloc_device_t                                                            */
/* ------------------------------------------------------------------------- */

static int dev_alloc(alloc_device_t *dev, int w, int h, int format, int usage,
                     buffer_handle_t *out, int *out_stride)
{
    (void)dev;
    if (!out || !out_stride || w <= 0 || h <= 0)
        return -EINVAL;

    int fmt = resolve_format(format, usage);
    int bpp = bytes_per_pixel(fmt);
    if (!bpp) {
        LOGE("unsupported pixel format 0x%x", format);
        return -EINVAL;
    }

    int stride_px, stride_bytes;
    if (fmt == HAL_PIXEL_FORMAT_BLOB) {
        stride_px = w;
        stride_bytes = w;
        h = 1;
    } else {
        stride_px = align_up(w, PITCH_ALIGN_PIXELS);
        stride_bytes = stride_px * bpp;
    }
    size_t size = buffer_size(fmt, w, h, stride_bytes);
    long page = sysconf(_SC_PAGESIZE);
    size = (size + (size_t)page - 1) & ~((size_t)page - 1);

    int fd = alloc_dmabuf(size);
    if (fd < 0)
        return -errno;
    ioctl(fd, DMA_BUF_SET_NAME, "waydroid-gralloc"); /* only for debugging */

    struct gralloc_handle_t *hnd = calloc(1, sizeof(*hnd));
    if (!hnd) {
        close(fd);
        return -ENOMEM;
    }
    hnd->base.version = sizeof(native_handle_t);
    hnd->base.numFds = GRALLOC_HANDLE_NUM_FDS;
    hnd->base.numInts = GRALLOC_HANDLE_NUM_INTS;
    hnd->prime_fd = fd;
    hnd->magic = GRALLOC_HANDLE_MAGIC;
    hnd->version = GRALLOC_HANDLE_VERSION;
    hnd->width = (uint32_t)w;
    hnd->height = (uint32_t)h;
    hnd->format = (uint32_t)fmt;
    hnd->usage = (uint32_t)usage;
    hnd->stride = (uint32_t)stride_bytes;
    hnd->modifier = DRM_FORMAT_MOD_LINEAR;

    *out = &hnd->base;
    *out_stride = stride_px;
    return 0;
}

static int dev_free(alloc_device_t *dev, buffer_handle_t handle)
{
    (void)dev;
    struct gralloc_handle_t *hnd = to_handle(handle);
    if (!hnd)
        return -EINVAL;
    pthread_mutex_lock(&g_lock);
    unmap_handle(hnd);
    pthread_mutex_unlock(&g_lock);
    close(hnd->prime_fd);
    free(hnd);
    return 0;
}

static int dev_close(struct hw_device_t *dev)
{
    free(dev);
    return 0;
}

static int mod_open(const struct hw_module_t *module, const char *name, struct hw_device_t **device)
{
    if (strcmp(name, GRALLOC_HARDWARE_GPU0) != 0)
        return -EINVAL;
    alloc_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -ENOMEM;
    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = dev_close;
    dev->alloc = dev_alloc;
    dev->free = dev_free;
    *device = &dev->common;
    LOGI("dma-buf heap allocator opened");
    return 0;
}

static struct hw_module_methods_t mod_methods = {
    .open = mod_open,
};

/* The name is how Mesa's u_gralloc recognises the libdrm handle layout. */
__attribute__((visibility("default")))
struct gralloc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .version_major = 1,
        .version_minor = 0,
        .id = GRALLOC_HARDWARE_MODULE_ID,
        .name = "GBM Memory Allocator",
        .author = "Droidspaces Waydroid rootfs (dma-buf heap)",
        .methods = &mod_methods,
    },
    .registerBuffer = mod_register_buffer,
    .unregisterBuffer = mod_unregister_buffer,
    .lock = mod_lock,
    .unlock = mod_unlock,
    .perform = mod_perform,
    .lock_ycbcr = mod_lock_ycbcr,
};
