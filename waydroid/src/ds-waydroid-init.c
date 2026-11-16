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
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

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

struct settings {
    bool force_second_stage;
    bool keep_host_cmdline;
    bool ignore_errors;
};

static void write_properties(void)
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
    if (in) {
        char line[1024];
        while (fgets(line, sizeof(line), in)) {
            char *s = trim(line);
            if (!*s || *s == '#' || !strchr(s, '='))
                continue;
            if (!strncmp(s, "bridge.", 7))
                continue;
            if (!strncmp(s, "init.", 5))
                continue;
            if (!strncmp(s, "sys.use_memfd=", 14))
                memfd_set = true;
            fprintf(out, "%s\n", s);
            count++;
        }
        fclose(in);
    } else {
        WARN("%s is missing, Android will start with its built-in defaults", CONFIG_PATH);
    }
    /* Kernels without the old ashmem driver need Android to use memfd. */
    if (!memfd_set && !exists("/dev/ashmem")) {
        fprintf(out, "sys.use_memfd=true\n");
        count++;
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
    int saved_err = dup(STDERR_FILENO);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    execve(ANDROID_INIT, argv, envp);
    if (saved_err >= 0)
        dup2(saved_err, STDERR_FILENO);
    FAIL("cannot execute %s: %s", ANDROID_INIT, strerror(errno));
}

static void on_stop_signal(int signo)
{
    (void)signo;
    _exit(1);
}

static void describe_environment(void)
{
    struct utsname u;
    if (uname(&u) == 0)
        INFO("kernel %s %s (%s)", u.sysname, u.release, u.machine);
    INFO("pid %d, uid %d", (int)getpid(), (int)getuid());
    INFO("/run/display.sock (Anland): %s",
         exists("/run/display.sock") ? "present" : "MISSING - enable Anland Display in Droidspaces");
    INFO("/dev/kgsl-3d0: %s, /dev/dri: %s", exists("/dev/kgsl-3d0") ? "present" : "absent",
         exists("/dev/dri") ? "present" : "absent");
    INFO("/proc/pressure (PSI): %s", exists("/proc/pressure/memory") ? "present" : "absent");
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
        g_log = fopen(LOG_PATH, "w");
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
    /* The properties are written after /dev is rebuilt so the ashmem check
     * sees what Android will see; read the init.* switches first. */
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

    write_properties();

    if (g_problems) {
        if (!settings.ignore_errors) {
            FAIL("%d problem(s) above must be fixed before Android can boot", g_problems);
            goto stuck;
        }
        WARN("%d problem(s) above, continuing anyway because init.ignore_errors=1", g_problems);
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
