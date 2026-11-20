/*
 * anland-gpu-copy
 *
 * Copies Android's frames into the Anland app's screen buffers with the GPU,
 * on behalf of anland-waydroid-bridge (see waydroid/src/gpu-copy-protocol.h).
 *
 * Both sides are dma-bufs: the frame comes from the dma-buf heap gralloc, the
 * screen buffer from the Anland app. Each is imported once as an EGLImage
 * (EGL_EXT_image_dma_buf_import); a frame is then one textured draw into the
 * screen buffer, which also scales it when Android renders below the screen
 * resolution and forces it opaque. The render-done fence goes back to the
 * bridge, which hands it to Anland, so neither side waits on the CPU.
 *
 * Built with the Android NDK; links Android's libEGL and libGLESv2, which load
 * Mesa freedreno for KGSL (ro.hardware.egl=kgsl). It also builds on desktop
 * Linux for a compile check.
 *
 * Usage: anland-gpu-copy SOCKET_FD
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "gpu-copy-protocol.h"

#define MAX_TARGETS 8
#define MAX_SOURCES 32
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffull

#ifndef EGL_SYNC_NATIVE_FENCE_ANDROID
#define EGL_SYNC_NATIVE_FENCE_ANDROID 0x3144
#define EGL_NO_NATIVE_FENCE_FD_ANDROID -1
#endif

static void logmsg(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "anland-gpu-copy", "%s", line);
#endif
    fprintf(stderr, "anland-gpu-copy: %s\n", line);
}

/* ------------------------------------------------------------------------- */
/* EGL / GLES state                                                          */
/* ------------------------------------------------------------------------- */

static EGLDisplay g_dpy = EGL_NO_DISPLAY;
static EGLContext g_ctx = EGL_NO_CONTEXT;
static EGLSurface g_surf = EGL_NO_SURFACE;
static bool g_has_modifiers, g_has_native_fence;
static GLuint g_prog, g_vbo;
static GLint g_attr_pos, g_uni_tex;

static PFNEGLCREATEIMAGEKHRPROC p_eglCreateImageKHR;
static PFNEGLDESTROYIMAGEKHRPROC p_eglDestroyImageKHR;
static PFNEGLCREATESYNCKHRPROC p_eglCreateSyncKHR;
static PFNEGLDESTROYSYNCKHRPROC p_eglDestroySyncKHR;
static PFNEGLDUPNATIVEFENCEFDANDROIDPROC p_eglDupNativeFenceFDANDROID;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_glEGLImageTargetTexture2DOES;
static PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC p_glEGLImageTargetRenderbufferStorageOES;

struct target {
    bool used;
    EGLImageKHR image;
    GLuint rb, fbo;
    uint32_t width, height;
};

struct source {
    bool used;
    uint32_t id;
    EGLImageKHR image;
    GLuint tex;
    uint32_t width, height;
    GLint filter;
};

static struct target g_targets[MAX_TARGETS];
static struct source g_sources[MAX_SOURCES];

static bool has_ext(const char *list, const char *name)
{
    size_t len = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += len)
        if ((p == list || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0))
            return true;
    return false;
}

