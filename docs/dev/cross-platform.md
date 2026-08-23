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
   platform (Wayland) so all three behave alike. On Wayland the read runs
   on a detached helper thread with a hard 5 s deadline (a stuck or
   malicious selection owner cannot hang the UI) and completion is
   delivered through `iris_post_to_main_thread`; the DND drop read and
   the middle-click primary-selection read use the same helper thread +
   deadline pattern (2 s for drops). Win32 posts
   `WM_IRIS_PASTE_DELIVER`, Cocoa an ApplicationDefined event.
5. **IME contract.** Composition state travels in
   `lens_input.preedit_utf8` + cursor/clause fields; the candidate window
   is positioned from `lens_caret_rect`; `ime_delete_before/after` covers
   `delete_surrounding_text`. Platform IMEs (text-input-v3, IMM/TSF,
   NSTextInputClient) must reduce to exactly these fields. Strings clipped
   into the fixed-size input buffers (`text_utf8[256]`,
   `preedit_utf8[LENS_PREEDIT_MAX=256]`) go through the shared
   boundary-aware helpers (`src/platform_text.h`) — never a raw byte cap
   that could split a multi-byte UTF-8 sequence.
6. **Accessibility-preference contract (ADR-0075).** Every backend
   queries the OS accessibility preference set at startup
   (`iris_a11y_prefs_query`) and applies it to lens unconditionally —
   `lens_set_reduced_motion`, `lens_set_text_scale`, and the
   high-contrast theme mutation — then watches for changes through its
   backend slot with main-thread delivery. A platform with no readable
   source for a field reports the library default (false / false / 1.0);
   it never invents a value and never fails.
6. **Thread-to-main delivery.** Platform watchers (theme changes, a11y
   pumps) never touch lens/flux state directly; they post to the backend
   event loop through the wakeup seam and run on the main thread. Hosts
   get the same capability through the public
   `iris_post_to_main_thread(fn, user)` (`iris/app.h`): thread-safe,
   callable from any thread, FIFO on the main thread, dropped when no loop
   is running.
7. **Keyboard contract.** Every backend reports both press and release
   edges for each mappable key; letters/digits are normalised to the
   unshifted ASCII code (`'a'`, never `'A'`) with shift travelling in
   `lens_input.mods` only; `lens_key_event.repeat` marks synthesised
   auto-repeat presses (Wayland repeats client-side from the compositor's
   `repeat_info`, Win32 and Cocoa report OS auto-repeat) and lens treats a
   repeat exactly like a press. The authoritative statement lives in
   `src/platform_internal.h`.

## Current platform feature gaps

Not defects — features that exist on one platform and are unscheduled
elsewhere:

- **Drag-and-drop** into the window is implemented on the Wayland backend
  only (real MIME negotiation over `text/uri-list` / `text/plain`, a
  bounded read on a detached helper thread, delivery through `lens_paste`).
  Win32 and Cocoa have no drop target yet.
- **Primary selection** (middle-click paste) is Wayland-only: copies are
  mirrored onto `zwp_primary_selection_unstable_v1` and a middle-button
  press pastes it (async, same channel as clipboard paste). Win32 and
  Cocoa have no native equivalent.
- **Live theme watching** on Linux requires libsystemd at build time
  (portal watcher thread); without it the startup query still works and
  `iris_color_scheme_watch` reports unavailable. The same applies to live
  accessibility-preference watching (`iris_a11y_prefs_watch`); the
  startup query itself shells gsettings/kreadconfig and needs no
  D-Bus client library.
- **Accessibility preference coverage** (ADR-0075): the macOS
  `text_scale` field reports 1.0 — macOS has no global text-size knob
  (Dynamic Type is per-app); "Large text" changes the display resolution,
  which arrives as a scale change instead. The Win32 and macOS a11y-pref
  watchers share their platform's verification ceiling
  (`theme_win32.c` / `theme_cocoa.m` caveats: compile-checked
  cross-platform, not yet exercised on real hardware).
- **File dialogs** on Linux keep the app responsive while the modal
  picker is open (the wait loop pumps the Wayland display fd and the
  AT-SPI bus fd alongside the portal bus) and pass the window's
  xdg-foreign handle as `parent_window` when the compositor exports one;
  without xdg-foreign-unstable-v2 the picker opens unparented.
