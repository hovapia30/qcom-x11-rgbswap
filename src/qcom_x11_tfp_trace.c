/*
 * qcom_x11_tfp_trace.c - diagnostic/trace task (and interposition entry point)
 *
 * Logs GLX texture-from-pixmap related calls made by the compositor
 * (glXCreatePixmap, glXDestroyPixmap, glXBindTexImageEXT, glXReleaseTexImageEXT,
 *  glXChooseFBConfig, glXChooseVisual) to help diagnose why redirected window
 * contents may not appear when compositing.
 *
 * This file provides the unified interposed GLX/dlsym/glXGetProcAddress entry
 * points for the combined libqcom_x11_tfp.so and is linked together with:
 *   - qcom_x11_tfp_fix.c     (TFP attribute override/fix task)
 *   - qcom_x11_rgbswap.c     (Adreno EGL/X11 R/B channel swap task)
 * TFP attributes are passed through the fix module before being forwarded to the
 * real driver. dlsym results are passed through the R/B-swap module so that
 * CreateSubDriver interception remains active.
 *
 * Output: stderr by default, or the file named by $QCOM_TFP_TRACE_LOG.
 * Enable/disable: set QCOM_TFP_TRACE=0 to disable (default: enabled when loaded).
 *
 * Build (as part of combined .so):
 *   cc -shared -fPIC -O2 -Wall -Wextra -o libqcom_x11_tfp.so \
 *      src/qcom_x11_tfp_trace.c src/qcom_x11_tfp_fix.c src/qcom_x11_rgbswap.c -ldl -lX11 -lGL
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

/* Implemented in qcom_x11_tfp_fix.c (fix task). */
extern const int *tfp_fix_rewrite_attribs(const int *in, int *out, int outcap, const char *where);

/* Implemented in qcom_x11_rgbswap.c (R/B swap task). */
extern void *rgbswap_wrap_dlsym_result(void *handle, const char *symbol, void *result);