static const char *vertex_src =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() {\n"
    /* Row 0 of the frame lands in row 0 of the screen buffer: GL keeps the
     * memory order of both images, so no flip is needed. */
    "    uv = pos * 0.5 + 0.5;\n"
    "    gl_Position = vec4(pos, 0.0, 1.0);\n"
    "}\n";

static const char *fragment_src =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D tex;\n"
    "void main() {\n"
    "    gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0);\n" /* a screen is opaque */
    "}\n";

static GLuint compile(GLenum type, const char *src, char *err, size_t err_len)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glGetShaderInfoLog(sh, (GLsizei)err_len, NULL, err);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int gl_setup(char *msg, size_t msg_len)
{
    g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_dpy == EGL_NO_DISPLAY || !eglInitialize(g_dpy, NULL, NULL)) {
        snprintf(msg, msg_len, "eglInitialize failed (0x%x)", eglGetError());
        return -ENODEV;
    }
    const char *exts = eglQueryString(g_dpy, EGL_EXTENSIONS);
    if (!has_ext(exts, "EGL_EXT_image_dma_buf_import")) {
        snprintf(msg, msg_len, "the EGL driver (%s) cannot import dma-bufs",
                 eglQueryString(g_dpy, EGL_VENDOR));
        return -ENOTSUP;
    }
    g_has_modifiers = has_ext(exts, "EGL_EXT_image_dma_buf_import_modifiers");
    g_has_native_fence = has_ext(exts, "EGL_ANDROID_native_fence_sync");

    p_eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    p_eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    p_eglCreateSyncKHR = (PFNEGLCREATESYNCKHRPROC)eglGetProcAddress("eglCreateSyncKHR");
    p_eglDestroySyncKHR = (PFNEGLDESTROYSYNCKHRPROC)eglGetProcAddress("eglDestroySyncKHR");
    p_eglDupNativeFenceFDANDROID =
        (PFNEGLDUPNATIVEFENCEFDANDROIDPROC)eglGetProcAddress("eglDupNativeFenceFDANDROID");
    p_glEGLImageTargetTexture2DOES =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    p_glEGLImageTargetRenderbufferStorageOES = (PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC)
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES");
    if (!p_eglCreateImageKHR || !p_eglDestroyImageKHR || !p_glEGLImageTargetTexture2DOES ||
        !p_glEGLImageTargetRenderbufferStorageOES) {
        snprintf(msg, msg_len, "the EGL driver lacks EGLImage functions");
        return -ENOTSUP;
    }
    if (!p_eglCreateSyncKHR || !p_eglDestroySyncKHR || !p_eglDupNativeFenceFDANDROID)
        g_has_native_fence = false;

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(g_dpy, cfg_attribs, &cfg, 1, &n) || n < 1) {
        snprintf(msg, msg_len, "no EGL config for OpenGL ES 2 (0x%x)", eglGetError());
        return -ENOTSUP;
    }
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    const EGLint pb_attribs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    g_surf = eglCreatePbufferSurface(g_dpy, cfg, pb_attribs);
    if (g_ctx == EGL_NO_CONTEXT || g_surf == EGL_NO_SURFACE ||
        !eglMakeCurrent(g_dpy, g_surf, g_surf, g_ctx)) {
        snprintf(msg, msg_len, "cannot create an OpenGL ES context (0x%x)", eglGetError());
        return -ENOTSUP;
    }

    char err[180] = "";
    GLuint vs = compile(GL_VERTEX_SHADER, vertex_src, err, sizeof(err));
    GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, fragment_src, err, sizeof(err)) : 0;
    if (!vs || !fs) {
        snprintf(msg, msg_len, "shader: %s", err);
        return -EINVAL;
    }
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    GLint linked = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glGetProgramInfoLog(g_prog, sizeof(err), NULL, err);
        snprintf(msg, msg_len, "program: %s", err);
        return -EINVAL;
    }
    g_attr_pos = glGetAttribLocation(g_prog, "pos");
    g_uni_tex = glGetUniformLocation(g_prog, "tex");

    static const GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    glUseProgram(g_prog);
    glUniform1i(g_uni_tex, 0);
    glEnableVertexAttribArray((GLuint)g_attr_pos);
    glVertexAttribPointer((GLuint)g_attr_pos, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);

    snprintf(msg, msg_len, "%s (%s)%s", (const char *)glGetString(GL_RENDERER),
             (const char *)glGetString(GL_VERSION),
             g_has_native_fence ? ", render fences" : ", no render fences");
    return 0;
}

