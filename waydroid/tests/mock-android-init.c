/*
 * Test stand-in for Android's /system/bin/init as shipped in Waydroid images.
 *
 * Without arguments it performs the same calls the Waydroid-patched first
 * stage makes and, like the real one, treats any failure as fatal. With
 * "second_stage" it writes down what the container looks like. Used to check
 * that ds-waydroid-init prepares an environment the real init accepts.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static FILE *out;
static int failures;

#define CHECKCALL(x) do { \
    if ((x) != 0) { failures++; fprintf(out, "first-stage FAIL %s: %s\n", #x, strerror(errno)); } \
    else fprintf(out, "first-stage ok   %s\n", #x); } while (0)

static void cat(const char *label, const char *path)
{
    char buf[512] = "";
    int fd = open(path, O_RDONLY);
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0)
        close(fd);
    if (n < 0)
        fprintf(out, "%s: <%s>\n", label, strerror(errno));
    else {
        buf[n] = 0;
        fprintf(out, "%s: %s%s", label, buf, n && buf[n - 1] == '\n' ? "" : "\n");
    }
}

int main(int argc, char **argv)
{
    out = fopen("/mock-android-init.log", argc > 1 ? "a" : "w");
    if (!out)
        out = stderr;
    setvbuf(out, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(out, "first-stage pid=%d\n", (int)getpid());
        CHECKCALL(mkdir("/dev/socket", 0755));
        CHECKCALL(chmod("/proc/cmdline", 0440));
        gid_t groups[] = { 3009 };
        CHECKCALL(setgroups(1, groups));
        CHECKCALL(mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11)));
        CHECKCALL(mknod("/dev/random", S_IFCHR | 0666, makedev(1, 8)));
        CHECKCALL(mknod("/dev/urandom", S_IFCHR | 0666, makedev(1, 9)));
        mknod("/dev/ptmx", S_IFCHR | 0666, makedev(5, 2));
        mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
        CHECKCALL(mount("tmpfs", "/mnt", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=1000"));
        CHECKCALL(mkdir("/mnt/vendor", 0755));
        CHECKCALL(mkdir("/mnt/product", 0755));
        CHECKCALL(mount("tmpfs", "/debug_ramdisk", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                        "mode=0755,uid=0,gid=0"));
        CHECKCALL(mount("tmpfs", "/second_stage_resources", "tmpfs",
                        MS_NOEXEC | MS_NOSUID | MS_NODEV, "mode=0755,uid=0,gid=0"));
        fprintf(out, "first-stage result: %s\n", failures ? "FATAL (real init would abort)" : "passed");
        fflush(out);
        char *const args[] = { argv[0], (char *)"second_stage", NULL };
        execv("/system/bin/init", args);
        return 1;
    }

    fprintf(out, "second-stage pid=%d arg=%s\n", (int)getpid(), argv[1]);
    fprintf(out, "env PATH=%s\n", getenv("PATH") ? getenv("PATH") : "<unset>");
    fprintf(out, "env FIRST_STAGE_STARTED_AT=%s\n",
            getenv("FIRST_STAGE_STARTED_AT") ? "set" : "<unset>");
    cat("cmdline", "/proc/cmdline");

    static const char *const nodes[] = {
        "/dev/null", "/dev/zero", "/dev/kmsg", "/dev/random", "/dev/urandom", "/dev/ptmx",
        "/dev/pts/ptmx", "/dev/console", "/dev/binder", "/dev/hwbinder", "/dev/vndbinder",
        "/dev/socket", "/run/xdg", "/run/display.sock", "/vendor/waydroid.prop", NULL
    };
    for (int i = 0; nodes[i]; i++) {
        struct stat st;
        if (stat(nodes[i], &st) < 0)
            fprintf(out, "node %-22s MISSING\n", nodes[i]);
        else
            fprintf(out, "node %-22s %s\n", nodes[i],
                    S_ISCHR(st.st_mode) ? "char device" : S_ISDIR(st.st_mode) ? "directory" :
                    S_ISSOCK(st.st_mode) ? "socket" : "file");
    }
    int fd = open("/dev/null", O_WRONLY);
    fprintf(out, "open /dev/null for writing: %s\n", fd >= 0 ? "ok" : strerror(errno));
    if (fd >= 0)
        close(fd);

    fprintf(out, "dev listing:");
    DIR *d = opendir("/dev");
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.')
            fprintf(out, " %s", e->d_name);
    if (d)
        closedir(d);
    fprintf(out, "\n");

    /* What the guest could do to the machine it runs on. Nothing is written:
     * asking whether a write would be allowed is enough. */
    fprintf(out, "can change kernel settings (/proc/sys): %s\n",
            access("/proc/sys/kernel/sysrq", W_OK) == 0 ? "YES" : "no");
    /* Android's network stack must be able to change these (they belong to
     * the container's own network namespace), or it never brings eth0 up. */
    fprintf(out, "can change network settings (/proc/sys/net): %s\n",
            access("/proc/sys/net/ipv4/tcp_fwmark_accept", W_OK) == 0 ? "YES" : "no");
    fprintf(out, "can trigger sysrq: %s\n",
            access("/proc/sysrq-trigger", W_OK) == 0 ? "YES" : "no");
    fprintf(out, "can write to /sys: %s\n", access("/sys/kernel", W_OK) == 0 ? "YES" : "no");
    FILE *status = fopen("/proc/self/status", "r");
    char sline[256];
    while (status && fgets(sline, sizeof(sline), status))
        if (!strncmp(sline, "CapBnd:", 7) || !strncmp(sline, "CapEff:", 7))
            fprintf(out, "%s", sline);
    if (status)
        fclose(status);

    /* Mount the legacy cpuset hierarchy the way Android does and list what is
     * visible in it: groups of the host must not show up. */
    mkdir("/mnt/cpuset-probe", 0755);
    if (mount("none", "/mnt/cpuset-probe", "cgroup", 0, "cpuset") == 0 ||
        mount("none", "/mnt/cpuset-probe", "cpuset", 0, NULL) == 0) {
        fprintf(out, "cpuset groups visible:");
        DIR *cg = opendir("/mnt/cpuset-probe");
        struct dirent *ce;
        while (cg && (ce = readdir(cg)))
            if (ce->d_type == DT_DIR && ce->d_name[0] != '.')
                fprintf(out, " %s", ce->d_name);
        if (cg)
            closedir(cg);
        fprintf(out, "\n");
        umount("/mnt/cpuset-probe");
    } else {
        fprintf(out, "cpuset groups visible: <cannot mount: %s>\n", strerror(errno));
    }
    cat("cgroup membership", "/proc/self/cgroup");

    FILE *prop = fopen("/vendor/waydroid.prop", "r");
    char line[256];
    while (prop && fgets(line, sizeof(line), prop))
        if (line[0] != '#')
            fprintf(out, "prop %s", line);
    if (prop)
        fclose(prop);
    fprintf(out, "second-stage report complete\n");
    fflush(out);
    for (;;)
        pause();
}
