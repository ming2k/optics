# Changelog

All notable, user-visible changes to the C libraries in this monorepo
(`libflux`, `libflux-text`, `libflux-scene-graph`, `liblens`, `libiris`,
`libprism`, `libanim`) are documented in this file. The Rust bindings under
`bindings/` carry their own per-crate changelogs.

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning: [semver](https://semver.org/) with the pre-1.0 allowances
documented in [docs/reference/api.md](docs/reference/api.md) — patch
releases are source- and binary-compatible, minor releases may break
either.

## [Unreleased]

## [0.0.36] - 2026-09-05

### Fixed

- **prism**: Decouple backdrop layer glass sampling with a dedicated scratch buffer to eliminate in-place read/write race hazards, and skip the frost pass when frost count is zero.

## [0.0.35] - 2026-09-05

### Fixed

- **prism**: Clamp liquid glass rim width on small bodies to maintain flat centers, and suppress excessive dark backdrop tinting under smoke plates.

## [0.0.34] - 2026-09-05

### Added

- **lens**: Added `Frame::textfield_password` safe Rust binding for masked password entry.

## [0.0.33] - 2026-09-05

### Added

- **lens**: Introduce patterns layer, declarative view tree, and fine-grained reactivity (`SplitOpts`, `TabStrip`, `SegmentedControl`, `virtual_grid_calc`).
- **iris**: Introduce `iris_window_start_move` for interactive CSD window dragging over Wayland/CSD.
- **iris/lens**: Cross-platform drag and drop subsystem with native OLE/COM and Cocoa backends (ADR-0086), exposing safe DnD API and `Capability::DragSource`.

### Fixed

- **lens**: Optical baseline centering, full caret span, concentric pill padding, text-input-v3 IME preedit, and SVG arc tangent derivative signs.

## [0.0.32] - 2026-08-30

### Fixed

- **lens**: Ensure box model orthogonality and layout constraints consistency across Rust bindings.

## [0.0.31] - 2026-08-29

### Changed — Box Model Orthogonality, Legacy Layout Elimination, and Native C23 SVG Engine

- **lens**:
  - **ADR-0085**: Streamlined `lens_layout_opts` by completely eliminating redundant top-level `min_width`, `max_width`, `min_height`, and `max_height` fields; all geometry, constraints, identity, and per-call styles are exclusively sourced from `opts.box`.
  - **C23 Compile-Time Contracts**: Enforced static assertions (`static_assert(offsetof(..., box) == 0)`) across all 14 component and container descriptor structs to guarantee structural subtyping in memory.
  - **Native C23 SVG Icon Engine**: Completely removed third-party vendored dependencies (`nanosvg`) from `libs/lens/src/vendor/`. Implemented a focused, lightweight native C23 SVG parser (`icon_svg.c` / `icon_svg.h`) supporting W3C path commands, curves, elliptical arcs, shapes, style inheritance, native `currentColor` mapping, and automatic color run merging.
  - **C23 Standard Attributes**: Annotated query and life-cycle APIs with `LENS_NODISCARD` (`[[nodiscard]]`).

- **bindings**:
  - `lens-rs`: Aligned `LayoutOpts` raw FFI translation with the unified `lens_box` geometry model.


### Changed — Minimal Orthogonal Component Architecture & Full C23 Standard Baseline

A comprehensive, zero-baggage architectural overhaul across `flux`, `lens`, and `iris` implementing minimal orthogonal primitives, the Single Opts Descriptor paradigm, and full C23 modern language standards (ADR-0082, ADR-0083, ADR-0084).

- **lens**:
  - `lens_widget_kind` pruned from 22 kinds to 9 strictly orthogonal primitives: `LABEL`, `ICON`, `IMAGE`, `SEPARATOR`, `BUTTON`, `CHECKBOX`, `SELECTABLE`, `SLIDER`, `TEXTEDIT`.
  - Merged `textfield` and `textarea` into a single, unified `lens_textedit` widget with multiline support.
  - Merged `switch` and `radio` into `lens_checkbox` with `.appearance` styling (`BOX`, `SWITCH`, `RADIO`).
  - Merged `primary`, `subtle`, `link`, `icon_button`, `image_button` into `lens_button` with `.variant` styling.
  - Eliminated dual-tier APIs (`_ex`) and parameter-derived suffixes (`_wrapped`, `_vertical`, etc.) across all components; every component now exports a single canonical `const lens_<widget>_opts *opts` entry point returning `lens_response`.
  - Removed compound widgets (`table`, `tabs`, `menu`, `menubar`, `dropdown`, `collapsing`, `tree`, `split`, `modal`) from the core micro-kernel into composable userland patterns.
  - Established a strict 4-layer physical separation: Overlay (`Place`), Viewport (`Scroll`), Layout (`Row`/`Column`/`Grid`), and Data Atoms.

- **flux**:
  - Unified 15+ specialized draw functions into a single orthogonal `flux_canvas_draw(canvas, const flux_shape *shape, const flux_paint *paint)` drawing primitive.
  - Introduced `flux_shape` covering `RECT`, `RRECT`, `CIRCLE`, `LINE`, `PATH`, `IMAGE`, and `GLYPHS`.

- **iris**:
  - Implemented the tripartite architecture: Physical Window/Lifecycle $\perp$ Pure Event Pump $\perp$ Modular Host Service Bridges.
  - Unified application launch into `[[nodiscard]] IRIS_API int iris_app_run(const iris_app_opts *opts)`.

- **governance**:
  - Upgraded meta-governance protocol in `docs-governance` to support top-level `docs/governance/`.
  - Established `docs/governance/index.md`, `docs/governance/api-design-guidelines.md`, and domain-specific governance specifications.
  - Published `ADR-0082`, `ADR-0083`, `ADR-0084`.

- **rust bindings**:
  - Synchronized `bindings/lens-rs` with the new orthogonal C23 single-descriptor API and removed obsolete wrapper glue.

### Changed — public API surface cleanup (all libraries)

Dead and superseded public symbols were removed outright (pre-1.0, no
remaining callers):

- **flux**: removed `flux_canvas_begin` / `flux_canvas_end` /
  `flux_canvas_end_checked` (GPU-spelled compatibility wrappers; use the
  backend-agnostic `flux_canvas_begin_frame` / `flux_canvas_end_frame` /
  `flux_canvas_end_frame_checked`) and the unused
  `flux_canvas_draw_image_coverage` / `flux_canvas_draw_image_coverage_sub`
  (the glyph-run atlas path is the supported R8-coverage route).
- **lens**: `lens_label_compact_ex` gained the `weight` parameter (it
  subsumes the zero-caller `lens_label_compact_ex2`); the zero-caller
  `lens_label_wrapped_ex` was removed (`lens_label_wrapped` with the
  cascade-resolved size remains). `lens_icon_table` left the installed
  headers (it is internal; `lens_icon_info()` is the public read access).
  `lens_version_string()` now derives from the `LENS_VERSION_*` macros
  instead of a hardcoded literal.
- **iris**: removed the zero-caller `iris_a11y_unique_name()`.
- **prism**: gained `PRISM_VERSION_*` macros and `prism_version_string()`
  (macro-derived), closing the versioning gap against lens/iris/flux.
- **deprecation machinery**: `FLUX_DEPRECATED` moved from canvas.h to
  core.h (single definition); `LENS_DEPRECATED`, `IRIS_DEPRECATED`,
  `PRISM_DEPRECATED` added to their export headers. Docs
  (`docs/reference/api.md`) now state the removal policy.

### Changed — Rust bindings

- The `-sys` crates no longer duplicate foreign types: `lens-sys`,
  `iris-sys`, `flux-text-sys`, and `flux-scene-graph-sys` blocklist the
  seam types they borrow and re-export them from `flux-sys`/`lens-sys`
  (the pattern prism-sys already used), so one Rust definition of each
  handle exists per process.
- `flux::Canvas` now wraps `end_frame` / `end_frame_checked` (the
  deprecated `begin`/`end`/`end_checked` wrappers are gone) and gains
  `new_cpu_aa`. `flux::Format` is an idiomatic enum instead of a raw
  bindgen re-export. `flux_text::Error` / `flux_scene_graph::Error` are
  re-exports of `flux::Error` (one error type for one `flux_result`).
- `iris::PaintHost` / `StartHost` expose typed `flux::Canvas` /
  `flux::Device` / `Ui` borrows instead of `*mut c_void`. `lens::Ui`
  gains `borrow_raw`. `prism::version()` added.
- Every `-sys` build.rs now enforces `atleast_version("0.0.29")`, and the
  stale `>= 0.1.0` / `>= 0.0.13` constraint comments were corrected.
- `flux-composition-graph` and `flux-text-layout` moved from
  `bindings/flux-rs/crates/` to the top-level `crates/` workspace: they
  are pure-Rust layers above the bindings, not bindings themselves.

### Added — Rust bindings (completeness round)

- `flux::version()` / `version_number()` / `version_check()` — flux was
  the only library whose Rust wrapper lacked the version trio lens/iris/
  prism already had.
- prism crate rustdoc coverage completed (19 pub fns, was 2 documented);
  flux/iris gap items documented; all `cargo doc` warnings across the
  four workspaces are now zero (fixed stale links left over from the
  begin/end removal).

### Changed — documentation maintenance

- `docs/reference/symbols.md` is now **generated** from the installed
  headers by `tools/gen_symbols.py`; a `symbols.md freshness` CI step
  fails on drift. It had rotted as hand-maintained lists do (removed
  symbols lingered, new symbols missing). The maintenance contract is
  documented in `docs/dev/documentation/common-docs.md`: regenerate in
  the same commit that changes a public signature; an empty Description
  cell means the header lacks a doc comment — fix the header, not the
  table.

### Added — iris (all backends)

- **`iris_request_frame_skip_render()` — zero-render skip that keeps the
  active cadence.** Hosts streaming content at a media cadence (a 30 fps
  visualizer on a 60/144 Hz display) had no way to express "skip rendering
  this frame but keep scheduling at the active rate":
  `iris_paint_mark_static()` decays the pacing toward the ~4 Hz idle tick,
  and omitting `iris_request_animation_frame` tears the stream apart. The
  new per-frame declaration suppresses only this frame's begin → clear →
  paint → present; an accompanying animation request re-arms the active
  deadline. Implemented identically across the Wayland, Win32, and Cocoa
  backends: input, resize/scale changes, lens chrome damage, and
  not-yet-presented surfaces always force a real paint regardless of the
  declaration. Rust binding: `iris::request_frame_skip_render()`.

### Fixed — flux resource lifetime and release validation

- Sampled images are retained until the recording frame slot is recycled,
  preventing glyph-atlas replacement from releasing an image still referenced
  by submitted GPU work.
- The deferred-resource hard limit now performs a true idle drain even when
  retire records are deliberately tagged one serial beyond the last submitted
  batch, keeping the retire queue bounded without relying on incidental upload
  submissions.
- The symbol-table generator once again parses adjacent trailing comments, and
  the repository format gate is clean across the C headers, sources, tests, and
  Rust bindings.
- GPU lifetime tests now isolate frame-slot retains and serial-watermark
  behaviour, while the backdrop probe tolerates the expected Gaussian-tail
  variance between Lavapipe and hardware drivers.

## [0.0.28] - 2026-08-25

### Added — prism (material library, ADR-0079)

- **`prism_backdrop_layer`** and shader **`backdrop_frost.comp`**: layered
  backdrop compositor that writes a frosted sheet and evaluates analytic
  liquid-glass bodies against the frosted layer in a single compute dispatch.
  The glass refracts the frost underneath rather than bypassing it into the
  sharp background.
- **`prism_frosted`** and **`prism_acrylic`**: standard standalone material
  implementations with uniform structures and dedicated shaders
  (`frosted.comp`, `acrylic.comp`).
- Rust bindings for backdrop layer, frosted, and acrylic materials in `prism`.

### Fixed — lens (widgets)

- **Table row click coordinate when scrolled.** `lens_table` row hit testing
  now correctly computes the body-relative cursor position before applying
  scroll offset, ensuring rows can be accurately clicked after scrolling.

## [0.0.27] - 2026-08-24

### Fixed — flux-text (glyph atlas)

- **`atlas_clear` no longer leaks page 0's image.** The retire loop
  guarded its release with `p > 0`, but page 0's image is recreated
  immediately after the loop — its old image is exactly as retired as
  the auxiliary pages'. Every clear leaked one 16 MiB dedicated
  `VkDeviceMemory` that never reached the retire queue, so long sessions
  with recurring glyph churn (browsers, CJK input) grew RSS without
  bound. Integration test `text_atlas_leak` forces repeated clears with
  distinct glyph working sets on a real GPU canvas and asserts live
  allocations return to baseline once the retire queue drains.

### Added — anim (new sibling library, ADR-0077)

- **`libanim`** (`-Danim=true`, on by default): the shared motion vocabulary
  as pure math on caller-owned state — closed-form analytic spring
  (non-divergent by construction for any accepted `dt`), exponential
  approach/decay, the standard easing curves, a dead-band + dwell
  **hysteresis latch** for binary decisions fed by noisy measurements, and a
  **motion-adaptive smoother** for temporally lagged continuous signals. No
  clock, no timeline, no allocation; `dt` is a parameter clamped once at the
  boundary to `[0, 1/30]` s, and `reduced_motion` resolves every primitive in
  one step. Property tests pin boundedness under adversarial `dt`, energy
  monotonicity, and endpoint/clamp behaviour (`tests/anim/test_anim.c`).
  Consumers stop copying integrators out of the showcase recipes.

### Added — lens (ghost replay, ADR-0078)

- **`lens_set_ghost(ui, subtree_root, alpha)`**: the leave-animation render
  surface ADR-0038 deferred. Pinning a live subtree arms capture; at its last
  live `lens_end` the subtree's draw commands and geometry are deep-copied
  out of the per-frame arena, and each subsequent `lens_set_ghost` call
  repaints the frozen snapshot at `alpha` through the same command emitter
  and ADR-0068 colour bake the live tree uses (byte-identical to fading the
  same subtree with `lens_set_opacity` — pinned by test). Hosts delete their
  keep-building-dead-content scaffolding: an exit animation is now
  `lens_set_ghost(ui, id, ease(1 - t))`. Snapshots are bounded (16
  concurrent, 64-frame unrefreshed lifetime), never hit-test/focus/a11y,
  and never create canvas display-list records.
- **`LENS_SKIN_SCRATCH_FLOATS`**: the inline scratch size is now a named
  public constant; the header documents the graduation path
  (`lens_skin_scratch` → `lens_node_state`) instead of leaving "more than
  one spring" callers to invent an external hashtable.

### Docs

- `docs/reference/prism.md`: the backdrop-stats caller example now shows the
  correct de-jitter consumers for the 3-frame-stale read — a hysteresis
  latch (dead band + dwell) for the binary `plate_polarity` and a
  motion-adaptive smoother for `backdrop_energy` — replacing the bare
  threshold that oscillated at every bright/dark boundary crossing.


### Added — flux (image upload)

- **`flux_image_update_region_strided`**: the update-region upload for
  source buffers whose rows are wider than the uploaded region — row `i`
  starts at `i * row_bytes`. The same validation contract as the packed
  entry point, with the minimum size derived from the stride
  (`(height - 1) * row_bytes + width * bpp`; larger buffers accepted).
  Rows are repacked internally into the tightly packed staging copy, so
  callers no longer upload row-by-row just to express a stride.
- **`flux_image_update_region_premultiply`**: takes straight
  (non-premultiplied) RGBA8 and premultiplies during the upload using the
  exact integer math of `flux_color_rgba_premul` — `(c * a + 127) / 255`
  with the `a == 255` / `a == 0` fast paths — so uploaded texels are
  bit-identical to that function's output. Also accepts a row stride.
  This closes the seam where every canvas-fed client had to replicate the
  premultiply convention client-side (and pin it with parity tests):
  straight-alpha sources now name their semantics at the upload call.
  Integration test `image_update_strided` verifies both: stride repack
  offsets and premultiply parity against `flux_color_rgba_premul` over
  all alpha buckets, read back through an offscreen NEAREST blit.

### Added — lens (icons)

- **`lens_icon_run` / `lens_icon_desc.runs`** (ADR-0076): per-shape paint
  runs for runtime-registered SVG icons. `lens_icon_register_svg` now
  records one run per source shape with its explicit fill/stroke colour
  (straight 0xRRGGBBAA; gradients degrade to the first stop) and mode,
  and replay paints one flux paint per run — runtime icons can finally
  be multicoloured or solid-filled instead of theme-stroke-only.
  Shapes painted `currentColor` (or with no explicit colour) record
  `color == 0`, "follow the theme"; an icon whose every shape is
  theme-coloured collapses to `runs == NULL` and replays byte-identically
  to a built-in. Built-in icons are unchanged (`runs == NULL` everywhere).
  Test: `tests/lens/test_icon_runtime_runs.c`.

### Added — iris (input)

- **`iris_scroll_accum`** (platform_input.h): the shared, sign-pinned
  scroll accumulator both channels flow through. Documents and tests the
  direction contract — platform events arrive positive = physical
  wheel-down/finger-down, lens consumes the opposite on BOTH channels —
  and performs that single inversion at the platform boundary. The
  Wayland backend now routes through it (one implementation instead of a
  private copy), and `test_platform_input.c` pins the signs for
  vertical/horizontal × step/pixel × up/down.

### Fixed — bindings (all `-rs` workspaces)

- **rpath metadata is now published unconditionally and filtered**: every
  `-sys` build script (flux, flux-text, flux-scene-graph, lens, iris,
  prism) publishes its link dirs as `cargo:rpaths` in installed mode too
  — a `meson install` into a custom prefix needs an rpath exactly like a
  build tree — and system libdirs (`/usr/lib*`, `/lib*`) are filtered out
  so they no longer add DT_RPATH noise. Previously flux-sys/lens-sys
  published an empty list outside dev mode and lens-sys published nothing
  at all, so downstream test binaries silently bound the *installed*
  (stale) libraries.
- **`flux` and `lens` re-publish their rpaths through their own `links`
  metadata** (`flux_rs` / `lens_rs`), mirroring what `iris` already did:
  a downstream that depends on `flux` alone now gets the full transitive
  rpath list via `DEP_FLUX_RS_RPATHS` (same for `DEP_LENS_RS_RPATHS`)
  and can delete its hand-rolled relay build scripts.
- **`OPTICS_BUILD_DIR` / `OPTICS_SOURCE_DIR`**: one variable pair points
  every `-sys` crate at the monorepo for out-of-tree consumers (git
  dependencies, registry builds) whose upward auto-discovery cannot find
  a checkout they are not inside of. The per-library `FLUX_/LENS_/IRIS_`
  variables keep working and still win.

### Added — iris-rs / flux-rs (Rust bindings)

- **`flux::Canvas::draw_image_sampled` / `draw_image_sampled_with_paint`**:
  the NEAREST-sampler blit path finally has a safe wrapper (previously
  C-only — pixel-art clients had to drop to `flux-sys` and hand-build
  `flux_rect`s). `Image::update_region_strided` and
  `Image::update_region_premultiply` wrap the new C entry points.
- **`iris::PaintHost::flux_canvas` / `flux_device`**: typed, borrowed
  `flux::Canvas` / `flux::Device` handles straight from the paint
  callback — no more `*mut c_void` casting at every call site (the C
  callback was always typed; only the Rust seam erased it).
- **`iris::FileDialog`** builder with `FileFilter` (name + glob), title,
  initial folder and multi-select — exposing `iris_file_dialog_opts`
  filters that all three C backends implement but no safe API reached.
  `PickError::InvalidUtf8` now distinguishes backend UTF-8 defects from
  "unavailable" instead of folding them together.

### Changed — docs

- The keyboard contract (`platform_internal.h`, `lens.h`) now states the
  printable-ASCII key-event guarantee explicitly: every printable ASCII
  codepoint (0x20–0x7E, unshifted) arrives as a key event on all three
  backends; non-ASCII printable input is text-only. Downstream shortcut
  documentation had drifted into believing NO printable key produced key
  events.

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