/* Real function pointers */
static PFNGLXCREATEPIXMAPPROC       real_glXCreatePixmap;
static PFNGLXDESTROYPIXMAPPROC      real_glXDestroyPixmap;
static PFNGLXBINDTEXIMAGEEXTPROC    real_glXBindTexImageEXT;
static PFNGLXRELEASETEXIMAGEEXTPROC real_glXReleaseTexImageEXT;
static PFNGLXCHOOSEFBCONFIGPROC     real_glXChooseFBConfig;
static PFNGLXGETFBCONFIGATTRIBPROC  real_glXGetFBConfigAttrib;
static XVisualInfo *(*real_glXChooseVisual)(Display *, int, int *);
static PFNGLXGETVISUALFROMFBCONFIGPROC real_glXGetVisualFromFBConfig;
static GLenum (*real_glGetError)(void);
static void (*real_glFinish)(void);
static void (*real_glFlush)(void);
static void (*real_glGetIntegerv)(GLenum, GLint *);
static void (*real_glGetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
static void (*real_glGenFramebuffers)(GLsizei, GLuint *);
static void (*real_glDeleteFramebuffers)(GLsizei, const GLuint *);
static void (*real_glBindFramebuffer)(GLenum, GLuint);
static void (*real_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
static GLenum (*real_glCheckFramebufferStatus)(GLenum);
static void (*real_glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static void *(*real_dlsym)(void *, const char *);
static __GLXextFuncPtr (*real_glXGetProcAddress)(const GLubyte *);
static __GLXextFuncPtr (*real_glXGetProcAddressARB)(const GLubyte *);
static void (*(*real_eglGetProcAddress)(const char *))(void);
static const char *(*real_eglQueryString)(EGLDisplay, EGLint);
static EGLSync (*real_eglCreateSync)(EGLDisplay, EGLenum, const EGLAttrib *);
static EGLBoolean (*real_eglDestroySync)(EGLDisplay, EGLSync);
static EGLint (*real_eglClientWaitSync)(EGLDisplay, EGLSync, EGLint, EGLTime);
static EGLBoolean (*real_eglGetSyncAttrib)(EGLDisplay, EGLSync, EGLint, EGLAttrib *);
static EGLBoolean (*real_eglWaitSync)(EGLDisplay, EGLSync, EGLint);


static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf;
static int enabled = 1;
static int readback = 0;
static int readback_count = 0;
static int sync_before_readback = 0;

static void trace(const char *fmt, ...)
{
    if (!enabled) return;
    va_list ap;
    pthread_mutex_lock(&log_mu);
    FILE *out = logf ? logf : stderr;
    fprintf(out, "[tfp-trace pid=%d] ", (int)getpid());
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "\n");
    fflush(out);
    pthread_mutex_unlock(&log_mu);
}

static void init_log(void)
{
    const char *dis = getenv("QCOM_TFP_TRACE");
    if (dis && strcmp(dis, "0") == 0) { enabled = 0; }
    const char *path = getenv("QCOM_TFP_TRACE_LOG");
    if (path && *path) {
        logf = fopen(path, "a");
    }
    const char *rb = getenv("QCOM_TFP_TRACE_READBACK");
    if (rb && strcmp(rb, "0") != 0) readback = 1;
    const char *sync = getenv("QCOM_TFP_TRACE_SYNC");
    if (sync && strcmp(sync, "0") != 0) sync_before_readback = 1;
    if (enabled)
        trace("init (log=%s readback=%d sync=%d)", (path && *path) ? path : "stderr", readback, sync_before_readback);
}

static void ensure_init(void)
{
    pthread_once(&init_once, init_log);
}

static const char *attr_name(int a)
{
    switch (a) {
    case GLX_TEXTURE_FORMAT_EXT: return "GLX_TEXTURE_FORMAT_EXT";
    case GLX_TEXTURE_TARGET_EXT: return "GLX_TEXTURE_TARGET_EXT";
#ifdef GLX_MIPMAP_TEXTURE_EXT
    case GLX_MIPMAP_TEXTURE_EXT: return "GLX_MIPMAP_TEXTURE_EXT";
#endif
    case GLX_FRONT_EXT: return "GLX_FRONT_EXT/FRONT_LEFT";
    case 0x20D8: return "GLX_TEXTURE_FORMAT_NONE_EXT";
    case 0x20D9: return "GLX_TEXTURE_FORMAT_RGB_EXT";
    case 0x20DA: return "GLX_TEXTURE_FORMAT_RGBA_EXT";
    case 0x20DB: return "GLX_TEXTURE_1D_EXT";
    case 0x20DC: return "GLX_TEXTURE_2D_EXT";
    case 0x20DD: return "GLX_TEXTURE_RECTANGLE_EXT";
    default: return NULL;
    }
}

static void log_attrib_list(const char *label, const int *attribs)
{
    if (!attribs) { trace("  %s: (null)", label); return; }
    char buf[512];
    size_t off = 0;
    buf[0] = 0;
    for (int i = 0; attribs[i] != None && i < 32; i += 2) {
        const char *n = attr_name(attribs[i]);
        int v = attribs[i + 1];
        char item[96];
        if (n) snprintf(item, sizeof(item), "%s(0x%x)=0x%x ", n, attribs[i], v);
        else   snprintf(item, sizeof(item), "0x%x=0x%x ", attribs[i], v);
        size_t len = strlen(item);
        if (off + len + 1 < sizeof(buf)) { memcpy(buf + off, item, len + 1); off += len; }
    }
    if (off == 0) snprintf(buf, sizeof(buf), "(empty/terminated)");
    trace("  %s: %s", label, buf);
}

static void log_fbconfig(Display *dpy, GLXFBConfig cfg, const char *label)
{
    if (!dpy || !cfg) { trace("  %s: (null)", label); return; }
    if (!real_glXGetFBConfigAttrib) {
        real_glXGetFBConfigAttrib = (PFNGLXGETFBCONFIGATTRIBPROC)dlsym(RTLD_NEXT, "glXGetFBConfigAttrib");
    }
    if (!real_glXGetFBConfigAttrib) { trace("  %s: cfg=%p (no getattrib)", label, (void *)cfg); return; }
    int v;
    int id = -1, r = -1, g = -1, b = -1, a = -1, depth = -1, stencil = -1, db = -1, drawable = -1, xrender = -1, xvisual = -1, visualid = -1, bind_rgb = -1, bind_rgba = -1, yinv = -1;
#define GA(attr, var) do { v = -999; real_glXGetFBConfigAttrib(dpy, cfg, attr, &v); var = v; } while (0)
    GA(GLX_FBCONFIG_ID, id);
    GA(GLX_RED_SIZE, r); GA(GLX_GREEN_SIZE, g); GA(GLX_BLUE_SIZE, b); GA(GLX_ALPHA_SIZE, a);
    GA(GLX_DEPTH_SIZE, depth); GA(GLX_STENCIL_SIZE, stencil);
    GA(GLX_DOUBLEBUFFER, db); GA(GLX_DRAWABLE_TYPE, drawable);
    GA(0x8012, xrender); /* GLX_X_RENDERABLE */
    GA(GLX_X_VISUAL_TYPE, xvisual);
    GA(GLX_VISUAL_ID, visualid);
    GA(0x20A0, bind_rgb);  /* GLX_BIND_TO_TEXTURE_RGB_EXT */
    GA(0x20A1, bind_rgba); /* GLX_BIND_TO_TEXTURE_RGBA_EXT */
    GA(0x20A6, yinv);      /* GLX_Y_INVERTED_EXT */
#undef GA
    trace("  %s: id=0x%x visual=0x%x rgba=%d/%d/%d/%d depth=%d stencil=%d db=%d drawable=0x%x xrender=%d xvisual=%d bind_rgb=%d bind_rgba=%d y_inv=%d",
          label, id, visualid, r, g, b, a, depth, stencil, db, drawable, xrender, xvisual, bind_rgb, bind_rgba, yinv);
}

static void resolve_real_dlsym(void)
{
    if (real_dlsym) return;
    real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
    if (!real_dlsym) real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
    if (!real_dlsym) real_dlsym = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
}

static void resolve_all(void)
{
    resolve_real_dlsym();
    if (!real_dlsym) return;
#define R(name) do { if (!real_##name) real_##name = (void *)real_dlsym(RTLD_NEXT, #name); } while (0)
    R(glXCreatePixmap);
    R(glXDestroyPixmap);
    R(glXBindTexImageEXT);
    R(glXReleaseTexImageEXT);
    R(glXChooseFBConfig);
    R(glXGetFBConfigAttrib);
    R(glXChooseVisual);
    R(glXGetVisualFromFBConfig);
    R(glXGetProcAddress);
    R(glXGetProcAddressARB);
    R(eglGetProcAddress);
    R(eglQueryString);
    R(eglCreateSync);
    R(eglDestroySync);
    R(eglClientWaitSync);
    R(eglGetSyncAttrib);
    R(eglWaitSync);
    R(glGetError);
    R(glFinish);
    R(glFlush);
    R(glGetIntegerv);
    R(glGetTexLevelParameteriv);
    R(glGenFramebuffers);
    R(glDeleteFramebuffers);
    R(glBindFramebuffer);
    R(glFramebufferTexture2D);
    R(glCheckFramebufferStatus);
    R(glReadPixels);
#undef R
}

static void log_gl_errors(const char *where)
{
    if (!real_glGetError) return;
    GLenum e;
    int n = 0;
    while ((e = real_glGetError()) != GL_NO_ERROR && n < 8) {
        trace("  %s: glGetError=0x%x", where, (unsigned)e);
        n++;
    }
    if (n == 0) trace("  %s: glGetError=GL_NO_ERROR", where);
}

static void try_readback(void)
{
    if (!readback) return;
    int c = __sync_fetch_and_add(&readback_count, 1);
    if (c > 20 && (c % 60) != 0) return;
    if (!real_glGetIntegerv || !real_glGetTexLevelParameteriv || !real_glGenFramebuffers
        || !real_glDeleteFramebuffers || !real_glBindFramebuffer || !real_glFramebufferTexture2D
        || !real_glCheckFramebufferStatus || !real_glReadPixels) {
        trace("  readback: missing GL functions");
        readback = 0;
        return;
    }
    GLint prev_fbo = 0, tex = 0;
    real_glGetIntegerv(0x8CA6 /* GL_READ_FRAMEBUFFER_BINDING */, &prev_fbo);
    real_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &tex);
    GLenum target = GL_TEXTURE_2D;
    if (tex == 0) {
        real_glGetIntegerv(0x84F6 /* GL_TEXTURE_BINDING_RECTANGLE */, &tex);
        target = 0x84F5; /* GL_TEXTURE_RECTANGLE */
    }
    if (tex == 0) {
        trace("  readback: no bound texture (2D/RECT)");
        return;
    }
    GLint w = 0, h = 0;
    real_glGetTexLevelParameteriv(target, 0, 0x1000 /* GL_TEXTURE_WIDTH */, &w);
    real_glGetTexLevelParameteriv(target, 0, 0x1001 /* GL_TEXTURE_HEIGHT */, &h);
    GLuint fbo = 0;
    real_glGenFramebuffers(1, &fbo);
    real_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, fbo);
    real_glFramebufferTexture2D(0x8CA8, 0x8CE0 /* GL_COLOR_ATTACHMENT0 */, target, (GLuint)tex, 0);
    GLenum status = real_glCheckFramebufferStatus(0x8CA8);
    if (status == 0x8CD5 /* GL_FRAMEBUFFER_COMPLETE */ && w > 0 && h > 0) {
        unsigned char px[4] = {0, 0, 0, 0};
        GLint x = w / 2, y = h / 2;
        real_glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        trace("  readback: tex=%d target=0x%x %dx%d center=(%d,%d) rgba=(%u,%u,%u,%u)",
              tex, (unsigned)target, w, h, x, y, px[0], px[1], px[2], px[3]);
    } else {
        trace("  readback: tex=%d target=0x%x %dx%d fbo_status=0x%x", tex, (unsigned)target, w, h, (unsigned)status);
    }
    real_glBindFramebuffer(0x8CA8, (GLuint)prev_fbo);
    real_glDeleteFramebuffers(1, &fbo);
}

