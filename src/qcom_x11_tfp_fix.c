/*
 * qcom_x11_tfp_fix.c - TFP attribute override (fix task)
 *
 * Rewrites GLX texture-from-pixmap attributes in glXCreatePixmap and
 * glXBindTexImageEXT to work around cases where redirected/composited window
 * contents do not appear with certain driver/interop combinations.
 *
 * This file implements the fix logic and is linked together with
 * qcom_x11_tfp_trace.c into a single libqcom_x11_tfp.so. The trace module
 * provides the interposed GLX/dlsym entry points and calls into this module.
 *
 * Environment:
 *   QCOM_TFP_FORCE_FORMAT = rgb | rgba   (override GLX_TEXTURE_FORMAT_EXT)
 *   QCOM_TFP_FORCE_TARGET = 2d  | rect   (override GLX_TEXTURE_TARGET_EXT)
 *   QCOM_TFP_LOG          = path or 1    (log to file, or stderr if "1"; default: off)
 */

#define _GNU_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <GL/glx.h>
#include <GL/glxext.h>

#ifndef GLX_TEXTURE_FORMAT_RGB_EXT
#define GLX_TEXTURE_FORMAT_RGB_EXT  0x20D9
#endif
#ifndef GLX_TEXTURE_FORMAT_RGBA_EXT
#define GLX_TEXTURE_FORMAT_RGBA_EXT 0x20DA
#endif
#ifndef GLX_TEXTURE_2D_EXT
#define GLX_TEXTURE_2D_EXT          0x20DC
#endif
#ifndef GLX_TEXTURE_RECTANGLE_EXT
#define GLX_TEXTURE_RECTANGLE_EXT   0x20DD
#endif

static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static FILE *logf;
static int do_log;

static int force_format; /* 0=none, 1=RGB, 2=RGBA */
static int force_target; /* 0=none, 1=2D,  2=RECT */

static void tlog(const char *fmt, ...)
{
    if (!do_log) return;
    va_list ap;
    pthread_mutex_lock(&mu);
    FILE *out = logf ? logf : stderr;
    fprintf(out, "[tfp-fix pid=%d] ", (int)getpid());
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fprintf(out, "\n");
    fflush(out);
    pthread_mutex_unlock(&mu);
}

static void do_init(void)
{
    const char *lf = getenv("QCOM_TFP_LOG");
    if (lf && *lf && strcmp(lf, "0") != 0) {
        do_log = 1;
        if (strcmp(lf, "1") != 0) logf = fopen(lf, "a");
    }
    const char *fmt = getenv("QCOM_TFP_FORCE_FORMAT");
    if (fmt) {
        if (strcasecmp(fmt, "rgb") == 0) force_format = 1;
        else if (strcasecmp(fmt, "rgba") == 0) force_format = 2;
    }
    const char *tgt = getenv("QCOM_TFP_FORCE_TARGET");
    if (tgt) {
        if (strcasecmp(tgt, "2d") == 0) force_target = 1;
        else if (strcasecmp(tgt, "rect") == 0 || strcasecmp(tgt, "rectangle") == 0) force_target = 2;
    }
    tlog("init force_format=%d force_target=%d", force_format, force_target);
}

void tfp_fix_init(void)
{
    pthread_once(&init_once, do_init);
}

/* Rewrite attrib list into caller-provided buffer (must be large enough: 64 ints).
 * Returns pointer to rewritten list, or original if no changes needed. */
const int *tfp_fix_rewrite_attribs(const int *in, int *out, int outcap, const char *where)
{
    tfp_fix_init();
    if (!in || (!force_format && !force_target)) return in;
    int i = 0, j = 0;
    int changed = 0;
    while (in[i] != None && j + 3 < outcap) {
        int key = in[i];
        int val = in[i + 1];
        if (force_format && key == GLX_TEXTURE_FORMAT_EXT) {
            int nv = (force_format == 1) ? GLX_TEXTURE_FORMAT_RGB_EXT : GLX_TEXTURE_FORMAT_RGBA_EXT;
            if (nv != val) { tlog("%s: format 0x%x -> 0x%x", where, val, nv); changed = 1; }
            val = nv;
        } else if (force_target && key == GLX_TEXTURE_TARGET_EXT) {
            int nv = (force_target == 1) ? GLX_TEXTURE_2D_EXT : GLX_TEXTURE_RECTANGLE_EXT;
            if (nv != val) { tlog("%s: target 0x%x -> 0x%x", where, val, nv); changed = 1; }
            val = nv;
        }
        out[j++] = key;
        out[j++] = val;
        i += 2;
    }
    out[j] = None;
    if (changed) tlog("%s: attribs rewritten", where);
    return changed ? out : in;
}
