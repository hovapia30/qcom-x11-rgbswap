/*
 * qcom_x11_tfp_trace.c - diagnostic/trace task (and interposition entry point)
 *
 * Logs GLX texture-from-pixmap related calls made by the compositor
 * (glXCreatePixmap, glXDestroyPixmap, glXBindTexImageEXT, glXReleaseTexImageEXT,
 *  glXChooseFBConfig, glXChooseVisual) to help diagnose why redirected window
 * contents may not appear when compositing.
 *
 * This file provides the interposed GLX/dlsym entry points for the combined
 * libqcom_x11_tfp.so and is linked together with qcom_x11_tfp_fix.c (attribute
 * override/fix task). TFP attributes are passed through the fix module before
 * being forwarded to the real driver.
 *
 * Output: stderr by default, or the file named by $QCOM_TFP_TRACE_LOG.
 * Enable/disable: set QCOM_TFP_TRACE=0 to disable (default: enabled when loaded).
 *
 * Build (as part of combined .so):
 *   cc -shared -fPIC -O2 -Wall -Wextra -o libqcom_x11_tfp.so \
 *      src/qcom_x11_tfp_trace.c src/qcom_x11_tfp_fix.c -ldl -lX11 -lGL
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>

/* Implemented in qcom_x11_tfp_fix.c (fix task). */
extern const int *tfp_fix_rewrite_attribs(const int *in, int *out, int outcap, const char *where);

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
    R(glGetError);
    R(glFinish);
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

void *dlsym(void *handle, const char *symbol)
{
    ensure_init();
    resolve_real_dlsym();
    if (!symbol) return real_dlsym ? real_dlsym(handle, symbol) : NULL;
    resolve_all();

#define RET_IF(name) if (strcmp(symbol, #name) == 0) return (void *)name
    RET_IF(glXCreatePixmap);
    RET_IF(glXDestroyPixmap);
    RET_IF(glXBindTexImageEXT);
    RET_IF(glXReleaseTexImageEXT);
    RET_IF(glXChooseFBConfig);
    RET_IF(glXChooseVisual);
    RET_IF(dlsym);
    RET_IF(glXGetProcAddress);
    RET_IF(glXGetProcAddressARB);
#undef RET_IF

    return real_dlsym ? real_dlsym(handle, symbol) : NULL;
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
