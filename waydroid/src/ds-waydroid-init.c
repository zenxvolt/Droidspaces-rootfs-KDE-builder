/*
 * ds-waydroid-init
 *
 * PID 1 for a Waydroid Android image that is booted directly as a Droidspaces
 * container (no LXC, no Linux distribution around it).
 *
 * Waydroid normally relies on its Python tools and LXC to prepare the container
 * before Android's own /init runs. This program does that preparation instead:
 *
 *   - builds a fresh /dev that looks like the one LXC would provide, with a
 *     private binder instance (the host's binder must never be shared);
 *   - hides the host kernel command line so the phone's own androidboot.*
 *     parameters do not leak into the guest;
 *   - chooses between the Adreno GPU driver and CPU drawing;
 *   - writes /vendor/waydroid.prop from one plain-text config file;
 *   - then replaces itself with Android's /init, which stays PID 1.
 *
 * It is built fully static because nothing in an Android image can be executed
 * before Android's init has set up its linker and APEX mounts.
 *
 * Everything it does is written to /ds-waydroid-boot.log in the rootfs so a
 * failed boot can be diagnosed from the host.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/personality.h>
#include <sys/prctl.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "display-geometry.h"

#define CONFIG_PATH   "/system/etc/droidspaces-waydroid.conf"
#define LOG_PATH      "/ds-waydroid-boot.log"
#define OLD_LOG_PATH  "/ds-waydroid-boot.previous.log"
#define BOOTS_PATH    "/ds-waydroid-boots"
#define PROP_PATH     "/vendor/waydroid.prop"
#define ANDROID_INIT  "/system/bin/init"
#define ANDROID_PATH  "/product/bin:/apex/com.android.runtime/bin:/apex/com.android.art/bin:" \
                      "/system_ext/bin:/system/bin:/system/xbin:/odm/bin:/vendor/bin:/vendor/xbin"

#define BINDERFS_MAX_NAME 255
struct binderfs_device {
    char name[BINDERFS_MAX_NAME + 1];
    uint32_t major;
    uint32_t minor;
};
#define BINDER_CTL_ADD _IOWR('b', 1, struct binderfs_device)

#ifndef BINDERFS_SUPER_MAGIC
#define BINDERFS_SUPER_MAGIC 0x6c6f6f70
#endif

static FILE *g_log;
static int g_problems;

static void say(const char *level, const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fprintf(stderr, "[ds-waydroid] %s%s\n", level, line);
    if (g_log) {
        fprintf(g_log, "%s%s\n", level, line);
        fflush(g_log);
    }
}

#define INFO(...) say("", __VA_ARGS__)
#define WARN(...) do { say("WARNING: ", __VA_ARGS__); } while (0)
#define FAIL(...) do { g_problems++; say("ERROR: ", __VA_ARGS__); } while (0)

/* ------------------------------------------------------------------------- */
/* Small filesystem helpers                                                  */
/* ------------------------------------------------------------------------- */

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static bool is_mountpoint(const char *path)
{
    struct stat a, b;
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s/..", path);
    if (stat(path, &a) < 0 || stat(parent, &b) < 0)
        return false;
    return a.st_dev != b.st_dev;
}

static int write_file(const char *path, const char *text, mode_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    size_t len = strlen(text);
    ssize_t n = write(fd, text, len);
    fchmod(fd, mode);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int touch(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    close(fd);
    return 0;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return s;
}

/* ------------------------------------------------------------------------- */
/* /dev                                                                      */
/* ------------------------------------------------------------------------- */

static bool g_can_mknod;

/* Reproduce one node of the container's current /dev inside the new one.
 * Prefers a real node; if the container may not create device nodes, the
 * existing one is bind-mounted instead. Missing sources are simply skipped. */
static bool carry_node(const char *stage, const char *src_name, const char *dst_name)
{
    char src[PATH_MAX], dst[PATH_MAX];
    struct stat st;
    snprintf(src, sizeof(src), "/dev/%s", src_name);
    snprintf(dst, sizeof(dst), "%s/%s", stage, dst_name);
    if (stat(src, &st) < 0 || (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode)))
        return false;

    if (g_can_mknod && mknod(dst, st.st_mode, st.st_rdev) == 0) {
        if (chown(dst, st.st_uid, st.st_gid) < 0) { /* best effort */ }
        chmod(dst, st.st_mode & 07777);
        return true;
    }
    if (touch(dst) == 0 && mount(src, dst, NULL, MS_BIND, NULL) == 0)
        return true;
    WARN("could not provide /dev/%s: %s", dst_name, strerror(errno));
    unlink(dst);
    return false;
}

static void carry_dir(const char *stage, const char *dir_name, const char *prefix)
{
    char src_dir[PATH_MAX], dst_dir[PATH_MAX], rel[PATH_MAX];
    snprintf(src_dir, sizeof(src_dir), "/dev/%s", dir_name);
    DIR *d = opendir(src_dir);
    if (!d)
        return;
    snprintf(dst_dir, sizeof(dst_dir), "%s/%s", stage, dir_name);
    mkdir(dst_dir, 0755);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (prefix && strncmp(e->d_name, prefix, strlen(prefix)) != 0)
            continue;
        snprintf(rel, sizeof(rel), "%s/%s", dir_name, e->d_name);
        carry_node(stage, rel, rel);
    }
    closedir(d);
}

/* Top-level /dev entries that start with one of these are GPU or camera
 * nodes; they are only there when the user enabled hardware access. */
static void carry_prefixed(const char *stage)
{
    static const char *const prefixes[] = { "kgsl", "mali", "pvr", "video", "ion", "genlock", NULL };
    DIR *d = opendir("/dev");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        for (int i = 0; prefixes[i]; i++) {
            if (!strncmp(e->d_name, prefixes[i], strlen(prefixes[i]))) {
                carry_node(stage, e->d_name, e->d_name);
                break;
            }
        }
    }
    closedir(d);
}

