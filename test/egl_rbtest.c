/* egl_rbtest.c — deterministic R/B channel-order test for EGL/X11.
 *
 * Draws an override-redirect window: the left half is pure red (255,0,0) and
 * the right half is pure blue (0,0,255). It stays on screen until killed.
 *
 * Interpretation:
 *   left  = red   and right = blue   -> channel order is correct
 *   left  = blue  and right = red    -> R/B are swapped (the bug)
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "no display\n"); return 1; }
    int scr = DefaultScreen(d);
    Window root = RootWindow(d, scr);
    int W = 400, H = 400;

    XSetWindowAttributes swa;
    swa.event_mask = ExposureMask | StructureNotifyMask;
    swa.background_pixel = BlackPixel(d, scr);
    swa.override_redirect = True;   /* on top of everything, no decorations */
    Window win = XCreateWindow(d, root, 20, 20, W, H, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWEventMask | CWBackPixel | CWOverrideRedirect, &swa);
    XStoreName(d, win, "egl_rbtest");
    XMapRaised(d, win);
    XFlush(d);
    usleep(300000);

    EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)d);
    if (edpy == EGL_NO_DISPLAY) { fprintf(stderr, "eglGetDisplay failed\n"); return 2; }
    if (!eglInitialize(edpy, 0, 0)) { fprintf(stderr, "eglInitialize failed\n"); return 2; }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint cfgattr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(edpy, cfgattr, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig failed\n"); return 2;
    }
    EGLint visid = -1;
    eglGetConfigAttrib(edpy, cfg, EGL_NATIVE_VISUAL_ID, &visid);
    fprintf(stderr, "chosen config EGL_NATIVE_VISUAL_ID=0x%x\n", visid);
    {
        XVisualInfo vitemp; vitemp.visualid = (VisualID)visid;
        XVisualInfo *vi = XGetVisualInfo(d, VisualIDMask, &vitemp, &n);
        if (vi) {
            fprintf(stderr, "visual depth=%d class=%d red_mask=0x%lx green_mask=0x%lx blue_mask=0x%lx\n",
                    vi->depth, vi->class, vi->red_mask, vi->green_mask, vi->blue_mask);
            XFree(vi);
        } else {
            fprintf(stderr, "XGetVisualInfo failed for visualid 0x%x\n", visid);
        }
    }

    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctxattr);
    EGLSurface surf = eglCreateWindowSurface(edpy, cfg, win, 0);
    if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE) {
        fprintf(stderr, "ctx/surface failed: eglGetError=0x%x\n", eglGetError());
        return 2;
    }
    eglMakeCurrent(edpy, surf, surf, ctx);

    glViewport(0, 0, W, H);
    glDisable(GL_DITHER);
    /* left half — red */
    glScissor(0, 0, W / 2, H);
    glEnable(GL_SCISSOR_TEST);
    glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    /* right half — blue */
    glScissor(W / 2, 0, W / 2, H);
    glClearColor(0.0f, 0.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
    glFinish();
    eglSwapBuffers(edpy, surf);
    glFinish();

    fprintf(stderr, "RENDERED: left=RED(255,0,0) right=BLUE(0,0,255) win=0x%lx\n", (unsigned long)win);
    fflush(stderr);
    sleep(30);
    return 0;
}