/* --- interposed GLX functions --- */

GLXPixmap glXCreatePixmap(Display *dpy, GLXFBConfig config, Pixmap pixmap, const int *attrib_list)
{
    ensure_init();
    resolve_all();
    trace("glXCreatePixmap(dpy=%p, config=%p, pixmap=0x%lx)", dpy, (void *)config, (unsigned long)pixmap);
    log_fbconfig(dpy, config, "config");
    log_attrib_list("attribs", attrib_list);
    int fixbuf[64];
    const int *attribs = tfp_fix_rewrite_attribs(attrib_list, fixbuf, 64, "glXCreatePixmap");
    if (attribs != attrib_list)
        log_attrib_list("attribs(effective)", attribs);
    GLXPixmap r = real_glXCreatePixmap ? real_glXCreatePixmap(dpy, config, pixmap, attribs) : 0;
    trace("glXCreatePixmap -> 0x%lx", (unsigned long)r);
    return r;
}

void glXDestroyPixmap(Display *dpy, GLXPixmap pixmap)
{
    ensure_init();
    resolve_all();
    trace("glXDestroyPixmap(dpy=%p, pixmap=0x%lx)", dpy, (unsigned long)pixmap);
    if (real_glXDestroyPixmap) real_glXDestroyPixmap(dpy, pixmap);
}