static bool binder_node_ok(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISCHR(st.st_mode);
}

static bool binderfs_add(const char *control, const char *name)
{
    int fd = open(control, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct binderfs_device dev;
    memset(&dev, 0, sizeof(dev));
    snprintf(dev.name, sizeof(dev.name), "%s", name);
    int rc = ioctl(fd, BINDER_CTL_ADD, &dev);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc == 0 || saved == EEXIST;
}

/* Android needs three binder devices that belong to it alone. Two ways:
 *  1. mount our own binderfs instance (clean, needs mount permission);
 *  2. add three extra devices to the host's binderfs and use those, which is
 *     what Waydroid itself does on a normal Linux host. */
static bool setup_binder(const char *stage)
{
    static const char *const names[] = { "binder", "hwbinder", "vndbinder" };
    char path[PATH_MAX], link[PATH_MAX];

    snprintf(path, sizeof(path), "%s/binderfs", stage);
    mkdir(path, 0755);
    if (mount("binder", path, "binder", 0, NULL) == 0) {
        char control[PATH_MAX];
        snprintf(control, sizeof(control), "%s/binder-control", path);
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            snprintf(link, sizeof(link), "%s/%s", path, names[i]);
            if (!binder_node_ok(link) && !binderfs_add(control, names[i]))
                WARN("binderfs: cannot create %s: %s", names[i], strerror(errno));
            if (!binder_node_ok(link)) {
                ok = false;
                continue;
            }
            chmod(link, 0666);
            char target[PATH_MAX];
            snprintf(target, sizeof(target), "/dev/binderfs/%s", names[i]);
            snprintf(link, sizeof(link), "%s/%s", stage, names[i]);
            if (symlink(target, link) < 0)
                ok = false;
        }
        chmod(path, 0755);
        if (ok) {
            INFO("binder: private binderfs instance mounted");
            return true;
        }
        FAIL("binder: binderfs mounted but its devices are missing");
        return false;
    }
    int mount_errno = errno;
    rmdir(path);

    /* Fallback: private devices inside the host's binderfs. */
    static const char *const controls[] = {
        "/dev/binderfs/binder-control", "/dev/binder-control", NULL
    };
    for (int c = 0; controls[c]; c++) {
        if (!exists(controls[c]))
            continue;
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s", controls[c]);
        *strrchr(base, '/') = 0;
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            char devname[64], src[PATH_MAX];
            snprintf(devname, sizeof(devname), "dsw-%s", names[i]);
            snprintf(src, sizeof(src), "%s/%s", base, devname);
            if (!binderfs_add(controls[c], devname) || !binder_node_ok(src)) {
                ok = false;
                break;
            }
            chmod(src, 0666);
            snprintf(link, sizeof(link), "%s/%s", stage, names[i]);
            if (touch(link) < 0 || mount(src, link, NULL, MS_BIND, NULL) < 0)
                ok = false;
        }
        if (ok) {
            INFO("binder: using private devices added to the host binderfs (%s)", base);
            return true;
        }
    }

    FAIL("binder: cannot mount binderfs (%s) and no usable binder-control was found.",
         strerror(mount_errno));
    FAIL("        Android cannot start without its own binder devices. The kernel needs");
    FAIL("        CONFIG_ANDROID_BINDERFS=y, and the container must be allowed to mount");
    FAIL("        filesystems (Droidspaces privileged mode, SELinux permissive).");
    return false;
}

/* want_emulation: the caller already knows Android's first stage cannot run.
 * *emulation_out: final decision, also true when device nodes cannot be made. */
