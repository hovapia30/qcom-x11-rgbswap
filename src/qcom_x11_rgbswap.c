/*
 * qcom_x11_rgbswap.c - R/B channel swap fix (task)
 *
 * LD_PRELOAD fix for swapped R/B channels with the proprietary Qualcomm Adreno
 * EGL/X11 subdriver (libeglSubDriverX11.so.1).
 *
 * libeglSubDriverX11 picks RGBA-in-memory pixel formats for BGRX X visuals, so
 * the renderer writes RGBA while X reads BGRX and red/blue channels are swapped.
 * This hooks subdriver CreateConfig (via CreateSubDriver) and rewrites the
 * pixel format before the config is created, preserving zero-copy DMA-BUF/DRI3.
 *
 * This file implements the R/B-swap task and is linked together with
 * qcom_x11_tfp_trace.c and qcom_x11_tfp_fix.c into a single libqcom_x11_tfp.so.
 * The trace module provides the unified interposed dlsym/glXGetProcAddress entry
 * points and consults this module for CreateSubDriver interception.
 *
 * Controls:
 *   QCOM_X11_RGBSWAP=0          - disable entirely (enabled by default).
 *   QCOM_X11_RGBSWAP_VERBOSE=1  - log every pixel format rewrite.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>

/* Qualcomm EGL pixel formats (verified by disassembling iQctPixelFormat @0x82c0). */
#define QCT_PF_XBGR8888 0x1fb /* -> DRM XBGR8888 (RGBA in memory) */
#define QCT_PF_ABGR8888 0x1c  /* -> DRM ABGR8888 (RGBA in memory) */
#define QCT_PF_XRGB8888 0x58  /* -> DRM XRGB8888 (BGRX in memory) */
#define QCT_PF_ARGB8888 0x57  /* -> DRM ARGB8888 (BGRX in memory) */

#define VT_CREATECONFIG_INDEX 5 /* vtable[5] == CreateConfig */
#define VT_COPY_ENTRIES 40      /* copied with headroom (the real table has ~16) */

static int enabled(void) {
    const char *e = getenv("QCOM_X11_RGBSWAP");
    return !(e && e[0] == '0');
}

static int verbose(void) {
    const char *e = getenv("QCOM_X11_RGBSWAP_VERBOSE");
    return e && e[0] != '0';
}

static uint32_t remap_pf(uint32_t pf) {
    switch (pf) {
        case QCT_PF_XBGR8888: return QCT_PF_XRGB8888;
        case QCT_PF_ABGR8888: return QCT_PF_ARGB8888;
        default: return pf;
    }
}

/* ---- the real dlsym (via dlvsym, so we do not recurse into ourselves) ---- */
typedef void *(*dlsym_fn)(void *, const char *);
static dlsym_fn real_dlsym = NULL;

static dlsym_fn get_real_dlsym(void) {
    if (!real_dlsym) {
        real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
        if (!real_dlsym)
            real_dlsym = (dlsym_fn)dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    }
    return real_dlsym;
}

/* ---- CreateConfig wrapper ---- */
/* CreateConfig(void *self, void *display, uint32_t pixelFormat, void *unused, void *out) */
typedef int (*CreateConfig_t)(void *self, void *display, uint32_t pf, void *x3, void *out);
static CreateConfig_t orig_CreateConfig = NULL;

static int wrap_CreateConfig(void *self, void *display, uint32_t pf, void *x3, void *out) {
    uint32_t nf = remap_pf(pf);
    if (nf != pf && verbose())
        fprintf(stderr, "[rgbswap] CreateConfig pf 0x%x -> 0x%x\n", pf, nf);
    return orig_CreateConfig(self, display, nf, x3, out);
}

/* ---- CreateSubDriver wrapper ---- */
typedef void *(*CreateSubDriver_t)(void);
static CreateSubDriver_t real_CreateSubDriver = NULL;
static int wrap_logged = 0;

static void *wrap_subdriver(void *obj) {
    if (!obj || !enabled()) return obj;
    void ***vt_slot = (void ***)obj; /* obj[0] == vtable */
    void **orig_vt = *vt_slot;
    if (!orig_vt) return obj;

    void *real_cfg = orig_vt[VT_CREATECONFIG_INDEX];
    if (real_cfg == (void *)wrap_CreateConfig) return obj; /* already wrapped */

    orig_CreateConfig = (CreateConfig_t)real_cfg;

    void **new_vt = (void **)malloc(VT_COPY_ENTRIES * sizeof(void *));
    if (!new_vt) return obj;
    memcpy(new_vt, orig_vt, VT_COPY_ENTRIES * sizeof(void *));
    new_vt[VT_CREATECONFIG_INDEX] = (void *)wrap_CreateConfig;
    *vt_slot = new_vt;

    if (!wrap_logged) {
        wrap_logged = 1;
        if (verbose())
            fprintf(stderr,
                    "[rgbswap] hooked subdriver obj=%p vtable %p -> %p, CreateConfig %p -> %p\n",
                    obj, (void *)orig_vt, (void *)new_vt, real_cfg, (void *)wrap_CreateConfig);
    }
    return obj;
}

static void *do_CreateSubDriver(void) {
    if (!real_CreateSubDriver) {
        void *h = dlopen("libeglSubDriverX11.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libeglSubDriverX11.so.1", RTLD_NOW);
        if (h) {
            dlsym_fn fn = get_real_dlsym();
            if (fn)
                real_CreateSubDriver = (CreateSubDriver_t)fn(h, "CreateSubDriver");
        }
        if (!real_CreateSubDriver) {
            fprintf(stderr, "[rgbswap] CreateSubDriver: real symbol not found\n");
            return NULL;
        }
    }
    return wrap_subdriver(real_CreateSubDriver());
}

/* Called by the unified dlsym hook with the real dlsym result. */
void *rgbswap_wrap_dlsym_result(void *handle, const char *symbol, void *result) {
    (void)handle;
    if (result && symbol && strcmp(symbol, "CreateSubDriver") == 0) {
        real_CreateSubDriver = (CreateSubDriver_t)result;
        if (verbose())
            fprintf(stderr, "[rgbswap] dlsym(\"CreateSubDriver\") -> wrapper\n");
        return (void *)do_CreateSubDriver;
    }
    return result;
}

/* In case the symbol is resolved through the global table instead. */
void *CreateSubDriver(void) {
    return do_CreateSubDriver();
}

#ifdef QCOM_RGBSWAP_STANDALONE
/* Standalone build needs its own dlsym interposition; the combined shim
 * calls rgbswap_wrap_dlsym_result() from its unified dlsym hook instead. */
void *dlsym(void *handle, const char *symbol)
{
    dlsym_fn fn = get_real_dlsym();
    void *real = fn ? fn(handle, symbol) : NULL;
    return rgbswap_wrap_dlsym_result(handle, symbol, real);
}
#endif