void glXBindTexImageEXT(Display *dpy, GLXDrawable drawable, int buffer, const int *attrib_list)
{
    ensure_init();
    resolve_all();
    trace("glXBindTexImageEXT(dpy=%p, drawable=0x%lx, buffer=0x%x)", dpy, (unsigned long)drawable, buffer);
    log_attrib_list("attribs", attrib_list);
    int fixbuf[64];
    const int *attribs = tfp_fix_rewrite_attribs(attrib_list, fixbuf, 64, "glXBindTexImageEXT");
    if (attribs != attrib_list)
        log_attrib_list("attribs(effective)", attribs);
    if (real_glXBindTexImageEXT) real_glXBindTexImageEXT(dpy, drawable, buffer, attribs);
    log_gl_errors("after glXBindTexImageEXT");
    if (sync_before_readback && real_glFinish) { real_glFinish(); usleep(100000); }
    try_readback();
    trace("glXBindTexImageEXT done");
}

void glXReleaseTexImageEXT(Display *dpy, GLXDrawable drawable, int buffer)
{
    ensure_init();
    resolve_all();
    trace("glXReleaseTexImageEXT(dpy=%p, drawable=0x%lx, buffer=0x%x)", dpy, (unsigned long)drawable, buffer);
    if (real_glXReleaseTexImageEXT) real_glXReleaseTexImageEXT(dpy, drawable, buffer);
    log_gl_errors("after glXReleaseTexImageEXT");
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen, const int *attrib_list, int *nelements)
{
    ensure_init();
    resolve_all();
    trace("glXChooseFBConfig(dpy=%p, screen=%d)", dpy, screen);
    if (attrib_list) {
        char buf[512]; size_t off = 0; buf[0] = 0;
        for (int i = 0; attrib_list[i] != None && i < 40; i += 2) {
            char item[64]; snprintf(item, sizeof(item), "0x%x=0x%x ", attrib_list[i], attrib_list[i + 1]);
            size_t len = strlen(item);
            if (off + len + 1 < sizeof(buf)) { memcpy(buf + off, item, len + 1); off += len; }
        }
        trace("  attribs: %s", buf[0] ? buf : "(empty)");
    } else {
        trace("  attribs: (null)");
    }
    GLXFBConfig *r = real_glXChooseFBConfig ? real_glXChooseFBConfig(dpy, screen, attrib_list, nelements) : NULL;
    trace("glXChooseFBConfig -> %p (nelements=%d)", (void *)r, nelements ? *nelements : -1);
    return r;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList)
{
    ensure_init();
    resolve_all();
    trace("glXChooseVisual(dpy=%p, screen=%d)", dpy, screen);
    XVisualInfo *r = real_glXChooseVisual ? real_glXChooseVisual(dpy, screen, attribList) : NULL;
    if (r) trace("glXChooseVisual -> visualid=0x%lx depth=%d", r->visualid, r->depth);
    else trace("glXChooseVisual -> NULL");
    return r;
}