static bool build_dev(bool want_emulation, bool *emulation_out)
{
    bool first_stage_emulation = want_emulation;
    const char *stage = "/tmp/.ds-waydroid-dev";
    static const char *const basic[] = {
        "null", "zero", "full", "tty", "fuse", "ashmem", "pmsg0", "sw_sync", "uhid", NULL
    };
    /* Android's own first stage creates these itself and treats "already
     * exists" as fatal, so they are only provided when we replace that stage. */
    static const char *const first_stage_nodes[] = { "kmsg", "random", "urandom", "ptmx", NULL };

    mkdir(stage, 0755);
    if (mount("tmpfs", stage, "tmpfs", MS_NOSUID, "mode=0755,size=8m") < 0) {
        FAIL("/dev: cannot mount a tmpfs for the new /dev: %s", strerror(errno));
        return false;
    }

    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/.probe", stage);
    g_can_mknod = mknod(probe, S_IFCHR | 0600, makedev(1, 3)) == 0;
    if (g_can_mknod) {
        int fd = open(probe, O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            g_can_mknod = false; /* node created but the device cgroup refuses it */
        else
            close(fd);
    }
    unlink(probe);
    INFO("/dev: creating device nodes is %s", g_can_mknod ? "allowed" : "NOT allowed");
    if (!g_can_mknod)
        first_stage_emulation = true;
    *emulation_out = first_stage_emulation;

    for (int i = 0; basic[i]; i++)
        carry_node(stage, basic[i], basic[i]);
    carry_node(stage, "net/tun", "tun");
    carry_dir(stage, "dri", NULL);
    carry_dir(stage, "dma_heap", NULL);
    carry_prefixed(stage);

    char path[PATH_MAX];
    /* Waydroid's hardware composer runs without root and creates its Wayland
     * event FIFOs here. InputFlinger also needs the directory before it can
     * watch for them. The tmpfs must start with only guest input devices. */
    snprintf(path, sizeof(path), "%s/input", stage);
    if (mkdir(path, 0755) < 0 || chown(path, 1000, 1000) < 0 || chmod(path, 0775) < 0)
        WARN("/dev/input: cannot prepare the Waydroid input directory: %s", strerror(errno));

    snprintf(path, sizeof(path), "%s/console", stage);
    if (exists("/dev/console") && touch(path) == 0 &&
        mount("/dev/console", path, NULL, MS_BIND, NULL) < 0)
        unlink(path);

    snprintf(path, sizeof(path), "%s/pts", stage);
    mkdir(path, 0755);
    if (exists("/dev/pts/ptmx")) {
        if (mount("/dev/pts", path, NULL, MS_BIND, NULL) < 0)
            WARN("/dev/pts: cannot carry over the existing devpts: %s", strerror(errno));
    } else if (mount("devpts", path, "devpts", MS_NOSUID | MS_NOEXEC,
                     "newinstance,ptmxmode=0666,mode=0620,gid=5") < 0) {
        WARN("/dev/pts: cannot mount devpts: %s", strerror(errno));
    }

    if (first_stage_emulation) {
        for (int i = 0; first_stage_nodes[i]; i++)
            if (!carry_node(stage, first_stage_nodes[i], first_stage_nodes[i]) &&
                strcmp(first_stage_nodes[i], "ptmx") != 0)
                carry_node(stage, "null", first_stage_nodes[i]);
        snprintf(path, sizeof(path), "%s/ptmx", stage);
        if (!exists(path) && symlink("pts/ptmx", path) < 0) { /* best effort */ }
    } else {
        snprintf(path, sizeof(path), "%s/ptmx", stage);
        if (symlink("pts/ptmx", path) < 0) { /* Android creates its own if this fails */ }
    }

    bool binder_ok = setup_binder(stage);

    if (mount(stage, "/dev", NULL, MS_MOVE, NULL) < 0) {
        int move_errno = errno;
        if (mount(stage, "/dev", NULL, MS_BIND | MS_REC, NULL) < 0) {
            FAIL("/dev: cannot put the new /dev in place: move: %s, bind: %s",
                 strerror(move_errno), strerror(errno));
            return false;
        }
    }
    rmdir(stage);
    INFO("/dev: replaced with a clean Android-style tree");
    return binder_ok;
}

/* ------------------------------------------------------------------------- */
/* /proc/cmdline                                                             */
/* ------------------------------------------------------------------------- */

/* The phone's real command line carries androidboot.* for the *host* Android
 * (hardware name, slot, verified boot state ...). The guest must not import
 * those, and Android's init insists on chmod()ing /proc/cmdline, which only
 * works on a file we own. */
static bool hide_host_boot_parameters(void)
{
    bool ok = true;
    if (write_file("/tmp/.ds-waydroid-cmdline", "ds_waydroid=1\n", 0440) < 0 ||
        mount("/tmp/.ds-waydroid-cmdline", "/proc/cmdline", NULL, MS_BIND, NULL) < 0) {
        WARN("/proc/cmdline: cannot hide the host command line: %s", strerror(errno));
        ok = false;
    }
    if (exists("/proc/bootconfig")) {
        if (write_file("/tmp/.ds-waydroid-bootconfig", "", 0440) < 0 ||
            mount("/tmp/.ds-waydroid-bootconfig", "/proc/bootconfig", NULL, MS_BIND, NULL) < 0)
            WARN("/proc/bootconfig: cannot hide the host boot config: %s", strerror(errno));
    }
    if (chmod("/proc/cmdline", 0440) < 0) {
        WARN("/proc/cmdline: chmod is refused (%s)", strerror(errno));
        ok = false;
    }
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Properties                                                                */
/* ------------------------------------------------------------------------- */

enum gpu_mode { GPU_AUTO, GPU_ON, GPU_OFF };

struct settings {
    bool force_second_stage;
    bool keep_host_cmdline;
    bool ignore_errors;
    bool unprotected;
    enum gpu_mode gpu_mode;
    bool gpu_vulkan;
};

/* ------------------------------------------------------------------------- */
/* GPU                                                                       */
/* ------------------------------------------------------------------------- */

/* The Adreno driver files the builder adds to /vendor (waydroid/gpu/). */
static const char *const gpu_files_64[] = {
    "/vendor/lib64/egl/libEGL_kgsl.so", "/vendor/lib64/egl/libGLESv2_kgsl.so",
    "/vendor/lib64/libgallium_kgsl.so", "/vendor/lib64/hw/gralloc.gbm.so", NULL
};
static const char *const gpu_files_32[] = {
    "/vendor/lib/egl/libEGL_kgsl.so", "/vendor/lib/egl/libGLESv2_kgsl.so",
    "/vendor/lib/libgallium_kgsl.so", "/vendor/lib/hw/gralloc.gbm.so", NULL
};
static const char *const dma_heaps[] = {
    "/dev/dma_heap/system", "/dev/dma_heap/qcom,system", "/dev/ion", NULL
};

static bool image_needs_32bit(void);

static bool can_open(const char *path, int flags)
{
    int fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        return false;
    close(fd);
    return true;
}

static const char *missing_file(const char *const *files)
{
    for (int i = 0; files[i]; i++)
        if (!exists(files[i]))
            return files[i];
    return NULL;
}

/* Android must be able to use the GPU from every app, not only as root. The
 * nodes are our own copies when device nodes could be created, so widening
 * their mode does not touch the phone's /dev. */
static void open_up_gpu_nodes(void)
{
    if (!g_can_mknod)
        return;
    chmod("/dev/kgsl-3d0", 0666);
    for (int i = 0; dma_heaps[i]; i++)
        if (exists(dma_heaps[i]))
            chmod(dma_heaps[i], 0444);
}

/* Decides whether Android draws with the Adreno GPU (Mesa freedreno on KGSL)
 * or with the CPU (SwiftShader). Runs after /dev was rebuilt, so it sees the
 * same devices Android will. */
#define GPU_FAILED_PATH  "/data/ds-gpu-failed"
#define GPU_CRASHES_PATH "/data/ds-gpu-crashes"

static bool choose_gpu(const struct settings *st)
{
    /* Written by /system/etc/ds-gpu-guard.sh; counts crashes of this start. */
    unlink(GPU_CRASHES_PATH);
    /* Keep Android's log of the start before (ds-logcat.rc starts afresh). */
    rename("/data/ds-waydroid-logcat.txt", "/data/ds-waydroid-logcat.previous.txt");
    rename("/data/ds-waydroid-logcat.txt.1", "/data/ds-waydroid-logcat.previous.txt.1");

    if (st->gpu_mode == GPU_OFF) {
        INFO("gpu: turned off with gpu.mode=off; Android draws with the CPU");
        return false;
    }
    if (st->gpu_mode == GPU_AUTO && exists(GPU_FAILED_PATH)) {
        char why[256] = "";
        FILE *f = fopen(GPU_FAILED_PATH, "r");
        if (f) {
            if (!fgets(why, sizeof(why), f))
                why[0] = 0;
            fclose(f);
        }
        WARN("gpu: the GPU driver failed before: %s", *why ? trim(why) : "(no details)");
        WARN("gpu: Android draws with the CPU. Delete %s in the rootfs, or set gpu.mode=on,", GPU_FAILED_PATH);
        WARN("gpu: to try the GPU again.");
        return false;
    }

    const char *missing = missing_file(gpu_files_64);
    if (!missing && image_needs_32bit())
        missing = missing_file(gpu_files_32);
    if (missing) {
        INFO("gpu: %s is missing from this rootfs; Android draws with the CPU", missing);
        return false;
    }

    open_up_gpu_nodes();
    const char *problem = NULL;
    if (!exists("/dev/kgsl-3d0"))
        problem = "there is no /dev/kgsl-3d0 (a Snapdragon phone with Hardware Access "
                  "turned on in the container settings is needed)";
    else if (!can_open("/dev/kgsl-3d0", O_RDWR))
        problem = "/dev/kgsl-3d0 cannot be opened";
    else {
        bool heap = false;
        for (int i = 0; dma_heaps[i] && !heap; i++)
            heap = can_open(dma_heaps[i], O_RDONLY);
        if (!heap)
            problem = "no dma-buf heap (/dev/dma_heap/system) or /dev/ion can be opened";
    }

    if (problem && st->gpu_mode == GPU_ON) {
        WARN("gpu: %s, but gpu.mode=on uses the GPU driver anyway", problem);
        return true;
    }
    if (problem) {
        INFO("gpu: %s; Android draws with the CPU", problem);
        return false;
    }
    INFO("gpu: Adreno GPU found; Android draws with the GPU (Mesa freedreno on KGSL)");
    return true;
}

static bool config_has_key(const char *key)
{
    FILE *in = fopen(CONFIG_PATH, "r");
    if (!in)
        return false;
    char line[1024];
    size_t len = strlen(key);
    bool found = false;
    while (!found && fgets(line, sizeof(line), in)) {
        char *s = trim(line);
        found = !strncmp(s, key, len) && s[len] == '=';
    }
    fclose(in);
    return found;
}

/* Writes "key=value" unless the config file sets the key itself. */
static int default_property(FILE *out, const char *key, const char *value)
{
    if (config_has_key(key)) {
        INFO("properties: %s is set in the config file, keeping that value", key);
        return 0;
    }
    fprintf(out, "%s=%s\n", key, value);
    return 1;
}

static void write_properties(const struct settings *st, bool gpu)
{
    FILE *in = fopen(CONFIG_PATH, "r");
    FILE *out = fopen(PROP_PATH, "w");
    if (!out) {
        FAIL("cannot write %s: %s", PROP_PATH, strerror(errno));
        if (in)
            fclose(in);
        return;
    }
    fprintf(out, "# Generated on every boot by /sbin/init from %s.\n"
                 "# Edit that file instead; changes here are overwritten.\n", CONFIG_PATH);
    int count = 0;
    bool memfd_set = false;
    bool android_gpu = gpu;
    int bridge_gpu = -1, render_scale = -1, ui_width_dp = 0;
    int width = DEFAULT_DISPLAY_WIDTH, height = DEFAULT_DISPLAY_HEIGHT;
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            char *s = trim(line);
            char *eq = strchr(s, '=');
            if (!*s || *s == '#' || !eq)
                continue;
            if (!strncmp(s, "bridge.", 7)) {
                *eq = 0;
                const char *key = trim(s + 7), *value = trim(eq + 1);
                if (!strcmp(key, "default_width"))
                    width = atoi(value);
                else if (!strcmp(key, "default_height"))
                    height = atoi(value);
                else if (!strcmp(key, "ui_width_dp"))
                    ui_width_dp = atoi(value);
                else if (!strcmp(key, "render_scale"))
                    render_scale = !strcmp(value, "auto") ? -1 : atoi(value);
                else if (!strcmp(key, "gpu"))
                    bridge_gpu = !strcmp(value, "auto") ? -1 : atoi(value) != 0;
                continue;
            }
            if (!strncmp(s, "init.", 5) || !strncmp(s, "gpu.", 4))
                continue;
            if (!strncmp(s, "sys.use_memfd=", 14))
                memfd_set = true;
            if (!strncmp(s, "ro.hardware.egl=", 16))
                android_gpu = !strcmp(s + 16, "kgsl");
            fprintf(out, "%s\n", s);
            count++;
        }
        fclose(in);
    } else {
        WARN("%s is missing; using default rootfs properties", CONFIG_PATH);
    }

    /* SurfaceFlinger caches this property in its constructor, before the
     * Wayland composer asks the bridge to configure the first window. */
    if (ui_width_dp > 0 && ui_width_dp <= 2000) {
        if (width < 64 || width > 16384) width = DEFAULT_DISPLAY_WIDTH;
        if (height < 64 || height > 16384) height = DEFAULT_DISPLAY_HEIGHT;
        if (render_scale < 0) render_scale = (bridge_gpu < 0 ? android_gpu : bridge_gpu) ? 100 : 50;
        if (render_scale < 10 || render_scale > 100) render_scale = 100;
        uint32_t w = scaled_dim((uint32_t)width, render_scale);
        uint32_t h = scaled_dim((uint32_t)height, render_scale);
        char density[16];
        snprintf(density, sizeof(density), "%d", display_density(w, h, ui_width_dp));
        if (default_property(out, "ro.sf.lcd_density", density)) {
            INFO("display density: %s dpi for %ux%u (%d dp target)", density, w, h, ui_width_dp);
            count++;
        }
    }
    /* Hardware mode can expose /dev/ashmem even when Android services cannot
     * open it. Use memfd by default on both kinds of host. */
    if (!memfd_set) {
        fprintf(out, "sys.use_memfd=true\n");
        count++;
    }
    /* How Android draws. The display bridge reads ro.hardware.egl from this
     * file to pick its own defaults. */
    if (gpu) {
        count += default_property(out, "ro.hardware.gralloc", "gbm");
        /* Waydroid's init replaces gralloc "gbm" with minigbm_gbm_mesa, which
         * needs a DRM GPU (patch "init: Default to minigbm_gbm_mesa"). This
         * keeps the dma-buf heap gralloc.gbm.so that works with KGSL. */
        count += default_property(out, "gralloc.gbm.legacy", "true");
        count += default_property(out, "ro.hardware.egl", "kgsl");
        count += default_property(out, "mesa.loader.driver.override", "kgsl");
        if (st->gpu_vulkan)
            count += default_property(out, "ro.hardware.vulkan", "kgsl");
    } else {
        count += default_property(out, "ro.hardware.gralloc", "default");
        count += default_property(out, "ro.hardware.egl", "swiftshader");
    }
    fchmod(fileno(out), 0644);
    fclose(out);
    INFO("properties: wrote %d entries to %s", count, PROP_PATH);
}

