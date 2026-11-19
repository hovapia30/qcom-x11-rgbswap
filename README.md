# libqcom_x11_rgbswap

An `LD_PRELOAD` shim that fixes **swapped red/blue channels** in OpenGL ES /
EGL applications running on the **proprietary Qualcomm Adreno X11 EGL
subdriver** (`libeglSubDriverX11.so.1`) under an X server that exposes a
32-bit BGRX visual (for example Termux:X11 / Lorie).

It is a small, self-contained C library: no patching of the proprietary driver,
no loss of zero-copy DMA-BUF or DRI3/GBM, and no forced software fallback.

## Required setup

Both of these are required for the proprietary Adreno X11 EGL stack:

* this shim, and
* the DRI3-patched Termux:X11 build —
  [`hovapia30/termux-x11`](https://github.com/hovapia30/termux-x11),
  branch `ed43f97-dri3`.

Vulkan additionally requires `DISABLE_UBWC_VK=1`.

---

## Symptom

Any GLES/EGL client (Firefox/WebRender, `es2gears_x11`, `glmark2-es2`, ...)
renders with the red and blue channels exchanged: a page that should be
`rgb(255,0,0)` shows up as `rgb(0,0,255)` and vice versa. Vulkan clients are
**not** affected — the Adreno Vulkan WSI adapts to the X visual masks itself.

## Root cause

During `eglChooseConfig()`, the GLES driver (`libGLESv2_adreno.so.2`) creates
EGL configs through

```
libeglSubDriverX11.so.1::CreateSubDriver()->vtable[5] = CreateConfig()
```

passing a **Qualcomm EGL pixel format**. For an X visual with
`depth = 32` and `red_mask = 0xff0000` (i.e. **BGRX** in memory, visual `0x44`
on Lorie) the driver picks a format whose memory layout is **RGBA**:

| driver picks           | Qualcomm value | memory layout | should be              | Qualcomm value |
|------------------------|----------------|---------------|------------------------|----------------|
| `XBGR8888`             | `0x1fb`        | RGBA (no alpha) | `XRGB8888` (BGRX)    | `0x58`         |
| `ABGR8888`             | `0x1c`         | RGBA (alpha)    | `ARGB8888` (BGRX)    | `0x57`         |

The renderer therefore writes RGBA while X reads BGRX, and the red and blue
channels end up swapped. (Verified by disassembling `iQctPixelFormat` at
offset `0x82c0` and with a gdb experiment on `CreateConfig`.)

## How the shim works

`libGLESv2` does **not** resolve `CreateSubDriver` from the global symbol table —
it uses `dlopen("libeglSubDriverX11.so.1")` followed by `dlsym()`. The shim
therefore:

1. interposes `dlsym()` (resolving the real one through
   `dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34")` to avoid recursion);
2. when a caller asks for `"CreateSubDriver"`, returns its own wrapper;
3. the wrapper calls the real `CreateSubDriver()`, **copies the subdriver
   object's vtable** and replaces entry `[5]` (`CreateConfig`) with a wrapper
   that rewrites the pixel format *before* the config is created:
   `0x1fb -> 0x58`, `0x1c -> 0x57`.

Because the rewrite happens at config creation time, it changes both the
renderer swizzle and the DRM fourcc of the back buffer (through
`iQctPixelFormat`), so the fast zero-copy path (DMA-BUF / DRI3 / GBM) is kept.

## Requirements

* A 64-bit glibc Linux environment (the shim is a glibc shared object — do **not**
  preload it into a bionic/Android process).
* The proprietary Qualcomm Adreno EGL stack:
  `libGLESv2_adreno.so.2`, `libeglSubDriverX11.so.1`.
* An X server exposing a depth-32 BGRX visual (Termux:X11 / Lorie, Xorg with a
  32-bit visual, ...).

## Build

```sh
make                     # builds libqcom_x11_rgbswap.so and egl_rbtest
make install             # installs the shim into /usr/local/lib
```

Manual build of just the shim:

```sh
cc -shared -fPIC -O2 -o libqcom_x11_rgbswap.so src/qcom_x11_rgbswap.c -ldl
```

## Install and use

Preload the shim into the GUI application (not into the X server, and not into
any bionic/Android process):

```sh
LD_PRELOAD=/usr/local/lib/libqcom_x11_rgbswap.so firefox
LD_PRELOAD=/usr/local/lib/libqcom_x11_rgbswap.so es2gears_x11
```

To make it apply to every session, put it into a login profile, e.g.
`/etc/profile.d/qcom-x11-rgbswap.sh`:

```sh
[ -f /usr/local/lib/libqcom_x11_rgbswap.so ] || return 0
case ":${LD_PRELOAD:-}:" in
    *:/usr/local/lib/libqcom_x11_rgbswap.so:*) ;;
    *) export LD_PRELOAD="/usr/local/lib/libqcom_x11_rgbswap.so${LD_PRELOAD:+:$LD_PRELOAD}" ;;
esac
```

## Environment variables

| Variable                     | Default | Meaning                                             |
|------------------------------|---------|-----------------------------------------------------|
| `QCOM_X11_RGBSWAP`           | on      | `0` disables the shim entirely.                     |
| `QCOM_X11_RGBSWAP_VERBOSE`   | off     | `1` logs every pixel-format rewrite to stderr.      |
| `DISABLE_UBWC_VK`            | —       | Required for the Adreno Vulkan driver.              |

Typical stderr output when active:

```
[rgbswap] hooked subdriver obj=0x... vtable 0x... -> 0x..., CreateConfig 0x... -> 0x...
```

## Verification

The bundled `egl_rbtest` draws a window split in half: **left = red**, **right =
blue**. Correct output:

```
left  = rgb(255,0,0)
right = rgb(0,0,255)
```

If the channels are swapped you will see the opposite. Run it with and without
the shim to compare:

```sh
./egl_rbtest                                    # broken (swapped)
LD_PRELOAD=./libqcom_x11_rgbswap.so ./egl_rbtest   # fixed
```

To capture and check pixels on the X server:

```sh
import -display :0 -window root /tmp/shot.png
magick /tmp/shot.png -format '%[pixel:p{100,200}] %[pixel:p{300,200}]\n' info:
```

## Notes and limitations

* The shim only rewrites the two known Qualcomm formats (`0x1fb`, `0x1c`).
  Unknown formats are passed through unchanged.
* The vtable copy is never freed (it lives for the lifetime of the process and
  is created once per subdriver).
* `libeglSubDriverWayland.so.1` is not handled; this is an X11-specific fix.
* If a future driver version returns an already-correct format, the shim is a
  no-op for it.

## Repository layout

```
.
├── Makefile
├── LICENSE
├── README.md
├── src/qcom_x11_rgbswap.c     # R/B-swap LD_PRELOAD shim
├── src/qcom_x11_tfp_trace.c   # TFP diagnostics/tracing task (interposition entry point)
├── src/qcom_x11_tfp_fix.c     # TFP attribute-override/fix task
└── test/egl_rbtest.c          # deterministic R/B channel-order test
```

## License

MIT — see [LICENSE](LICENSE).


---

## Diagnostic/experimental: `libqcom_x11_tfp.so`

Two separate C sources solve different tasks but are compiled and linked into
a single `LD_PRELOAD` shim:

- `src/qcom_x11_tfp_trace.c` — **diagnostics/tracing task**: logs GLX
  texture-from-pixmap calls (`glXCreatePixmap`, `glXDestroyPixmap`,
  `glXBindTexImageEXT`, `glXReleaseTexImageEXT`, `glXChooseFBConfig`,
  `glXChooseVisual`) with optional bound-texture readback, and provides the
  interposed GLX/dlsym entry points.
- `src/qcom_x11_tfp_fix.c` — **attribute-override/fix task**: rewrites TFP
  attributes (format/target) in `glXCreatePixmap` and `glXBindTexImageEXT` to
  work around redirected/composited window contents not appearing as expected.

Both are linked into one shared object (`libqcom_x11_tfp.so`), independent from
the R/B-swap shim.

### Build

```sh
make libqcom_x11_tfp.so
# or: make
```

### Usage

```sh
# trace TFP calls
LD_PRELOAD=/path/to/libqcom_x11_tfp.so QCOM_TFP_TRACE_LOG=/tmp/tfp.log kwin_x11 --replace

# force TFP attributes (optionally log rewrites)
LD_PRELOAD=/path/to/libqcom_x11_tfp.so QCOM_TFP_FORCE_FORMAT=rgba QCOM_TFP_LOG=/tmp/tfp-fix.log kwin_x11 --replace
```

* `QCOM_TFP_TRACE_LOG` — trace log file path (default: `stderr`).
* `QCOM_TFP_TRACE=0` — disable tracing (enabled by default when loaded).
* `QCOM_TFP_TRACE_READBACK=1` — sample a few bound textures via FBO readback.
* `QCOM_TFP_TRACE_SYNC=1` — `glFinish` + short delay before readback.
* `QCOM_TFP_FORCE_FORMAT` — `rgb` or `rgba` (overrides `GLX_TEXTURE_FORMAT_EXT`).
* `QCOM_TFP_FORCE_TARGET` — `2d` or `rect` (overrides `GLX_TEXTURE_TARGET_EXT`).
* `QCOM_TFP_LOG` — log file path for rewrite messages, or `1` for `stderr`
  (default: off). Logs only when an attribute is actually rewritten.