/* --- EGL_KHR_fence_sync mapping (EGL 1.5 native sync if available, fallback) --- */

#ifndef EGL_SYNC_FENCE_KHR
#define EGL_SYNC_FENCE_KHR 0x30F9
#endif
#ifndef EGL_SYNC_TYPE_KHR
#define EGL_SYNC_TYPE_KHR 0x30F7
#endif
#ifndef EGL_SYNC_STATUS_KHR
#define EGL_SYNC_STATUS_KHR 0x30F1
#endif
#ifndef EGL_SYNC_CONDITION_KHR
#define EGL_SYNC_CONDITION_KHR 0x30F8
#endif
#ifndef EGL_SIGNALED_KHR
#define EGL_SIGNALED_KHR 0x30F2
#endif
#ifndef EGL_UNSIGNALED_KHR
#define EGL_UNSIGNALED_KHR 0x30F3
#endif
#ifndef EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR
#define EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR 0x30F0
#endif
#ifndef EGL_SYNC_FLUSH_COMMANDS_BIT_KHR
#define EGL_SYNC_FLUSH_COMMANDS_BIT_KHR 0x0001
#endif
#ifndef EGL_CONDITION_SATISFIED_KHR
#define EGL_CONDITION_SATISFIED_KHR 0x30F6
#endif
#ifndef EGL_TIMEOUT_EXPIRED_KHR
#define EGL_TIMEOUT_EXPIRED_KHR 0x30F5
#endif
#ifndef EGL_FOREVER_KHR
#define EGL_FOREVER_KHR 0xFFFFFFFFFFFFFFFFull
#endif

static pthread_mutex_t ext_mu = PTHREAD_MUTEX_INITIALIZER;
static char extbuf[8192];
static int extbuf_real; /* 1 if extbuf was built from a real driver query */
static __thread int in_egl_query_string;

static EGLSyncKHR fake_fence_sync(void)
{
    return (EGLSyncKHR)(uintptr_t)0x1;
}

static int is_fake_fence_sync(EGLSyncKHR sync)
{
    return sync == fake_fence_sync();
}

static int is_fake_fence_sync_core(EGLSync sync)
{
    return (uintptr_t)sync == (uintptr_t)0x1;
}

/* Not exported as a symbol override: libEGL may call eglQueryString internally
 * during its own initialization, and overriding the exported symbol can deadlock
 * on libEGL's pthread_once. External callers resolving via eglGetProcAddress/dlsym
 * still receive this wrapper so EGL_KHR_fence_sync can be advertised. */