/* ------------------------------------------------------------------------- */
/* Safety checks                                                             */
/* ------------------------------------------------------------------------- */

/* Many recent phone CPUs can no longer run 32-bit ARM programs. A Waydroid
 * image built for "64 and 32 bit" runs a 32-bit self test at boot and reboots
 * when it fails, which would loop forever on such a phone. */
static bool image_needs_32bit(void)
{
    return exists("/system/bin/linker") || exists("/vendor/bin/boringssl_self_test32");
}

static bool kernel_runs_32bit(void)
{
#if defined(__aarch64__)
    /* arm64 refuses the 32-bit personality with EINVAL when the CPU has no
     * 32-bit user mode. Any other answer is not a clear "no". */
    if (personality(PER_LINUX32) == -1 && errno == EINVAL)
        return false;
    personality(PER_LINUX);
#endif
    return true;
}

/* A failed Android boot ends in a reboot, and Droidspaces restarts the
 * container. Without a limit that repeats forever and drains the battery.
 * Returns false after too many starts in a short time. */
static bool boot_loop_guard(void)
{
    enum { MAX_BOOTS = 5, WINDOW_SECONDS = 180 };
    long long stamps[MAX_BOOTS + 1];
    int count = 0;
    long long now = (long long)time(NULL);

    FILE *f = fopen(BOOTS_PATH, "r");
    if (f) {
        long long t;
        while (count < MAX_BOOTS && fscanf(f, "%lld", &t) == 1)
            if (t <= now && now - t < WINDOW_SECONDS)
                stamps[count++] = t;
        fclose(f);
    }
    if (count >= MAX_BOOTS) {
        unlink(BOOTS_PATH); /* the next manual start gets a fresh try */
        return false;
    }
    stamps[count++] = now;
    f = fopen(BOOTS_PATH, "w");
    if (f) {
        for (int i = 0; i < count; i++)
            fprintf(f, "%lld\n", stamps[i]);
        fclose(f);
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* Protecting the phone from the guest                                       */
/* ------------------------------------------------------------------------- */

/* Android's boot scripts assume they own the machine: they tune the scheduler
 * and the memory manager through /proc/sys, switch on kernel panics for hung
 * tasks, rearrange the CPU groups, and init can even ask the kernel to crash
 * through /proc/sysrq-trigger. On a normal Waydroid host LXC makes all of that
 * read-only and removes a set of capabilities. In a privileged Droidspaces
 * container nothing stops it, and "the machine" is the phone itself. The
 * functions below rebuild LXC's protections (lxc.mount.auto = proc:mixed
 * sys:ro, lxc.cap.keep) so the guest runs in the environment it was made for. */

static unsigned long mount_flags_of(const char *path)
{
    struct statvfs vfs;
    unsigned long flags = 0;
    if (statvfs(path, &vfs) == 0) {
        if (vfs.f_flag & ST_NOSUID) flags |= MS_NOSUID;
        if (vfs.f_flag & ST_NODEV) flags |= MS_NODEV;
        if (vfs.f_flag & ST_NOEXEC) flags |= MS_NOEXEC;
    }
    return flags;
}

static void make_read_only(const char *path)
{
    if (!exists(path))
        return;
    unsigned long flags = mount_flags_of(path);
    if (mount(path, path, NULL, MS_BIND | MS_REC, NULL) < 0 ||
        mount(NULL, path, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY | flags, NULL) < 0) {
        WARN("%s: cannot make it read-only for Android: %s", path, strerror(errno));
        return;
    }
    INFO("protect: %s is read-only for Android", path);
}

static bool read_first_line(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(buf, (int)size, f) != NULL;
    fclose(f);
    if (ok)
        trim(buf);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text);
}

/* The phone's own Android keeps its processes in legacy cgroup hierarchies
 * (cpuset, cpu, blkio, memory). A container that sits at the root of one of
 * them sees the phone's groups (top-app, background, ...) when it mounts that
 * hierarchy, and the guest's boot scripts would rewrite them. Move into a
 * group of our own in every such hierarchy and start a new cgroup namespace,
 * so the guest only ever sees and changes its own subtree. */
static void isolate_cgroups(void)
{
    FILE *f = fopen("/proc/self/cgroup", "r");
    if (!f)
        return;
    const char *mp = "/tmp/.ds-waydroid-cgroup";
    mkdir(mp, 0755);
    char line[512];
    int moved = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        char *c1 = strchr(s, ':');
        char *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c2)
            continue;
        *c2 = 0;
        const char *controllers = c1 + 1;
        const char *path = c2 + 1;
        if (!*controllers || !strncmp(controllers, "name=", 5))
            continue; /* the unified hierarchy and named ones need nothing */

        bool is_cpuset = strstr(controllers, "cpuset") != NULL;
        bool mounted = mount("none", mp, "cgroup", MS_NOSUID | MS_NODEV | MS_NOEXEC,
                             controllers) == 0;
        if (!mounted && is_cpuset) {
            /* Android mounts cpuset through its legacy filesystem type. */
            mounted = mount("none", mp, "cpuset", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0;
        }
        if (!mounted)
            continue;

        char parent[PATH_MAX], dir[PATH_MAX + 32], file[PATH_MAX + 64], value[256];
        snprintf(parent, sizeof(parent), "%s%s", mp, strcmp(path, "/") ? path : "");
        snprintf(dir, sizeof(dir), "%s/ds-waydroid", parent);
        mkdir(dir, 0755);
        if (is_cpuset) {
            /* A new cpuset group has no CPUs until it is given some. */
            static const char *const seeds[] = { "cpuset.cpus", "cpuset.mems", "cpus", "mems" };
            for (size_t i = 0; i < sizeof(seeds) / sizeof(seeds[0]); i++) {
                snprintf(file, sizeof(file), "%s/%s", parent, seeds[i]);
                if (!read_first_line(file, value, sizeof(value)))
                    continue;
                snprintf(file, sizeof(file), "%s/%s", dir, seeds[i]);
                write_text(file, value);
            }
        }
        snprintf(file, sizeof(file), "%s/cgroup.procs", dir);
        bool ok = write_text(file, "1");
        if (!ok) {
            snprintf(file, sizeof(file), "%s/tasks", dir);
            ok = write_text(file, "1");
        }
        if (ok)
            moved++;
        else
            WARN("cgroup %s: could not move into a group of our own: %s", controllers,
                 strerror(errno));
        umount2(mp, MNT_DETACH);
    }
    fclose(f);
    rmdir(mp);
    if (unshare(CLONE_NEWCGROUP) < 0)
        WARN("cgroup: cannot start a new cgroup namespace: %s", strerror(errno));
    else
        INFO("protect: Android gets its own cgroup subtree (%d legacy hierarchies)", moved);
}

