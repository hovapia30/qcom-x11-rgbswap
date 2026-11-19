# qcom-x11-tfp / Adreno X11 shim

A single `LD_PRELOAD` shim (`libqcom_x11_tfp.so`) for the proprietary Qualcomm
Adreno X11 EGL/GLX stack in a glibc environment, built from three C source files
that solve three different tasks:

1. **R/B channel swap fix** (`src/qcom_x11_rgbswap.c`) — fixes swapped red/blue
   channels in GLES/EGL applications on the Adreno X11 EGL subdriver when the X
   server exposes a 32-bit BGRX visual.
2. **TFP diagnostics/tracing** (`src/qcom_x11_tfp_trace.c`) — logs GLX
   texture-from-pixmap and related calls to diagnose redirected/composited
   window content issues.
3. **TFP attribute override/fix** (`src/qcom_x11_tfp_fix.c`) — can rewrite GLX
   texture-from-pixmap attributes (format/target) as a workaround.

The shim is self-contained: no patching of the proprietary driver and no forced
software fallback. The R/B-swap path preserves zero-copy DMA-BUF/DRI3/GBM.

## Required setup

For the proprietary Adreno X11 EGL stack:

* this shim, and
* the DRI3-patched Termux:X11 build —
  [`hovapia30/termux-x11`](https://github.com/hovapia30/termux-x11),
  branch `ed43f97-dri3`.

Vulkan additionally requires `DISABLE_UBWC_VK=1`.

---

## Task 1: R/B channel swap fix

### Symptom

Any GLES/EGL client (Firefox/WebRender, `es2gears_x11`, `glmark2-es2`, ...)
renders with the red and blue channels exchanged: a color that should be
`rgb(255,0,0)` shows up as `rgb(0,0,255)` and vice versa. Vulkan clients are
**not** affected — the Adreno Vulkan WSI adapts to the X visual masks itself.

### Root cause

During `eglChooseConfig()`, the GLES driver (`libGLESv2_adreno.so.2`) creates
EGL configs through

```
libeglSubDriverX11.so.1::CreateSubDriver()->vtable[5] = CreateConfig()
```

passing a Qualcomm EGL pixel format. For an X visual with `depth = 32` and
`red_mask = 0xff0000` (i.e. **BGRX** in memory) the driver picks a format whose
memory layout is **RGBA**:

| driver picks | Qualcomm value | memory layout   | should be          | Qualcomm value |
|--------------|----------------|-----------------|--------------------|----------------|
| `XBGR8888`   | `0x1fb`        | RGBA (no alpha) | `XRGB8888` (BGRX)  | `0x58`         |
| `ABGR8888`   | `0x1c`         | RGBA (alpha)    | `ARGB8888` (BGRX)  | `0x57`         |

The renderer therefore writes RGBA while X reads BGRX, and the red and blue
channels end up swapped.

### How it works

`libGLESv2` resolves `CreateSubDriver` via `dlopen` + `dlsym()`. The shim
interposes `dlsym()` (resolving the real one through
`dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34")` to avoid recursion), wraps
`CreateSubDriver`, copies the subdriver vtable, and replaces entry `[5]`
(`CreateConfig`) with a wrapper that rewrites the pixel format *before* the
config is created: `0x1fb -> 0x58`, `0x1c -> 0x57`. Because the rewrite happens
at config creation time, it changes both the renderer swizzle and the DRM
fourcc of the back buffer, so the fast zero-copy path (DMA-BUF / DRI3 / GBM) is
kept.

---

## Task 2: TFP diagnostics/tracing

`src/qcom_x11_tfp_trace.c` logs GLX texture-from-pixmap and related calls made
by compositors: `glXCreatePixmap`, `glXDestroyPixmap`, `glXBindTexImageEXT`,
`glXReleaseTexImageEXT`, `glXChooseFBConfig`, and `glXChooseVisual`. It can
optionally sample bound textures via FBO readback to inspect actual pixel data.

It also provides the unified interposed `dlsym`/`glXGetProcAddress` entry
points for the combined library and forwards TFP attributes through the fix
module and dlsym results through the R/B-swap module.

## Task 3: TFP attribute override/fix

`src/qcom_x11_tfp_fix.c` can rewrite texture-from-pixmap attributes in
`glXCreatePixmap` and `glXBindTexImageEXT` (format/target) to work around cases
where redirected/composited window contents do not appear as expected.

---

## Repository layout

```
.
├── Makefile
├── LICENSE
├── README.md
├── src/qcom_x11_rgbswap.c     # task 1: Adreno EGL/X11 R/B channel swap fix
├── src/qcom_x11_tfp_trace.c   # task 2: GLX TFP diagnostics/tracing (+ unified hooks)
├── src/qcom_x11_tfp_fix.c     # task 3: GLX TFP attribute override/fix
└── test/egl_rbtest.c          # deterministic R/B channel-order test
```

## Requirements

* A 64-bit glibc Linux environment (do **not** preload into a bionic/Android
  process).
* The proprietary Qualcomm Adreno EGL stack:
  `libGLESv2_adreno.so.2`, `libeglSubDriverX11.so.1`.
* An X server exposing a depth-32 BGRX visual (Termux:X11 / Lorie, Xorg, ...).

## Build

```sh
make                     # builds libqcom_x11_tfp.so and egl_rbtest
make install             # installs libqcom_x11_tfp.so into /usr/local/lib
```

## Install and use

Preload the shim into GUI applications (not into the X server, and not into any
bionic/Android process):

```sh
LD_PRELOAD=/usr/local/lib/libqcom_x11_tfp.so firefox
LD_PRELOAD=/usr/local/lib/libqcom_x11_tfp.so es2gears_x11
```

TFP tracing (diagnostic):

```sh
LD_PRELOAD=/usr/local/lib/libqcom_x11_tfp.so \
QCOM_TFP_TRACE_LOG=/tmp/tfp.log \
kwin_x11 --replace
```

TFP attribute override (experimental):

```sh
LD_PRELOAD=/usr/local/lib/libqcom_x11_tfp.so \
QCOM_TFP_FORCE_FORMAT=rgba \
QCOM_TFP_LOG=/tmp/tfp-fix.log \
kwin_x11 --replace
```

## Environment variables

| Variable                   | Default | Meaning                                                                 |
|----------------------------|---------|-------------------------------------------------------------------------|
| `QCOM_X11_RGBSWAP`         | on      | `0` disables the R/B-swap fix.                                          |
| `QCOM_X11_RGBSWAP_VERBOSE` | off     | `1` logs every pixel-format rewrite to stderr.                          |
| `QCOM_TFP_TRACE`           | on      | `0` disables TFP tracing (set to avoid overhead when not debugging).    |
| `QCOM_TFP_TRACE_LOG`       | stderr  | Trace log file path (also enables tracing).                             |
| `QCOM_TFP_TRACE_READBACK`  | off     | `1` samples a few bound textures via FBO readback.                      |
| `QCOM_TFP_TRACE_SYNC`      | off     | `1` does `glFinish` + short delay before readback.                      |
| `QCOM_TFP_FORCE_FORMAT`    | —       | `rgb` or `rgba` (overrides `GLX_TEXTURE_FORMAT_EXT`).                   |
| `QCOM_TFP_FORCE_TARGET`    | —       | `2d` or `rect` (overrides `GLX_TEXTURE_TARGET_EXT`).                    |
| `QCOM_TFP_LOG`             | off     | Rewrite log file path, or `1` for stderr (logs only actual rewrites).   |
| `DISABLE_UBWC_VK`          | —       | Required for the Adreno Vulkan driver.                                  |

## Test

`test/egl_rbtest.c` renders two half-rects and reads pixels back via
`glReadPixels`:

* left half: R=`1.0`, G=`0.0`, B=`0.0` (alpha `1.0`)
* right half: R=`0.0`, G=`0.0`, B=`1.0` (alpha `1.0`)

It expects BGRX memory order `{0,0,255,255}` on the left and `{255,0,0,255}`
on the right and prints `OK` or `SWAPPED`.

```sh
make                     # builds egl_rbtest
./egl_rbtest             # buggy driver: SWAPPED
LD_PRELOAD=./libqcom_x11_tfp.so ./egl_rbtest   # fixed: OK
```

## Notes and limitations

* The R/B-swap fix only rewrites the two known Qualcomm formats (`0x1fb`,
  `0x1c`). Unknown formats are passed through unchanged.
* The vtable copy is never freed (lives for the lifetime of the process).
* `libeglSubDriverWayland.so.1` is not handled; the R/B fix is X11-specific.
* If a future driver version returns an already-correct format, the shim is a
  no-op for it.

## License

MIT — see [LICENSE](LICENSE).