static const char *shim_eglQueryString(EGLDisplay dpy, EGLint name)
{
    (void)dpy;
    ensure_init();
    resolve_all();

    /* Never call real eglQueryString: libEGL may invoke this hook (via
     * eglGetProcAddress/dlsym resolution during its own pthread_once init) and
     * libEGL's eglQueryString can self-deadlock on the same once control.
     * Return safe defaults for common queries. */
    switch (name) {
    case EGL_EXTENSIONS:
        pthread_mutex_lock(&ext_mu);
        if (extbuf[0] == '\0') {
            snprintf(extbuf, sizeof(extbuf),
                     "EGL_KHR_fence_sync EGL_KHR_image EGL_KHR_image_base "
                     "EGL_KHR_image_pixmap EGL_KHR_gl_texture_2D_image "
                     "EGL_KHR_gl_texture_cubemap_image EGL_KHR_gl_renderbuffer_image "
                     "EGL_KHR_surfaceless_context EGL_KHR_create_context "
                     "EGL_EXT_platform_base EGL_EXT_platform_x11 EGL_KHR_platform_x11 "
                     "EGL_EXT_image_dma_buf_import EGL_EXT_image_dma_buf_import_modifiers");
        }
        {
            const char *r = extbuf;
            pthread_mutex_unlock(&ext_mu);
            return r;
        }
    case EGL_VERSION:
        return "1.5 Adreno (shim)";
    case EGL_VENDOR:
        return "Qualcomm";
    case EGL_CLIENT_APIS:
        return "OpenGL_ES OpenGL";
    default:
        return "";
    }
}

EGLSyncKHR eglCreateSyncKHR(EGLDisplay dpy, EGLenum type, const EGLint *attrib_list)
{
    ensure_init();
    resolve_all();

    if (real_eglCreateSync) {
        /* EGL 1.5 takes EGLAttrib attributes; convert from EGLint list. */
        EGLAttrib attrs[16];
        const EGLAttrib *ap = NULL;
        if (attrib_list) {
            int i = 0, j = 0;
            while (attrib_list[i] != EGL_NONE && j < 14) {
                attrs[j++] = (EGLAttrib)attrib_list[i++];
                attrs[j++] = (EGLAttrib)attrib_list[i++];
            }
            attrs[j] = (EGLAttrib)EGL_NONE;
            ap = attrs;
        }
        EGLSync sync = real_eglCreateSync(dpy, type, ap);
        if (sync != EGL_NO_SYNC) {
            return (EGLSyncKHR)sync;
        }
    }

    /* Fallback: fake fence sync with GL flush/finish semantics. */
    if (real_glFlush) real_glFlush();
    return fake_fence_sync();
}

EGLBoolean eglDestroySyncKHR(EGLDisplay dpy, EGLSyncKHR sync)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync(sync)) {
        return EGL_TRUE;
    }
    if (real_eglDestroySync) {
        return real_eglDestroySync(dpy, (EGLSync)sync);
    }
    return EGL_FALSE;
}

EGLint eglClientWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags, EGLTimeKHR timeout)
{
    ensure_init();
    resolve_all();
    (void)flags;
    if (is_fake_fence_sync(sync)) {
        if (timeout > 0 && real_glFinish) {
            real_glFinish();
        } else if (real_glFlush) {
            real_glFlush();
        }
        return EGL_CONDITION_SATISFIED_KHR;
    }
    if (real_eglClientWaitSync) {
        return real_eglClientWaitSync(dpy, (EGLSync)sync, flags, (EGLTime)timeout);
    }
    if (timeout > 0 && real_glFinish) real_glFinish();
    return EGL_CONDITION_SATISFIED_KHR;
}

EGLBoolean eglGetSyncAttribKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint attribute, EGLint *value)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync(sync)) {
        if (!value) return EGL_FALSE;
        switch (attribute) {
        case EGL_SYNC_TYPE_KHR:
            *value = EGL_SYNC_FENCE_KHR;
            return EGL_TRUE;
        case EGL_SYNC_STATUS_KHR:
            *value = EGL_SIGNALED_KHR;
            return EGL_TRUE;
        case EGL_SYNC_CONDITION_KHR:
            *value = EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
            return EGL_TRUE;
        default:
            *value = 0;
            return EGL_TRUE;
        }
    }
    if (real_eglGetSyncAttrib) {
        EGLAttrib v = 0;
        EGLBoolean r = real_eglGetSyncAttrib(dpy, (EGLSync)sync, attribute, &v);
        if (value) *value = (EGLint)v;
        return r;
    }
    return EGL_FALSE;
}