/* The capabilities Waydroid's LXC configuration keeps, plus CAP_SYS_BOOT: in a
 * PID namespace a reboot only ends the container, and that is how Android's
 * own "restart" and "power off" reach Droidspaces. Everything else (loading
 * kernel modules, raw hardware access, ...) is given up for good. */
static void drop_capabilities(void)
{
    static const int keep[] = {
        0 /* chown */, 1 /* dac_override */, 2 /* dac_read_search */, 3 /* fowner */,
        4 /* fsetid */, 5 /* kill */, 6 /* setgid */, 7 /* setuid */, 8 /* setpcap */,
        10 /* net_bind_service */, 12 /* net_admin */, 13 /* net_raw */, 14 /* ipc_lock */,
        18 /* sys_chroot */, 19 /* sys_ptrace */, 21 /* sys_admin */, 22 /* sys_boot */,
        23 /* sys_nice */, 24 /* sys_resource */, 25 /* sys_time */, 27 /* mknod */,
        30 /* audit_control */, 34 /* syslog */, 35 /* wake_alarm */, 36 /* block_suspend */,
    };
    uint64_t keep_mask = 0;
    for (size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++)
        keep_mask |= 1ULL << keep[i];

    int last = 40;
    char buf[32];
    if (read_first_line("/proc/sys/kernel/cap_last_cap", buf, sizeof(buf)))
        last = atoi(buf);
    if (last < 0 || last > 63)
        last = 40;
    for (int cap = 0; cap <= last; cap++)
        if (!(keep_mask & (1ULL << cap)))
            prctl(PR_CAPBSET_DROP, cap, 0, 0, 0);
    prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0);

    struct { uint32_t version; int pid; } header = { 0x20080522 /* v3 */, 0 };
    struct { uint32_t effective, permitted, inheritable; } data[2];
    if (syscall(SYS_capget, &header, data) == 0) {
        for (int i = 0; i < 2; i++) {
            uint32_t mask = (uint32_t)(keep_mask >> (32 * i));
            data[i].effective &= mask;
            data[i].permitted &= mask;
            data[i].inheritable &= mask;
        }
        if (syscall(SYS_capset, &header, data) < 0)
            WARN("capabilities: cannot reduce our own set: %s", strerror(errno));
    }
    INFO("protect: capabilities reduced to the set Waydroid uses");
}

