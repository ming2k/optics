# ADR-0056: Win32 + Cocoa backends and the MoltenVK baseline

- Status: Accepted
- Date: 2026-08-08
- Scope: iris platform backends (Windows, macOS) and the flux device
  requirements they rely on.

## Context

ADR-0044 reserved `src/app_win32.c` / `src/app_cocoa.m` behind
compile-time selection; ADR-0055 gave every backend the same wakeup,
input-mapping and callback contracts. flux requires Vulkan 1.3 with
`dynamicRendering`, `synchronization2`, `timelineSemaphore`,
`bufferDeviceAddress` and full descriptor indexing — a bar that native
Windows drivers meet but that only MoltenVK ≥ 1.3 reaches on macOS.

## Decision

1. **Two new backends implement the platform_internal.h contract**:
   - `src/app_win32.c` (~1300 lines): per-monitor-DPI-v2 window,
     `MsgWaitForMultipleObjectsEx` loop, IMM32 IME (structured so a TSF
     upgrade drops in), clipboard via PostMessage-deferred delivery,
     `vkCreateWin32SurfaceKHR`. Plus `theme_win32.c` (registry +
     `WM_SETTINGCHANGE`) and `file_dialog_win32.c` (`IFileOpenDialog` /
     `IFileSaveDialog`).
   - `src/app_cocoa.m` (~1560 lines): manual `NSApplication` loop with
     deadline waits, `NSTextInputClient` IME, `CAMetalLayer`-backed view
     with `vkCreateMetalSurfaceEXT`, pasteboard, KVO theme watching
     (`theme_cocoa.m`), `NSOpenPanel`/`NSSavePanel`
     (`file_dialog_cocoa.m`).
2. **All three backends run the same frame-pacing algorithm** (active
   ~60 Hz / idle caret ~4 Hz / fully static waits on events, driven by
   `lens_frame_needs_repaint` / `lens_anim_pending`), ported line-by-line
   from `app_wayland.c`.
3. **macOS requires MoltenVK ≥ 1.3.** flux auto-enables
   `VK_KHR_portability_enumeration` (and the instance create flag) and
   `VK_KHR_portability_subset` whenever the driver advertises them —
   no-ops elsewhere. Older MoltenVK releases that only advertise Vulkan
   1.2 are rejected at device selection, deliberately.
4. **IME reduces to the lens_input contract on all three**:
   text-input-v3 / IMM32 / NSTextInputClient each fill
   `preedit_utf8` + caret/clause fields; `ime_delete_before/after` stays
   zero on Win32/Cocoa (no native concept).
5. **a11y ships as the stub on both new platforms** until UIA /
   NSAccessibility bridges are scheduled; the public API is unaffected.
6. Cocoa wakeup uses a posted application-defined `NSEvent` rather than
   `CFRunLoopPerformBlock`: the latter does not make a
   `nextEventMatchingMask:untilDate:` wait return, so it cannot wake a
   fully idle loop (recorded in `app_cocoa.m`'s header).

## Alternatives Considered

- **GLFW/SDL as the desktop backend.** Rejected: ADR-0043 puts the
  platform layer in iris; middleware would still leave IME, a11y, DPI
  and frame pacing to be written natively, while adding a dependency.
- **A Metal RHI for macOS.** Rejected: ADR-0006 (no runtime RHI);
  MoltenVK carries the Vulkan contract.
- **TSF-first IME on Windows.** Deferred, not rejected: IMM32 covers the
  lens_input contract today; the implementation is structured for a TSF
  swap when field data demands it.
- **`VK_MVK_macos_surface`.** Not used: `VK_EXT_metal_surface` is the
  maintained WSI path on MoltenVK.

## Consequences

- Three frame-pacing implementations share one algorithm; changes must
  land in all three files (the algorithm is comment-cross-referenced).
- Windows/macOS verification today: every Win32 TU is compile-gated
  against real MinGW-w64 headers via zig cc in CI
  (`windows-zig-compile-check`); Cocoa is gated by the native `macos-14`
  CI job. Runtime behaviour on real machines follows each file's
  documented verification checklist (IME, Retina/DPI transitions,
  fullscreen, presentation orientation).
- Flux device creation must keep the auto-append behaviour for
  portability extensions; removing it breaks macOS only.

## References

- [ADR-0043](0043-iris-foundations.md),
  [ADR-0044](0044-iris-backend-selection.md),
  [ADR-0055](0055-watch-apis-and-wakeup-seam.md),
  [ADR-0006](0006-no-runtime-rhi.md).
- `libs/iris/src/app_win32.c`, `app_cocoa.m`;
  `docs/dev/cross-platform.md`.