EGLBoolean eglWaitSyncKHR(EGLDisplay dpy, EGLSyncKHR sync, EGLint flags)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync(sync)) {
        if (real_glFlush) real_glFlush();
        return EGL_TRUE;
    }
    if (real_eglWaitSync) {
        return real_eglWaitSync(dpy, (EGLSync)sync, flags);
    }
    if (real_glFlush) real_glFlush();
    return EGL_TRUE;
}

/* Core EGL 1.5 sync wrappers for compatibility with modules resolving non-KHR symbols. */
EGLSync eglCreateSync(EGLDisplay dpy, EGLenum type, const EGLAttrib *attrib_list)
{
    ensure_init();
    resolve_all();
    if (real_eglCreateSync) {
        EGLSync sync = real_eglCreateSync(dpy, type, attrib_list);
        if (sync != EGL_NO_SYNC)
            return sync;
    }
    if (type == EGL_SYNC_FENCE_KHR || type == 0x30F9 /* EGL_SYNC_FENCE */) {
        if (real_glFlush) real_glFlush();
        return (EGLSync)(uintptr_t)0x1;
    }
    return EGL_NO_SYNC;
}

EGLBoolean eglDestroySync(EGLDisplay dpy, EGLSync sync)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync_core(sync))
        return EGL_TRUE;
    if (real_eglDestroySync)
        return real_eglDestroySync(dpy, sync);
    return EGL_FALSE;
}

EGLint eglClientWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags, EGLTime timeout)
{
    ensure_init();
    resolve_all();
    (void)flags;
    if (is_fake_fence_sync_core(sync)) {
        if (timeout > 0 && real_glFinish)
            real_glFinish();
        else if (real_glFlush)
            real_glFlush();
        return EGL_CONDITION_SATISFIED_KHR;
    }
    if (real_eglClientWaitSync)
        return real_eglClientWaitSync(dpy, sync, flags, timeout);
    if (timeout > 0 && real_glFinish) real_glFinish();
    return EGL_CONDITION_SATISFIED_KHR;
}

EGLBoolean eglGetSyncAttrib(EGLDisplay dpy, EGLSync sync, EGLint attribute, EGLAttrib *value)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync_core(sync)) {
        if (!value) return EGL_FALSE;
        switch (attribute) {
        case EGL_SYNC_TYPE_KHR:
            *value = (EGLAttrib)EGL_SYNC_FENCE_KHR;
            return EGL_TRUE;
        case EGL_SYNC_STATUS_KHR:
            *value = (EGLAttrib)EGL_SIGNALED_KHR;
            return EGL_TRUE;
        case EGL_SYNC_CONDITION_KHR:
            *value = (EGLAttrib)EGL_SYNC_PRIOR_COMMANDS_COMPLETE_KHR;
            return EGL_TRUE;
        default:
            *value = 0;
            return EGL_TRUE;
        }
    }
    if (real_eglGetSyncAttrib)
        return real_eglGetSyncAttrib(dpy, sync, attribute, value);
    return EGL_FALSE;
}

EGLBoolean eglWaitSync(EGLDisplay dpy, EGLSync sync, EGLint flags)
{
    ensure_init();
    resolve_all();
    if (is_fake_fence_sync_core(sync)) {
        if (real_glFlush) real_glFlush();
        return EGL_TRUE;
    }
    if (real_eglWaitSync)
        return real_eglWaitSync(dpy, sync, flags);
    if (real_glFlush) real_glFlush();
    return EGL_TRUE;
}