static void protect_host(void)
{
    make_read_only("/proc/sys");
    make_read_only("/proc/sysrq-trigger");
    make_read_only("/sys");
    isolate_cgroups();
}

/* ------------------------------------------------------------------------- */
/* Handing over to Android                                                   */
/* ------------------------------------------------------------------------- */

static void exec_android_first_stage(void)
{
    char *const argv[] = { (char *)"/init", NULL };
    char *const envp[] = { (char *)"PATH=" ANDROID_PATH, NULL };
    INFO("starting Android: /init");
    if (g_log)
        fflush(g_log);
    execve("/init", argv, envp);
    execve(ANDROID_INIT, argv, envp);
    FAIL("cannot execute Android init: %s", strerror(errno));
}

/* Used when the container may not create device nodes: Android's first stage
 * would abort on that, so do its (small) job here and enter the second stage
 * directly. Mirrors FirstStageMain() of the Waydroid-patched init. */
static void exec_android_second_stage(void)
{
    INFO("performing Android's first-stage setup ourselves");
    umask(0);
    if (mkdir("/dev/socket", 0755) < 0 && errno != EEXIST)
        WARN("mkdir /dev/socket: %s", strerror(errno));
    gid_t readproc = 3009; /* AID_READPROC */
    if (setgroups(1, &readproc) < 0)
        WARN("setgroups: %s", strerror(errno));
    if (mount("tmpfs", "/mnt", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
              "mode=0755,uid=0,gid=1000") < 0)
        WARN("mount /mnt: %s", strerror(errno));
    mkdir("/mnt/vendor", 0755);
    mkdir("/mnt/product", 0755);
    if (mount("tmpfs", "/debug_ramdisk", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
              "mode=0755,uid=0,gid=0") < 0)
        WARN("mount /debug_ramdisk: %s", strerror(errno));
    if (mount("tmpfs", "/second_stage_resources", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
              "mode=0755,uid=0,gid=0") < 0)
        WARN("mount /second_stage_resources: %s", strerror(errno));

    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    char started[64], env_started[96];
    snprintf(started, sizeof(started), "%lld",
             (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    snprintf(env_started, sizeof(env_started), "FIRST_STAGE_STARTED_AT=%s", started);

    char *const argv[] = { (char *)ANDROID_INIT, (char *)"second_stage", NULL };
    char *const envp[] = { (char *)"PATH=" ANDROID_PATH, env_started, NULL };
    INFO("starting Android: %s second_stage", ANDROID_INIT);
    if (g_log)
        fflush(g_log);
    int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    int saved_err = -1;
    if (fd >= 0) {
        saved_err = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    execve(ANDROID_INIT, argv, envp);
    int exec_errno = errno;
    if (saved_err >= 0) {
        dup2(saved_err, STDERR_FILENO);
        close(saved_err);
    }
    FAIL("cannot execute %s: %s", ANDROID_INIT, strerror(exec_errno));
}

static void on_stop_signal(int signo)
{
    (void)signo;
    _exit(1);
}

static void report_file(const char *label, const char *path)
{
    char buf[256];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (fgets(buf, sizeof(buf), f))
        INFO("%s: %s", label, trim(buf));
    fclose(f);
}

static void report_dir(const char *label, const char *path)
{
    char names[512] = "";
    DIR *d = opendir(path);
    if (!d) {
        INFO("%s: absent", label);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        size_t used = strlen(names);
        if (used + strlen(e->d_name) + 2 >= sizeof(names))
            break;
        snprintf(names + used, sizeof(names) - used, "%s%s", used ? " " : "", e->d_name);
    }
    closedir(d);
    INFO("%s: %s", label, *names ? names : "(empty)");
}

/* A short description of the phone and of how Droidspaces set the container
 * up. It goes to the container log on the host as well, so it survives a
 * failed boot and tells whoever reads it what there is to work with. */
static void describe_environment(void)
{
    struct utsname u;
    if (uname(&u) == 0)
        INFO("kernel %s %s (%s)", u.sysname, u.release, u.machine);
    INFO("pid %d, uid %d", (int)getpid(), (int)getuid());
    report_file("device", "/sys/devices/soc0/machine");
    report_file("chip id", "/sys/devices/soc0/soc_id");
    report_file("device (devicetree)", "/proc/device-tree/model");
    report_file("gpu", "/sys/class/kgsl/kgsl-3d0/gpu_model");
    report_file("memory", "/proc/meminfo");
    {
        FILE *f = fopen("/proc/self/status", "r");
        char line[256];
        while (f && fgets(line, sizeof(line), f))
            if (!strncmp(line, "CapBnd:", 7))
                INFO("capability bounding set: %s", trim(line + 7));
        if (f)
            fclose(f);
    }
    INFO("kernel settings (/proc/sys) writable: %s, /sys writable: %s",
         access("/proc/sys/kernel/sysrq", W_OK) == 0 ? "yes" : "no",
         access("/sys/kernel", W_OK) == 0 ? "yes" : "no");
    INFO("/run/display.sock (Anland): %s",
         exists("/run/display.sock") ? "present" : "MISSING - enable Anland Display in Droidspaces");
    INFO("/dev/kgsl-3d0: %s", exists("/dev/kgsl-3d0") ? "present" : "absent");
    report_dir("/dev/dri", "/dev/dri");
    report_dir("/dev/dma_heap", "/dev/dma_heap");
    INFO("/dev/ion: %s, /dev/ashmem: %s, binderfs in kernel: %s",
         exists("/dev/ion") ? "present" : "absent", exists("/dev/ashmem") ? "present" : "absent",
         exists("/sys/module/binder") || exists("/sys/module/binder_linux") ? "likely" : "unknown");
    INFO("/proc/pressure (PSI): %s", exists("/proc/pressure/memory") ? "present" : "absent");
    {
        FILE *f = fopen("/proc/self/cgroup", "r");
        char line[256], all[1024] = "";
        while (f && fgets(line, sizeof(line), f)) {
            size_t used = strlen(all);
            snprintf(all + used, sizeof(all) - used, "%s%s", used ? " | " : "", trim(line));
        }
        if (f)
            fclose(f);
        INFO("cgroups: %s", all);
    }
}

int main(int argc, char **argv)
{
    bool check_only = getpid() != 1 || (argc > 1 && !strcmp(argv[1], "--check"));

    if (!exists("/proc/self/stat")) {
        mkdir("/proc", 0555);
        if (mount("proc", "/proc", "proc", 0, NULL) < 0) { /* reported below */ }
    }

    if (!check_only) {
        rename(LOG_PATH, OLD_LOG_PATH); /* keep the report of the boot before this one */
        /* Zygote aborts if a non-allowlisted descriptor reaches it from init. */
        g_log = fopen(LOG_PATH, "we");
    }
    INFO("ds-waydroid-init: preparing Android for Droidspaces");
    describe_environment();

    if (check_only) {
        INFO("not running as PID 1: this was only an environment report.");
        INFO("Set this program as the container's init to boot Android.");
        return 0;
    }

    if (!exists(ANDROID_INIT)) {
        FAIL("%s is missing: this rootfs is not a Waydroid Android image", ANDROID_INIT);
        goto stuck;
    }
    if (!boot_loop_guard()) {
        FAIL("Android was started 5 times within three minutes, so it keeps failing to boot.");
        FAIL("Not starting it again, to protect the battery. The report of the last attempt");
        FAIL("is in %s; Android's own messages are in the host's kernel log", OLD_LOG_PATH);
        FAIL("(run \"dmesg\" as root on the phone). Start the container again to retry.");
        goto stuck;
    }
    if (!exists("/proc/self/stat"))
        FAIL("/proc is not mounted and could not be mounted");
    if (image_needs_32bit() && !kernel_runs_32bit()) {
        FAIL("this phone's processor cannot run 32-bit programs, but this rootfs was built");
        FAIL("from the 64+32-bit Waydroid image. Use the \"arm64_only\" rootfs instead.");
    }
    if (!exists("/sys/kernel") && mount("sysfs", "/sys", "sysfs", MS_RDONLY, NULL) < 0)
        WARN("/sys is not mounted and could not be mounted: %s", strerror(errno));

    if (!is_mountpoint("/tmp") &&
        mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777") < 0) {
        FAIL("cannot mount a tmpfs on /tmp: %s (the container is not allowed to mount)",
             strerror(errno));
        goto stuck;
    }

    /* Where the bridge puts its Wayland socket for the Android composer. */
    mkdir("/run", 0755);
    mkdir("/run/xdg", 0777);
    chmod("/run/xdg", 0777);

    /* Android treats /data as its own mount; give it one. */
    if (!is_mountpoint("/data") && mount("/data", "/data", NULL, MS_BIND | MS_REC, NULL) < 0)
        WARN("/data: cannot make it a mount point: %s", strerror(errno));

    struct settings settings = { 0 };
    /* Read the init.* switches before rebuilding /dev. */
    FILE *cfg = fopen(CONFIG_PATH, "r");
    if (cfg) {
        char line[1024];
        while (fgets(line, sizeof(line), cfg)) {
            char *s = trim(line);
            if (!strcmp(s, "init.stage=second"))
                settings.force_second_stage = true;
            else if (!strcmp(s, "init.keep_host_cmdline=1"))
                settings.keep_host_cmdline = true;
            else if (!strcmp(s, "init.ignore_errors=1"))
                settings.ignore_errors = true;
            else if (!strcmp(s, "init.unprotected=1"))
                settings.unprotected = true;
            else if (!strcmp(s, "gpu.mode=on"))
                settings.gpu_mode = GPU_ON;
            else if (!strcmp(s, "gpu.mode=off"))
                settings.gpu_mode = GPU_OFF;
            else if (!strcmp(s, "gpu.vulkan=1"))
                settings.gpu_vulkan = true;
        }
        fclose(cfg);
    }

    /* Decide how Android's first stage runs before /dev is laid out, because
     * the two modes need different nodes in it. */
    bool cmdline_ok = settings.keep_host_cmdline ? chmod("/proc/cmdline", 0440) == 0
                                                 : hide_host_boot_parameters();
    bool second_stage = settings.force_second_stage || !cmdline_ok;
    bool dev_ok = build_dev(second_stage, &second_stage);
    if (second_stage && !settings.force_second_stage)
        INFO("Android's first stage cannot run in this container (%s); it will be emulated",
             cmdline_ok ? "device nodes cannot be created" : "/proc/cmdline is not adjustable");
    if (!dev_ok && !settings.ignore_errors)
        goto stuck;

    bool gpu = choose_gpu(&settings);
    write_properties(&settings, gpu);

    if (g_problems) {
        if (!settings.ignore_errors) {
            FAIL("%d problem(s) above must be fixed before Android can boot", g_problems);
            goto stuck;
        }
        WARN("%d problem(s) above, continuing anyway because init.ignore_errors=1", g_problems);
    }

    if (settings.unprotected) {
        WARN("init.unprotected=1: Android can change the phone's kernel settings");
    } else {
        protect_host();
        drop_capabilities(); /* last: everything above needs them */
    }

    INFO("preparation finished. Android's own messages go to the kernel log (dmesg)");
    INFO("and, once it is up, to logcat. The display bridge logs to /ds-waydroid-bridge.log.");

    if (second_stage)
        exec_android_second_stage();
    else
        exec_android_first_stage();

stuck:
    FAIL("Android was not started. See %s inside the rootfs for this report.", LOG_PATH);
    if (g_log)
        fflush(g_log);
    sync();
    /* Stay around so the report can be read, but leave as soon as the
     * container is asked to stop. */
    {
        static const int stop_signals[] = { SIGTERM, SIGINT, SIGHUP, SIGPWR, SIGUSR1, SIGUSR2 };
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_stop_signal;
        for (size_t i = 0; i < sizeof(stop_signals) / sizeof(stop_signals[0]); i++)
            sigaction(stop_signals[i], &sa, NULL);
        for (int sig = SIGRTMIN; sig <= SIGRTMAX; sig++)
            sigaction(sig, &sa, NULL);
    }
    for (;;)
        pause();
    return 1;
}