static EGLImageKHR import_dmabuf(int fd, const struct gc_request *rq, char *msg, size_t msg_len)
{
    EGLint attribs[32];
    int i = 0;
    attribs[i++] = EGL_WIDTH; attribs[i++] = (EGLint)rq->width;
    attribs[i++] = EGL_HEIGHT; attribs[i++] = (EGLint)rq->height;
    attribs[i++] = EGL_LINUX_DRM_FOURCC_EXT; attribs[i++] = (EGLint)rq->fourcc;
    attribs[i++] = EGL_DMA_BUF_PLANE0_FD_EXT; attribs[i++] = fd;
    attribs[i++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attribs[i++] = (EGLint)rq->offset;
    attribs[i++] = EGL_DMA_BUF_PLANE0_PITCH_EXT; attribs[i++] = (EGLint)rq->stride;
    if (g_has_modifiers && rq->modifier != DRM_FORMAT_MOD_INVALID) {
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
        attribs[i++] = (EGLint)(rq->modifier & 0xffffffffu);
        attribs[i++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
        attribs[i++] = (EGLint)(rq->modifier >> 32);
    }
    attribs[i++] = EGL_NONE;
    EGLImageKHR img = p_eglCreateImageKHR(g_dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attribs);
    if (img == EGL_NO_IMAGE_KHR)
        snprintf(msg, msg_len, "cannot import %ux%u fourcc 0x%08x stride %u modifier 0x%llx (0x%x)",
                 rq->width, rq->height, rq->fourcc, rq->stride,
                 (unsigned long long)rq->modifier, eglGetError());
    return img;
}

static void drop_target(struct target *t)
{
    if (!t->used)
        return;
    glDeleteFramebuffers(1, &t->fbo);
    glDeleteRenderbuffers(1, &t->rb);
    p_eglDestroyImageKHR(g_dpy, t->image);
    memset(t, 0, sizeof(*t));
}

static void drop_source(struct source *s)
{
    if (!s->used)
        return;
    glDeleteTextures(1, &s->tex);
    p_eglDestroyImageKHR(g_dpy, s->image);
    memset(s, 0, sizeof(*s));
}

static struct source *find_source(uint32_t id)
{
    for (int i = 0; i < MAX_SOURCES; i++)
        if (g_sources[i].used && g_sources[i].id == id)
            return &g_sources[i];
    return NULL;
}

static int add_target(int fd, const struct gc_request *rq, char *msg, size_t msg_len)
{
    if (rq->id >= MAX_TARGETS) {
        snprintf(msg, msg_len, "Anland buffer index %u out of range", rq->id);
        return -EINVAL;
    }
    struct target *t = &g_targets[rq->id];
    drop_target(t);
    EGLImageKHR img = import_dmabuf(fd, rq, msg, msg_len);
    if (img == EGL_NO_IMAGE_KHR)
        return -EINVAL;
    t->image = img;
    glGenRenderbuffers(1, &t->rb);
    glBindRenderbuffer(GL_RENDERBUFFER, t->rb);
    p_glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)img);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->rb);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    t->used = true;
    t->width = rq->width;
    t->height = rq->height;
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        snprintf(msg, msg_len, "the GPU cannot draw into Anland buffer %u (framebuffer 0x%x)",
                 rq->id, status);
        drop_target(t);
        return -ENOTSUP;
    }
    return 0;
}

static int add_source(int fd, const struct gc_request *rq, char *msg, size_t msg_len)
{
    struct source *s = find_source(rq->id);
    if (s)
        drop_source(s);
    for (int i = 0; i < MAX_SOURCES && !s; i++)
        if (!g_sources[i].used)
            s = &g_sources[i];
    if (!s) {
        /* The bridge forgets buffers it no longer has; this only happens if
         * Android keeps more buffers alive than expected. Reuse a slot. */
        s = &g_sources[rq->id % MAX_SOURCES];
        drop_source(s);
    }
    EGLImageKHR img = import_dmabuf(fd, rq, msg, msg_len);
    if (img == EGL_NO_IMAGE_KHR)
        return -EINVAL;
    s->image = img;
    glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    p_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        snprintf(msg, msg_len, "cannot use Android's frame as a texture (GL 0x%x)", err);
        p_eglDestroyImageKHR(g_dpy, img);
        glDeleteTextures(1, &s->tex);
        memset(s, 0, sizeof(*s));
        return -EINVAL;
    }
    s->used = true;
    s->id = rq->id;
    s->width = rq->width;
    s->height = rq->height;
    s->filter = GL_NEAREST;
    return 0;
}