void (*eglGetProcAddress(const char *procname))(void)
{
    ensure_init();
    resolve_all();
    if (procname) {
        if (strcmp(procname, "eglQueryString") == 0) return (void (*)(void))shim_eglQueryString;
        if (strcmp(procname, "eglCreateSync") == 0) return (void (*)(void))eglCreateSync;
        if (strcmp(procname, "eglDestroySync") == 0) return (void (*)(void))eglDestroySync;
        if (strcmp(procname, "eglClientWaitSync") == 0) return (void (*)(void))eglClientWaitSync;
        if (strcmp(procname, "eglGetSyncAttrib") == 0) return (void (*)(void))eglGetSyncAttrib;
        if (strcmp(procname, "eglWaitSync") == 0) return (void (*)(void))eglWaitSync;
        if (strcmp(procname, "eglCreateSyncKHR") == 0) return (void (*)(void))eglCreateSyncKHR;
        if (strcmp(procname, "eglDestroySyncKHR") == 0) return (void (*)(void))eglDestroySyncKHR;
        if (strcmp(procname, "eglClientWaitSyncKHR") == 0) return (void (*)(void))eglClientWaitSyncKHR;
        if (strcmp(procname, "eglGetSyncAttribKHR") == 0) return (void (*)(void))eglGetSyncAttribKHR;
        if (strcmp(procname, "eglWaitSyncKHR") == 0) return (void (*)(void))eglWaitSyncKHR;
    }
    if (real_eglGetProcAddress)
        return real_eglGetProcAddress(procname);
    /* Fallback: try to resolve via real dlsym from libEGL if needed. */
    resolve_real_dlsym();
    if (real_dlsym)
        return (void (*)(void))real_dlsym(RTLD_NEXT, "eglGetProcAddress");
    return NULL;
}

void *dlsym(void *handle, const char *symbol)
{
    ensure_init();
    resolve_real_dlsym();

    void *real = real_dlsym ? real_dlsym(handle, symbol) : NULL;
    void *result = rgbswap_wrap_dlsym_result(handle, symbol, real);

    if (!symbol)
        return result;

    resolve_all();

#define RET_IF(name) if (strcmp(symbol, #name) == 0) return (void *)name
    RET_IF(glXCreatePixmap);
    RET_IF(glXDestroyPixmap);
    RET_IF(glXBindTexImageEXT);
    RET_IF(glXReleaseTexImageEXT);
    RET_IF(glXChooseFBConfig);
    RET_IF(glXChooseVisual);
    RET_IF(eglGetProcAddress);
    RET_IF(eglCreateSync);
    RET_IF(eglDestroySync);
    RET_IF(eglClientWaitSync);
    RET_IF(eglGetSyncAttrib);
    RET_IF(eglWaitSync);
    RET_IF(eglCreateSyncKHR);
    RET_IF(eglDestroySyncKHR);
    RET_IF(eglClientWaitSyncKHR);
    RET_IF(eglGetSyncAttribKHR);
    RET_IF(eglWaitSyncKHR);
    RET_IF(dlsym);
    RET_IF(glXGetProcAddress);
    RET_IF(glXGetProcAddressARB);
#undef RET_IF
    if (strcmp(symbol, "eglQueryString") == 0) return (void *)shim_eglQueryString;

    return result;
}

__GLXextFuncPtr glXGetProcAddress(const GLubyte *procName)
{
    ensure_init();
    resolve_all();
    if (procName) {
        const char *s = (const char *)procName;
        if (strcmp(s, "glXBindTexImageEXT") == 0) return (__GLXextFuncPtr)glXBindTexImageEXT;
        if (strcmp(s, "glXReleaseTexImageEXT") == 0) return (__GLXextFuncPtr)glXReleaseTexImageEXT;
        if (strcmp(s, "glXCreatePixmap") == 0) return (__GLXextFuncPtr)glXCreatePixmap;
        if (strcmp(s, "glXDestroyPixmap") == 0) return (__GLXextFuncPtr)glXDestroyPixmap;
    }
    if (real_glXGetProcAddress) return real_glXGetProcAddress(procName);
    return NULL;
}

__GLXextFuncPtr glXGetProcAddressARB(const GLubyte *procName)
{
    ensure_init();
    resolve_all();
    if (procName) {
        const char *s = (const char *)procName;
        if (strcmp(s, "glXBindTexImageEXT") == 0) return (__GLXextFuncPtr)glXBindTexImageEXT;
        if (strcmp(s, "glXReleaseTexImageEXT") == 0) return (__GLXextFuncPtr)glXReleaseTexImageEXT;
        if (strcmp(s, "glXCreatePixmap") == 0) return (__GLXextFuncPtr)glXCreatePixmap;
        if (strcmp(s, "glXDestroyPixmap") == 0) return (__GLXextFuncPtr)glXDestroyPixmap;
    }
    if (real_glXGetProcAddressARB) return real_glXGetProcAddressARB(procName);
    return glXGetProcAddress(procName);
}
