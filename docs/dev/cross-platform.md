# Cross-platform support (Windows / macOS)

Optics targets three desktop platforms from one source tree:

| Layer | Linux | Windows | macOS |
|---|---|---|---|
| flux rendering | Vulkan 1.3 | Vulkan 1.3 | Vulkan 1.3 via **MoltenVK ≥ 1.3** |
| flux text fonts | fontconfig | DirectWrite | CoreText |
| iris backend | Wayland (`app_wayland.c`) | Win32 (`app_win32.c`) | Cocoa (`app_cocoa.m`) |
| lens | platform-neutral, unchanged everywhere | | |

Backend selection is compile-time (ADR-0044); `iris_app_run` is the only
entry point and never exposes the backend. Non-Linux builds without a
backend still link via `IRIS_BUILD_NO_BACKEND`.

## Platform requirements

- **All platforms**: a Vulkan 1.3 implementation with `dynamicRendering`,
  `synchronization2`, `timelineSemaphore`, `bufferDeviceAddress`, and full
  descriptor indexing (these are hard requirements, checked at device
  creation).
- **Windows**: clang-cl ≥ 19, MinGW GCC ≥ 15, or any compiler with C23
  `#embed` — otherwise meson falls back to generated shader headers
  automatically (`-Dshader_embed=auto`). MSVC `cl` is *not* supported (no
  `#embed`, no C23 atomics). The Vulkan SDK provides headers, the loader,
  and `glslangValidator`.
- **macOS**: MoltenVK ≥ 1.3 (older releases only advertise Vulkan 1.2 and
  are rejected at device selection). flux auto-enables
  `VK_KHR_portability_enumeration` and `VK_KHR_portability_subset` when the
  driver advertises them. Homebrew LLVM or Xcode 16+ clang; without
  `#embed` support the generated-header fallback is used. Cocoa backend
  requires Objective-C (`add_languages('objc')` is handled inside
  `libs/iris/meson.build`).
- **Linux**: unchanged (gcc ≥ 15 or clang ≥ 19, wayland-client, xkbcommon).

The dma-buf subsystem (`flux/dmabuf.h`, zero-copy import/export) is
Linux-only. On Windows/macOS the same entry points exist but return
`FLUX_ERROR_UNSUPPORTED`; the code is compiled from a stub translation
unit selected at configure time.

## Verifying changes without Windows/macOS hardware

`tools/zig-win32-check.sh` compile-checks any C translation unit against
MinGW-w64 headers using `zig cc -target x86_64-windows-gnu` (C23, including
`#embed`). It isolates the platform-neutral Vulkan headers so host glibc
headers never leak into the check:

```bash
tools/zig-win32-check.sh -Ilibs/flux/include -Ilibs/lens/include \
    -Ilibs/iris/include libs/iris/src/app_win32.c
```

Every `*_win32.c` / `*_directwrite.c` file is checked this way in CI
(`.github/workflows/ci-cross.yml`, job `windows-zig-compile-check`); the
file list is discovered, so new platform files join the gate automatically.

macOS Objective-C sources cannot be compile-checked on Linux (no Apple
SDK); they are gated by the native `macos-14` CI job instead.

## Consistency invariants

These are the rules that keep behaviour identical across platforms; any
platform code must preserve them:

1. **Logical pixels everywhere.** Input coordinates, `lens_input`
   geometry, and layout use logical pixels; the host reports the
   device-pixel ratio via `lens_desc.scale` / `lens_set_scale`
   (Wayland fractional-scale, Win32 per-monitor DPI, Cocoa
   `backingScaleFactor`).
2. **CPU canvas as the rendering oracle.** `canvas_cpu` is a pixel-level
   reimplementation of the GPU canvas pipeline (ADR-0019) and runs
   identically on every OS. Rendering conformance tests compare GPU
   output against it rather than against per-platform goldens.
3. **Damage-driven frame pacing.** Hosts paint when
   `lens_frame_needs_repaint` / `lens_anim_pending` say so, not on a
   fixed timer; the platform event loop (poll / MsgWait / CFRunLoop)
   supplies the deadline sleep.
4. **Async paste.** `lens_clipboard.request_text` is always answered
   later via `lens_paste` — never synchronously — matching the strictest
   platform (Wayland) so all three behave alike.
5. **IME contract.** Composition state travels in
   `lens_input.preedit_utf8` + cursor/clause fields; the candidate window
   is positioned from `lens_caret_rect`; `ime_delete_before/after` covers
   `delete_surrounding_text`. Platform IMEs (text-input-v3, IMM/TSF,
   NSTextInputClient) must reduce to exactly these fields.
6. **Thread-to-main delivery.** Platform watchers (theme changes, a11y
   pumps) never touch lens/flux state directly; they post to the backend
   event loop through the internal wakeup seam and run on the main
   thread.