static int copy(const struct gc_request *rq, int *fence_out, char *msg, size_t msg_len)
{
    *fence_out = -1;
    struct source *s = find_source(rq->id);
    if (!s || rq->aux >= MAX_TARGETS || !g_targets[rq->aux].used) {
        snprintf(msg, msg_len, "copy of unknown buffers (source %u, target %u)", rq->id, rq->aux);
        return -ENOENT;
    }
    struct target *t = &g_targets[rq->aux];

    /* Smooth enlargement when Android renders below the screen resolution. */
    GLint filter = (s->width == t->width && s->height == t->height) ? GL_NEAREST : GL_LINEAR;
    glBindTexture(GL_TEXTURE_2D, s->tex);
    if (filter != s->filter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        s->filter = filter;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glViewport(0, 0, (GLsizei)t->width, (GLsizei)t->height);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (g_has_native_fence) {
        EGLSyncKHR sync = p_eglCreateSyncKHR(g_dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush(); /* the fence only gets its fd once the work is submitted */
        if (sync != EGL_NO_SYNC_KHR) {
            int fd = p_eglDupNativeFenceFDANDROID(g_dpy, sync);
            p_eglDestroySyncKHR(g_dpy, sync);
            if (fd >= 0) {
                *fence_out = fd;
                goto done;
            }
        }
        g_has_native_fence = false;
        logmsg("render fences do not work with this driver; waiting for the GPU instead");
    }
    glFinish();
done:;
    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        snprintf(msg, msg_len, "drawing failed (GL 0x%x)", err);
        if (*fence_out >= 0)
            close(*fence_out);
        *fence_out = -1;
        return -EIO;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Socket                                                                    */
/* ------------------------------------------------------------------------- */

static ssize_t recv_request(int sock, struct gc_request *rq, int *fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { rq, sizeof(*rq) };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);
    *fd = -1;
    ssize_t n;
    do {
        n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); n > 0 && c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(fd, CMSG_DATA(c), sizeof(int));
    return n;
}

static void send_reply(int sock, const struct gc_reply *rp, int fd)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { (void *)rp, sizeof(*rp) };
    struct msghdr mh = { 0 };
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (fd >= 0) {
        memset(cbuf, 0, sizeof(cbuf));
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    while (sendmsg(sock, &mh, MSG_NOSIGNAL) < 0 && errno == EINTR)
        ;
}

int main(int argc, char **argv)
{
    int sock = argc > 1 ? atoi(argv[1]) : 3;
    signal(SIGPIPE, SIG_IGN);
    fcntl(sock, F_SETFD, FD_CLOEXEC);

    bool ready = false;
    for (;;) {
        struct gc_request rq;
        int fd = -1;
        ssize_t n = recv_request(sock, &rq, &fd);
        if (n <= 0)
            break; /* the bridge went away */
        struct gc_reply rp = { .type = rq.type, .status = 0, .text = "" };
        int out_fd = -1;
        if (n != (ssize_t)sizeof(rq)) {
            rp.status = -EPROTO;
            snprintf(rp.text, sizeof(rp.text), "malformed request");
        } else if (rq.type == GC_HELLO) {
            if (rq.aux != GC_VERSION) {
                rp.status = -EPROTO;
                snprintf(rp.text, sizeof(rp.text), "protocol version %u, expected %u", rq.aux, GC_VERSION);
            } else {
                rp.status = gl_setup(rp.text, sizeof(rp.text));
                ready = rp.status == 0;
            }
        } else if (!ready) {
            rp.status = -ENODEV;
            snprintf(rp.text, sizeof(rp.text), "not initialised");
        } else {
            switch (rq.type) {
            case GC_TARGET:
                rp.status = fd < 0 ? -EBADF : add_target(fd, &rq, rp.text, sizeof(rp.text));
                break;
            case GC_SOURCE:
                rp.status = fd < 0 ? -EBADF : add_source(fd, &rq, rp.text, sizeof(rp.text));
                break;
            case GC_FORGET_SOURCE: {
                struct source *s = find_source(rq.id);
                if (s)
                    drop_source(s);
                break;
            }
            case GC_RESET_TARGETS:
                for (int i = 0; i < MAX_TARGETS; i++)
                    drop_target(&g_targets[i]);
                break;
            case GC_COPY:
                rp.status = copy(&rq, &out_fd, rp.text, sizeof(rp.text));
                break;
            default:
                rp.status = -EPROTO;
                snprintf(rp.text, sizeof(rp.text), "unknown request %u", rq.type);
            }
        }
        if (fd >= 0)
            close(fd); /* EGL keeps its own reference to an imported buffer */
        send_reply(sock, &rp, out_fd);
        if (out_fd >= 0)
            close(out_fd);
        if (rp.status && rq.type == GC_HELLO)
            logmsg("%s", rp.text);
    }
    return 0;
}
