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

### Added — lens (accessibility)

- **`lens_set_text_scale` / `lens_text_scale` / `lens_desc.text_scale`**
  (ADR-0075): the OS "make text bigger" preference as a pure multiplier
  on every font-size token. Applied at the single font-size funnel
  (`lensi_style_font_size`) so measurement, intrinsic widget heights,
  caret metrics, and paint scale together — text grows, boxes follow,
  nothing clips. Orthogonal to the DPI scale (raster density untouched);
  explicit point sizes scale too; pure-px geometry (padding, strokes)
  deliberately does not. Non-finite and non-positive factors are ignored.
  Rust binding: `Ui::set_text_scale` / `Ui::text_scale`.

### Added — iris (accessibility)

- **`<iris/a11y_prefs.h>`** (ADR-0075): `iris_a11y_prefs_query()` and
  `iris_a11y_prefs_watch()` — the OS accessibility preference set
  (reduced motion, high contrast, text scale) on all three backends.
  Linux reads gsettings/kreadconfig at startup and rides the shared
  portal `SettingChanged` pump for live changes; Windows reads
  `SPI_GETCLIENTAREAANIMATION` / `SPI_GETHIGHCONTRAST` /
  `SPI_GETNONCLIENTMETRICS` with `WM_SETTINGCHANGE` delivery; macOS reads
  `NSWorkspace` accessibility display options with the corresponding
  notification. Never fails — unreadable fields report the library
  defaults.
- **All backends now apply the preferences to lens unconditionally at
  startup and on every change** (backend watcher slot; the host's public
  slot is untouched): `lens_set_reduced_motion`, `lens_set_text_scale`,
  and a raised-contrast theme mutation for high contrast. This wires the
  previously dead `lens_set_reduced_motion` switch to the OS preference
  that was meant to drive it.

### Changed — iris (a11y bridge)

- AT-SPI semantic-tree cap raised 256 → 1024 nodes; the walk now records
  solved bounds and emits `BoundsChanged` events on geometry changes, so
  magnifier tracking survives scrolling and relayout.

## [0.0.26] - 2026-08-23

### Added — flux (effect module)

- **`flux_shadow_filter`: the realtime frame-slot shadow path** (ADR-0074
  follow-through). The exact `flux_effect_shadow` leases intermediates
  from the per-device transient pool per call — a compositor driving it
  every frame would hold one lease per dispatch until
  `flux_effect_reset`. The filter instead owns its ping/pong/output
  images per frame-in-flight slot (the `flux_blur_filter` ownership
  model): no pool growth, no lease accumulation, no device-wide wait,
  and blur/offset/tint/alpha vary per call without pipeline churn.
  Accepts the same descriptor (`next` must be NULL); requires a
  recording frame at a pass boundary; the output is borrowed from the
  filter's slot. Parameter validation now happens before the frame is
  dereferenced, so validation calls against a not-yet-started frame
  return `FLUX_ERROR_INVALID_ARGUMENT` instead of undefined behaviour.
  Rust binding: `ShadowFilter` + `ShadowedImage` (with `draw`) in
  `flux-rs`, and `EffectImage` gained the missing `draw()`. GPU
  coverage: five-slot cycling in `test_canvas_target.c` plus validation
  cases in `test_effect_shadow.c` (43 checks total).

- **`flux_effect_shadow`: drop shadows as a first-class image operator**
  (ADR-0074's first intake). A shape mask in (the input's alpha channel),
  a premultiplied tinted shadow out: two separable Gaussian passes over
  the mask (the same kernel as `flux_effect_blur`, `blur` clamped to
  `[0, FLUX_EFFECT_SHADOW_BLUR_MAX]`), then a combine pass sampling the
  blurred mask at `p - offset`, applying tint, opacity, and
  premultiplication in one shader (`mode` 0/1/2). Geometry-neutral per
  ADR-0047: shape geometry arrives only through the mask; the canvas
  already draws rounded rects and prism owns its own SDF path.
  Animation-safe per ADR-0074: fixed dispatch shape per extent — blur,
  offset, tint, and alpha vary per frame without pipeline churn, leasing
  intermediates through the existing transient pool with reset epochs.
  `FLUX_TYPE_EFFECT_SHADOW_DESC = 40` appended after the reserved paint
  block (35–39) with a new ABI drift guard. Rust binding:
  `ShadowParams` + `effect_shadow` + `EffectImage` (with `promote`) in
  `flux-rs`. GPU coverage in `tests/flux/integration/test_effect_shadow.c`
  (35 checks: validation, identity copy, exact 6 px offset, analytic
  σ=4 Gaussian centre, alpha 0, lease reuse).

### Fixed — flux (canvas hot path)

- **Per-submit canvas pipeline lookups no longer take the device-wide
  lock.** `vk_bind_program` runs for every single canvas submit; routing
  each one through `get_canvas_pipeline_id` took the device-wide
  canvas-state mutex twice (canvas_state_get_or_init) plus an 8-slot
  linear probe, even on a cache hit. Point-field style workloads issue
  thousands of submits per frame (a 48x32 dot field is 1536), and that
  lock traffic was measurable as the top CPU cost of a frame. The
  canvas now memoises the last (program, blend) → (layout, pipeline)
  resolution for the current pass — reset at pass begin and at the
  output blit, since the pass-fixed key parts (linear colour format,
  sample count, stencil availability) are captured by the memo's scope.
  The same pass now resolves repeats without touching the pipeline
  cache lock.
- **`flux_device_default_sampler_handle` is now a lock-free read.** The
  handle is immutable once published (the single writer never changes it
  afterwards — teardown destroys the device), so it is stored as an
  atomic: readers load-acquire and skip the bindless lock entirely once
  populated; the single writer stores-release inside the lock. Every
  glyph run and image draw lands here, so the uncontended atomic load
  replaces the bindless lock round-trip that used to dominate
  text-heavy frames. The handle slot is now explicitly initialised to
  the `FLUX_BINDLESS_INVALID` sentinel at device creation (the previous
  calloc zero was, in principle, a valid handle value).
- GPU coverage: `test_canvas_render.c` gains the point-field batching
  case — 256 small dots in one draw (the wavora visual-stage workload)
  — pinning that a 1536-dot field collapses to a single `vkCmdDraw`.

### Fixed — iris (Wayland)

- **Continuous (touchpad) scroll now reaches `lens_input.scroll_pixels_*` on
  Wayland.** The backend folded *all* `wl_pointer.axis` / `axis_value120`
  deltas — notched wheels *and* two-finger touchpad scrolls — into the
  wheel-step channel (`scroll_x/y`) and left the continuous pixel channel
  (`scroll_pixels_x/y`, ADR-0036) permanently zero. Widgets that read both
  channels (scroll views, sliders) still worked, but any host gesture keyed
  to the pixel channel alone (e.g. aphrodite's cursor-anchored wheel zoom)
  was dead code on the only platform it targeted. `axis_source` is now
  recorded per frame group (it precedes its axis events) and
  FINGER/CONTINUOUS sources accumulate — inverted, like the wheel channel —
  into `scroll_pixels_*`; `drain_input` forwards and clears them. The
  routing policy lives in `platform_input.h`
  (`iris_scroll_source_is_continuous`) so every backend decides it the same
  way, with a headless pin in `tests/iris/test_platform_input.c`.

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

