# Changelog

All notable, user-visible changes to the C libraries in this monorepo
(`libflux`, `libflux-text`, `libflux-scene-graph`, `liblens`, `libiris`,
`libprism`) are documented in this file. The Rust bindings under
`bindings/` carry their own per-crate changelogs.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [semver](https://semver.org/) with the pre-1.0 allowances
documented in [docs/reference/api.md](docs/reference/api.md) — patch
releases are source- and binary-compatible, minor releases may break
either.

## [Unreleased]

## [0.0.25] - 2026-08-22

### Changed — compatibility machinery (policy now enforced)

- **Soname now encodes the compatibility boundary, not the release
  identity.** All six libraries previously carried the full `X.Y.Z` in the
  SONAME (`libflux.so.0.0.24`), so every patch release changed the link
  name — contradicting the policy table's "patch → binary compatible".
  SONAMEs are now `major.minor` (`libflux.so.0.0`) with the full `X.Y.Z`
  kept as the real-file suffix. Patch releases no longer require relinking.
- **`[[deprecated]]` is now applied, not just documented.**
  `flux_canvas_begin` / `flux_canvas_end` / `flux_canvas_end_checked`
  (kept for source compatibility only, superseded by
  `flux_canvas_begin_frame` / `flux_canvas_end_frame_checked`) now carry
  deprecation attributes with replacement guidance, per the two-step
  deprecation policy in `docs/reference/api.md`. A new
  `FLUX_DEPRECATED(msg)` macro handles C23/C++/GNU attribute spellings.
- **`FLUX_NODISCARD` coverage completed.** Every fallible entry point
  returning `flux_result` is now marked, honouring the contract stated at
  the top of `<flux/core.h>`. Previously unmarked:
  `flux_surface_resize`, `flux_frame_collect_timestamps`,
  `flux_canvas_cpu_begin`, `flux_result_string`, and the entire
  `<flux-scene-graph/scene-graph.h>` fallible surface
  (`flux_sg_load_glb`, `flux_sg_scene_set_materials`,
  `flux_sg_load_animation_glb`, `flux_sg_scene_apply_animation`).

### Fixed — Windows / portability

- **`<flux-text/text.h>` exports through its own `FLUX_TEXT_API` macro.**
  It previously reused `FLUX_API` while being built with
  `-DFLUX_TEXT_BUILDING` (not libflux's `FLUX_BUILDING`), so on `_WIN32`
  every exported `flux_text_*` symbol was *defined* under
  `__declspec(dllimport)` — an MSVC error — and
  `flux_text_version_string` would have been imported from `libflux`
  rather than `libflux_text`.
- **`<iris/cursor.h>` is self-contained.** It used `IRIS_API` while
  including only `<stdint.h>`; including it alone did not compile. It now
  includes `<iris/app.h>` like every sibling header.
- **`lens_icon_table` is exported.** The built-in icon table lacked
  `LENS_API`, so it would not link across a Windows DLL boundary.
- **`<lens/export.h>` is the single source of truth for `LENS_API`.**
  `lens.h` and `icon.h` each carried their own copy of the macro; two
  spellings of one export macro drift. Both now include `export.h`,
  which is installed alongside them.
- **`<lens/lens.h>` is self-contained** — it now includes
  `<stdbool.h>`/`<stddef.h>`/`<stdint.h>` directly instead of relying on
  flux headers to provide them transitively.

### Fixed — documentation drift

- `effect.h` no longer says the promote-to-owned helper "may land later";
  `flux_effect_promote` has existed below that comment since it was written.
- `vulkan.h` no longer claims `R32_SFLOAT` is "not currently in enum".
- `docs/reference/api.md` no longer claims `<flux/dmabuf.h>` is gated
  behind the canvas module (untrue since ADR-0052).
- `iris.h` no longer describes theme watching and file dialogs in
  Linux-only terms; both have per-platform implementations (ADR-0056).
- `lens.h` no longer claims text measurement is a monospace stub; the
  text seam calls `flux_text_measure` (ADR-0033).
- Stale "to be added" / "not implemented" comments in
  `platform_internal.h`, `theme_linux.c`, and `libs/iris/meson.build`
  updated to reflect shipped backends.
- ADR-0036's paste-buffer figure (1024) aligned with the implementation
  (`LENSI_PASTE_MAX` = 1 MiB).

### Added — capability & extension surface

- **`flux_paint` now carries the sType/pNext extension chain** (`next`
  field + reserved `FLUX_TYPE_PAINT_EXT_BASE` tag block). Adding a paint
  kind no longer requires restructuring the hottest descriptor in the
  library — previously an ABI break by the project's own compatibility
  table. All existing constructors produce `next = NULL`.
- **`flux_device_get_limits()`** — device limits query (max image
  dimensions, framebuffer bounds, alignment requirements, timestamp
  period/validity, applied frames-in-flight cap). Feature discovery by
  query instead of by failure.
- **`flux_device_supports_image_usage()`** — format/usage support query
  that mirrors `flux_image_create_compute_writable`'s acceptance policy
  exactly (pinned by an integration test that asserts query/create
  agreement).
- **`flux_oneshot_begin` / `flux_oneshot_submit_and_end` /
  `flux_oneshot_run`** — the public one-shot submission surface. The
  effect runtime's header required callers to "supply a one-shot command
  buffer" and to have completed a `vkQueueWaitIdle` first, while flux
  owned the only queues and exposed no way to do either; the machinery
  existed internally and is now published.
- **`flux_pipeline_release_deferred()` /
  `flux_graphics_pipeline_release_deferred()`** — safe-at-any-time
  release for pipelines via the retire queue. The plain `*_release`
  remains destroy-inline (documented more loudly); the identical call
  shape no longer hides opposite semantics — callers who learned
  "release is always safe" from images now have the same guarantee
  available for pipelines.
- **`iris_supports()` / `iris_backend_name()`** (`iris/capability.h`) —
  build-level feature discovery with one documented degradation contract
  per capability. Cross-platform divergence is now observable by
  construction instead of silent.
- **Uniform file-dialog result codes** (`IRIS_PICK_CANCELLED` /
  `IRIS_PICK_UNAVAILABLE` / `IRIS_PICK_TRUNCATED`). The three backends
  previously had three contradictory overflow behaviours (portal:
  indistinguishable from cancel; Win32: silent prefix; Cocoa: outright
  failure) and two `out_bytes_used` accountings. Now: truncation is
  always a visible `IRIS_PICK_TRUNCATED` carrying the full required size
  for retry, accounting excludes the trailing terminator everywhere, and
  `iris_pick_file` ignores `multiple_selection` on every backend.
- **`lens_desc` size guard** + `LENS_DESC_INIT`, and **`lens_theme`
  copies are now actually clamped** to min(caller, library) as ADR-0032
  always claimed (previously a raw full-struct copy — an over-read on
  input and over-write on output for mismatched layouts). Pinned by
  `tests/lens/test_abi_guards.c`.
- **Compile-time ABI drift guards** in `<flux/core.h>`: the
  `flux_struct_type` enum values (including the retired-tag boundary and
  the new paint-extension base) are `static_assert`ed, so a renumbering
  fails the build instead of silently mis-routing chained descriptors.

### Added — Rust bindings (`bindings/iris-rs`)

- **Safe wrappers for the previously unbound 18 of 30 C functions**:
  all window controls (`window_close` … `window_get_geometry`,
  `window_maximize`/`window_unmaximize`), the sole thread-safe entry
  point `post_to_main_thread` (closure-boxing trampoline, `false` on the
  documented no-loop path), `pick_save_path`, and `pick_files` (with
  automatic buffer growth driven by the truncation contract). A Rust
  application can finally close its own window and wake the loop from a
  worker thread.
- **Capability bindings**: `supports(Capability)` + `backend_name()`,
  with tests pinning the per-backend capability table.
- **`PickError`** distinguishes cancelled / unavailable / truncated
  instead of collapsing all three into `None` (the old `pick_file` kept
  its `Option` shape for compatibility; the new dialogs return
  `Result`).
- **`window.h` and `capability.h` added to bindgen's rerun-if-changed
  list** — editing `window.h` previously produced stale generated
  bindings.
- The misleadingly-named `run_with_null_config_returns_error_not_crash`
  smoke test (which tested neither) renamed to what it actually checks.

