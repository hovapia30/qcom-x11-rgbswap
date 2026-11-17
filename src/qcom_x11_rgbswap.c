/*
 * libqcom_x11_rgbswap.so — LD_PRELOAD fix for swapped R/B channels with the
 * proprietary Qualcomm Adreno EGL/X11 subdriver (libeglSubDriverX11.so.1).
 *
 * The bug (confirmed empirically with a gdb experiment):
 *   During eglChooseConfig(), libGLESv2_adreno.so.2 creates EGL configs through
 *   libeglSubDriverX11.so.1::CreateSubDriver()->vtable[5] = CreateConfig(),
 *   passing a Qualcomm EGL pixel format. For X visual 0x44
 *   (depth = 32, red_mask = 0xff0000, i.e. BGRX in memory) the driver picks
 *     - 0x1fb (XBGR8888, RGBA in memory, no alpha)  -> should be 0x58 (XRGB8888)
 *     - 0x1c  (ABGR8888, RGBA in memory, with alpha) -> should be 0x57 (ARGB8888)
 *   As a result the renderer writes RGBA while X reads BGRX, so the red and blue
 *   channels end up swapped. Vulkan (libvulkan_adreno.so.1) is fine: its WSI
 *   adapts to the X visual masks on its own.
 *
 * How we hook it:
 *   libGLESv2 does not obtain CreateSubDriver from the global symbol table; it
 *   uses dlopen("libeglSubDriverX11.so.1") + dlsym(). So we intercept dlsym()
 *   and, when "CreateSubDriver" is requested, return our own wrapper. The
 *   wrapper calls the real CreateSubDriver, copies the subdriver object's vtable
 *   and replaces entry [5] (CreateConfig) with a wrapper that rewrites the pixel
 *   format BEFORE the config is created. This changes both the render swizzle
 *   and the DRM fourcc of the back buffer (via iQctPixelFormat), so zero-copy
 *   DMA-BUF and DRI3/GBM are preserved (no DISABLE_DRI3 hack needed).
 *
 * Controls:
 *   QCOM_X11_RGBSWAP=0          — disable entirely (enabled by default).
 *   QCOM_X11_RGBSWAP_VERBOSE=1  — log every pixel format rewrite.
 *
 * Build:  cc -shared -fPIC -O2 -o libqcom_x11_rgbswap.so qcom_x11_rgbswap.c -ldl
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
        fprintf(stderr,
                "[rgbswap] hooked subdriver obj=%p vtable %p -> %p, CreateConfig %p -> %p\n",
                obj, (void *)orig_vt, (void *)new_vt, real_cfg, (void *)wrap_CreateConfig);
        wrap_logged = 1;
    }
    return obj;
}

static void *do_CreateSubDriver(void) {
    if (!real_CreateSubDriver) {
        void *h = dlopen("libeglSubDriverX11.so.1", RTLD_NOW | RTLD_NOLOAD);
        if (!h) h = dlopen("libeglSubDriverX11.so.1", RTLD_NOW);
        if (h) real_CreateSubDriver = (CreateSubDriver_t)get_real_dlsym()(h, "CreateSubDriver");
        if (!real_CreateSubDriver) {
            fprintf(stderr, "[rgbswap] CreateSubDriver: real symbol not found\n");
            return NULL;
        }
    }
    return wrap_subdriver(real_CreateSubDriver());
}

/* dlsym interception: we only substitute "CreateSubDriver". */
void *dlsym(void *handle, const char *symbol) {
    dlsym_fn fn = get_real_dlsym();
    void *r = fn ? fn(handle, symbol) : NULL;
    if (r && strcmp(symbol, "CreateSubDriver") == 0) {
        real_CreateSubDriver = (CreateSubDriver_t)r;
        if (verbose())
            fprintf(stderr, "[rgbswap] dlsym(\"CreateSubDriver\") -> wrapper\n");
        return (void *)do_CreateSubDriver;
    }
    return r;
}

/* In case the symbol is resolved through the global table instead. */
void *CreateSubDriver(void) {
    return do_CreateSubDriver();
}
