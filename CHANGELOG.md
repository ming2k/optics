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

### Changed

- **iris**: Presentation is paced by the display instead of by a client-side timer across all platforms (ADR-0122). The frame loop no longer sleeps out a hardcoded ~60 Hz period and re-derives its next wake-up from the last *paint*; it subscribes to the compositor's `wl_surface.frame` grant on Wayland and dynamic display refresh queries across Windows (`EnumDisplaySettings`) and macOS (`CGDisplayModeGetRefreshRate`). On a 3072×1920 @ 120 Hz panel this is the difference between 60 fps and the panel's actual 120 fps.
- **iris**: Incremental damage presentation (ADR-0123). Evaluates `lens_damage_rect(ui)` and constrains the Vulkan dynamic rendering `renderArea` (`render_offset_x/y`, `render_width/height`) strictly to dirty bounding boxes. Attached to Wayland via `wl_surface_damage_buffer` and wired into Win32 and Cocoa, eliminating full-screen clear-and-draw overhead on localized UI updates.
- **iris**: `iris_request_frame_skip_render()` no longer turns a zero-render skip into a busy-spin. The schedule can only move *forward*, and is never recomputed from a render timestamp, so the past-deadline `poll(0)` loop that consumed a full CPU core is structurally unreachable across Wayland, Win32, and Cocoa. A media host declaring one paint in four now runs at the display rate for ~3.5 % of one core.
- **iris**: Animations are evaluated against the compositor's frame timestamp rather than the wall clock at build time, so an eased value lands on the frame the display actually shows.

### Added

- **lens**: `lens_damage_rect(ui)` — computes the tight bounding box of visual damage across dirty base/placed subtrees, carets, and tooltips in logical UI units, clipped to the layout viewport (ADR-0123).
- **iris**: `libs/iris/src/display_pacer.{h,c}` — the platform-neutral frame-cadence state machine (ADR-0122). Pure: no clock, no platform headers, no allocation. Each backend feeds it its own VSync source and it answers paint / skip / quiesce plus the poll budget, with a hard `>= 1 ns` budget postcondition while non-quiescent.
- **iris**: Hardware dual-plane subsurface composition on Wayland (`wl_subcompositor` + `wl_subsurface` with premultiplied alpha), physically isolating dynamic Host Canvas (`cfg->paint`) from static Lens UI chrome, delegating blending directly to display controller hardware planes (ADR-0123).
- **iris**: `libs/iris/src/frame_mailbox.{h,c}` — lockless triple-buffered snapshot handoff channel (`iris_frame_mailbox`) decoupling the UI event loop from the compositor presentation engine (ADR-0124).
- **iris**: Dedicated presentation worker thread dispatch on Wayland (`wp_presenter_thread_main`), asynchronously consuming immutable scene snapshots and presenting to Vulkan swapchains without stalling the UI main event pump (`[INV-THREAD-01]`, `[INV-THREAD-03]`).
- **prism**: Persistent Material Backdrop Boundary caching in `prism_backdrop_layer_filter` (`[INV-COMP-04]`). Skips compute passes (clear, statistics, frost, and glass dispatches) when input textures and material parameters remain unchanged across frames, directly reusing the persistent slot output.
- **tests**: `test_frame_mailbox` asserts wait-free atomic exchange, monotonic sequence ordering, and concurrent multithreaded producer-consumer integrity.
- **tests**: `test_display_pacer` pins the cadence contract headlessly (16.5k checks) — display-driven lockstep at 60/120/144 Hz, the skip-render no-spin regression, caret idle alignment, quiescence, and forced-paint precedence.
- **tests**: `test_repaint` now asserts `lens_damage_rect` cleanliness during quiescent steady state and bounds containment under leaf mutations.
- **examples**: `frame_skip_demo` is the live acceptance for the skip-render regression: it prints its tick rate and paint/skip counters so a cadence collapse or a busy-spin is visible at a glance.

### Fixed

- **examples**: `paint_static_demo` never printed its counters — it compared elapsed seconds against an absolute start timestamp, so the report condition was never true. It also called `lens_begin`/`lens_end` *inside* its build callback, though iris already brackets the build, which reset the frame mid-build and wiped the input iris had just filled in.

## [0.0.61] - 2026-10-25

### Added

- **lens**: Orthogonal `icon_size` style atom (`LENS_STYLE_ICON_SIZE`), `lens_style.icon_size`, `lens_style_resolved.icon_size`, `lens_theme.icon_size`, and `lens_selectable_opts.icon_size` for decoupled icon and typography scaling (ADR-0111).
- **lens-rs**: `Theme::with_icon_size`, `Style::with_icon_size`, and `Frame::selectable_icon_sized`.
- **lens**: Device-resolution icon coverage masks (ADR-0112). Icons are baked once per (icon, device size, stroke, outline, paint run) into a device-free canvas at 8×8 supersampling and blitted 1:1 through the batched glyph-run pipeline, so small-icon edges carry tens of coverage levels instead of the draw-time 4× MSAA budget's three. The cache is bounded (96 entries, LRU) and falls back to the live vector path if a bake fails.
- **flux**: `flux_canvas_supersample_desc` (pNext on `flux_canvas_desc`) and `flux_canvas_create_cpu_ss` set the software rasteriser's supersampling factor per axis (1..`FLUX_CANVAS_SUPERSAMPLE_MAX`); N samples resolve N+1 coverage levels. Out-of-range fails with `FLUX_ERROR_OUT_OF_RANGE` rather than clamping.
- **flux**: `flux_canvas_cpu_coverage` returns a single-channel alpha-coverage plane (one byte per pixel) — the mask readback coverage caches are baked from. New optional `read_coverage` backend vtable slot; the Vulkan backend leaves it NULL.
- **flux**: `flux_glyph_run_desc` may now carry both an `atlas` and a `host_coverage`; the canvas backend selects the source it can sample instead of rejecting the dual payload. This makes one compiled display list replayable into either backend.
- **flux**: `<flux/sdf.h>` — signed distance fields for the analytic geometry classes (ADR-0118). `flux_sdf_circle`, `flux_sdf_round_rect`, `flux_sdf_ring` and `flux_sdf_coverage` place closed-form distance fields as a peer of the curve kernel: a shape with an exact boundary distance gets exact, resolution-independent coverage instead of being flattened and sampled. The software backend's SDF host port now evaluates the shared primitive, so host and device coverage agree by construction rather than by tolerance; `test_sdf` pins the primitive against the shader's expression. Pure leaf module — no device, no allocation.
- **flux**: The analytic coverage fill reuses a canvas-owned plane instead of allocating one per draw — the per-fill buffer was 8 MB at 1080p and 33 MB at 4K. Measured cost against the tessellation path it replaces, one fill on a 512² CPU canvas: **0.17–0.28× the time on typical UI geometry** (circle, 64- and 256-vertex shapes) and **0.82–0.90× on pathological 512/1024-vertex ones**, i.e. never slower. Both paths are quadratic-ish in vertex count (an active-edge rescan per scanline band vs ear clipping), which is a pre-existing property of the software backend; ADR-0119 records the active-edge-table fix as the follow-up rather than claiming it here.
- **flux**: Gradient path fills also rasterize through analytic coverage. Coverage answers *where* a pixel is painted, so the seam op takes the draw's brush rather than a flat colour, and the CPU backend evaluates a gradient per pixel with the same `frag_gradient` the triangle path uses (a backend that cannot evaluate a brush declines and the draw falls back per draw). Measured on one oblique edge: **117 levels for a solid fill, 88 for a gradient, against 10 for the device's 4× MSAA**. What still tessellates on the CPU is now images, glyph runs, strokes and non-SRC_OVER blends — which is exactly the scope `test_canvas_tess_cache` exercises the cache through.
- **flux**: Solid path fills now rasterize through analytic coverage instead of triangles (ADR-0119 S2). `canvas_fill_path_internal` routes a solid SRC_OVER fill to the backend's optional `submit_cover` op when it has one, so the CPU backend gets continuous per-pixel coverage — 17–65 distinct levels on curved and oblique geometry where a 4× sample count yields five — with no ear clipping, no tessellation-cache traffic and **no stencil attachment**. That last point is a capability gain: an even-odd fill in a no-stencil pass used to be diagnosed and dropped, and now paints correctly, because winding is accumulated from sorted crossings rather than an algebraic area sum. Gradients, images, glyphs and strokes keep the triangle path and the supersample buffer, so nothing is removed in this step. `canvas_consistency` (GPU↔CPU) and the render goldens pass unmodified with the flip active.
- **flux (internal)**: Sub-band exact coverage rasterizer (ADR-0119 S1) — `canvas_coverage_rasterize` computes the exact per-pixel area of the region a fill rule selects, from flattened contours, so coverage is continuous rather than quantised by a sample count. It splits the scanline axis at every edge endpoint and row boundary, sorts crossings at each band's midpoint, accumulates winding, and integrates each span's overlap with each pixel column in closed form. Non-zero and even-odd both work because winding comes from sorted crossings rather than an algebraic area sum, so a self-intersecting contour is painted rather than cancelled. Verified against the independent S0 harness by `test_coverage_raster` (4186 assertions): exact to float precision (≤5e-7) on squares at four grid origins, a 64-gon circle, a half disc and the production flattener's own output, carrying 17–65 distinct coverage levels where a 4× sample count carries five. The seam is landed: an optional `submit_cover` backend op plus a CPU implementation that composites coverage per supersample, with routing gated behind `FLUX_COVERAGE_FILL_ENABLED` (off). Enabling it is stage S2, whose three impacts are measured and listed in ADR-0119.
- **tests**: Coverage acceptance harness (ADR-0119 stage S0) — the independent ground truth every coverage backend will be measured against, deliberately outside libflux so a backend cannot validate itself against its own algorithm. `cov_harness_area` computes exact per-pixel planar area by clipping flattened contours to each pixel and summing signed areas; `cov_harness_supersample` provides the winding-sampled reference; `cov_harness_compare` produces one conformance report. Verified by `test_cover_reference` (60 assertions): the coverage sum equals the geometry's signed area exactly at integer and fractional offsets and with a wound hole; the area and sampled routes converge (mean 0.02 → 0.005); an oblique edge carries **31 coverage levels** where a 4× sample count carries 4; and a self-intersecting contour is **refused** rather than silently mis-reported.
- **flux**: `flux_curve_cover_coverage` evaluates coverage of a tile from the winding of curve strips (supersampled, and named so — it is a reference evaluator, not the analytic area of ADR-0116). It is validated against the closed-form distance field on the same shape: the two routes share no code, and they agree to 0.08 at 16×16 samples and 0.04 at 32×32, monotonically improving. A curved strip yields more than eight distinct coverage levels, beyond the five a 4× sample count can express.
- **flux**: Rotated rounded rectangles, circles and squircles now render through the analytic SDF pipeline instead of falling back to tessellation. The field is evaluated in the shape's own space through the inverse of the transform's linear part, which is exact for any similarity (rotation × uniform scale preserves distance), and the one-pixel coverage ramp is expressed in shape units so it stays exactly one screen pixel at any scale. Shear and anisotropic scale are deliberately excluded — they distort distance — and keep tessellating rather than being drawn with an approximated field. Verified against the tessellated path under rotation and rotation×scale with zero wrong-side pixels.
- **flux**: `FLUX_GEOM_SQUIRCLE` now renders through the analytic SDF pipeline instead of flattening and tessellating, so a continuous-curvature corner gets exact, resolution-independent coverage for the cost of one quad (ADR-0118 INV-SDF-03). Curvature rides the previously-unused `image_src[2]` push slot, so there is no ABI change and no extra pipeline variant. `draw_sdf_rrect` became `draw_sdf_shape` and both the device shader and the software backend evaluate the shared `flux_sdf_squircle`, so host and device agree by construction.
- **flux**: `flux_path_add_squircle` now draws the same corner shape as `flux_sdf_squircle`, closing the schism where the canvas and the Prism materials rendered different squircles for the same `curvature` (ADR-0118 INV-SDF-06). Each corner is two cubics fitted to the field's zero set; the fit is validated by recovering the classically known two-cubic circle constant at curvature 0 (0.2652022 vs 0.265216) and stays within 0.3 % of the corner radius across the family. **Shape change:** the corner radius is now `radius` at every curvature (the arc reshapes in place) rather than growing to `radius·(1 + 0.52c)`.
- **flux**: `flux_sdf_squircle` completes the field set: a rounded rectangle whose corner arc blends a circular arc (`curvature == 0`, which reproduces `flux_sdf_round_rect` bit-for-bit) with an L4 superellipse (`curvature == 1`). The Prism materials now consume this single definition instead of restating it, so the squircle field — like the rounded-rectangle field — has one owner (ADR-0118 INV-SDF-02). Rendered Prism output is unchanged.
- **flux**: The analytic distance field is now defined **once** for GLSL consumers too: `include/flux/shaders/flux_sdf.glsl` is included by the canvas SDF shader and the five Prism materials, replacing eight copies of the same rounded-rectangle expression (ADR-0118). The snippet is declared as a build dependency, so changing it rebuilds the SPIR-V instead of leaving stale copies, and every material's output is numerically identical.
- **flux**: `<flux/curve.h>` — the `flux_curve` analytic curve kernel (ADR-0115). It reduces a cubic to line/quadratic segments with an exact deviation bound (`|E|·1/(6·2√3)`), locates inflections and cusps in closed form, counts segments by Wang's formula, offsets a reduced segment analytically (`flux_curve_seg_offset`) together with the closed-form **retrograde interval** where that offset self-intersects (`flux_curve_seg_offset_retrograde` — a quadratic's curvature numerator is constant, so "the offset exceeds the local radius of curvature" is a quadratic in the parameter), and exposes the coverage primitives (bounds, ray crossings, signed winding, fill rules). Backend-agnostic leaf module — no device, no raster API, no allocation on the hot path; every failure is explicit and writes nothing.

- **tests**: `test_canvas_coverage_ceiling` measures the coverage gap S3 exists to close, rather than leaving it as prose: the same oblique edge renders as **117 distinct coverage levels through the CPU analytic path and 10 through the GPU's 4× MSAA**. The assertion is the INV-COV-02 contract threshold (>16) which the device path cannot meet by construction, so the test fails loudly the day the device gains coverage — S3's completion condition is moving that expectation, not remembering to.

- **docs**: ADR-0120 plans GPU analytic coverage for arbitrary paths (tile binning, prefix-sum winding, per-pixel area integration) and makes two things explicit that were previously implicit: the acceptance instrument is the CPU reference rasterizer plus the independent exact-area harness built in ADR-0119, and a **real-driver CI lane is a prerequisite**, because every failure mode of the plan is device-specific. It also records why the cheaper routes were rejected: per-primitive analytic fragments cannot express holes, self-intersection or arbitrary winding; uploading a CPU-rasterized coverage texture is a bake (ADR-0112's failure mode); and raising MSAA moves the ceiling only linearly (10 measured levels at 4×).

### Fixed

- **flux**: Translucent strokes no longer double-blend where the band overlaps itself. The stroker now clamps the inner-side offset to the local radius of curvature, so a stroke whose half-width exceeds the curve's radius of curvature tiles its region exactly once instead of emitting a self-overlapping band (an r=6 circle at width 20: 204 double-blended pixels → 4, each about one supersample). Opaque strokes are unchanged — the clamp fires only past the curvature limit — and the whole suite, including the icon pixel tests, is unmodified. Covered by the new `test_canvas_stroke_region`, an analytic coverage oracle that renders a translucent stroke against a single-coverage fill reference.
- **lens**: Runtime SVG registration preserves per-shape paint runs when theme-colored shapes combine fill and stroke (`currentColor`), preventing premature whole-icon fill collapse (ADR-0111).
- **lens**: Pixel-snap geometry positions in selectable row and button skins to prevent half-pixel stroke smearing.
- **lens**: Icon origins and stroke widths are resolved in device space (ADR-0112). Command replay snapped the origin to the device grid and the icon branch then re-rounded it in *logical* space, moving the glyph off the grid at fractional scales (1.25/1.5); icons also drew at a sub-1.5 px stroke that never reached full opacity. Both are fixed and covered by centroid tests at scale 1.0/1.25/1.5/2.0.
- **lens**: An icon's vector fallback again honours its paint runs. While masks were being introduced, the fallback collapsed the whole command stream into a single paint, discarding every per-shape colour a runtime SVG declares (the regression ADR-0111 had fixed). Run resolution and geometry emission are now single-sourced (`lensi_icon_run_spans` / `lensi_emit_run_path`), consumed by both the mask bake and the fallback, so the two cannot drift again.
- **lens**: Icons no longer vanish when a device-backed context renders into a device-less canvas (headless snapshot, offscreen export). Mask representation is selected by the consuming canvas rather than by `lens_desc.device`, and every mask keeps its host coverage bytes alongside its R8 image.
- **lens**: An icon with an outline no longer loses its outline colour — body and outline are separate mask layers, blitted outline-first.
- **lens**: Icons no longer allocate path scratch from the per-frame arena, removing the dominant per-frame allocation for icon-heavy UIs.

### Performance

- **flux**: Convex path fills now take an O(n) winding-robust fan
  fast-path in the ear-clip tessellator (`ear_clip_contour`,
  ADR-0113). A strictly-convex CCW contour is fan-triangulated linearly
  instead of paying the O(n²) blocking-vertex scan; measured ~340× /
  ~100× / ~54× on 2048- / 1024- / 512-vertex regular polygons
  (7.05 ms → ~20 µs, 1.06 ms → ~10 µs, 279 µs → ~5 µs per fill). The
  convexity gate sums total exterior turning to separate winding-one
  convex polygons from K-wound self-intersecting stars, so the
  self-intersecting stencil fallback (ADR-0014) is never bypassed.
- **flux**: The ear-clip tessellator's remaining O(n²) full-ring scan is
  gone (ADR-0114). `ear_clip_contour` now scans only an incremental
  non-convex **candidate set** (reflex or collinear vertices — FIST /
  FitzHugh), updated in O(1) per clip, so concave fills with few reflex
  vertices track near-linearly (gear shapes: 3.3–3.8× faster; a
  320-vertex gear 193 µs → 51 µs). The `cross <= 0` candidate test keeps
  the emitted triangle stream byte-identical to the deleted legacy scan
  (verified by a differential harness over convex/combs/stars, plus the
  GPU↔CPU pixel oracle), and contours that exceed the inline scratch
  allocate matching heap scratch rather than falling back to O(n²). No
  ABI, cache-key, or render-plan change.
- **flux**: Path flattening no longer recurses (ADR-0115). The deleted
  `geometry_flatten.c` is replaced by `curve_flatten.c`: each curve is sampled
  at the segment count the `flux_curve` kernel's Wang's formula returns — a
  closed form with a provable deviation bound, no depth cap and no flatness
  search. Screen-space tolerance is preserved (0.25 px) and rendered output is
  unchanged (the GPU↔CPU pixel oracle and the render golden tests pass
  unmodified). Measured against the deleted implementation on the same corpus
  it is faster and emits ~45% fewer points for the same guarantee (a 50px
  circle at 1:1: 65 → 37 points). No public API change;
  `flatten_path_to_contours` keeps its contract.

## [0.0.60] - 2026-10-17

### Fixed

- **flux**: The deferred dma-buf `FOREIGN → graphics-family` acquire transition now recycles its command pool into the graphics-family transient cache (and frees the command buffer) instead of destroying and recreating it on every import. The pool is tagged with its real queue family rather than an unknown one, so the removed `flux_vk_upload_pending_park` entry point no longer forces a per-import `vkDestroyCommandPool` + `vkCreateCommandPool`.

## [0.0.59] - 2026-10-16

### Added

- **flux**: `flux_frame_abandon` — release a submitted frame-slot borrow without presenting or advancing the offscreen ring slot, so a deferred or rejected external KMS flip reuses the same free backbuffer on retry (ADR-0110).
- **flux-rs**: `SubmittedFrame::abandon` and a `Drop` implementation that releases an unpresented frame deterministically.

### Fixed

- **flux**: Offscreen export semaphores are recreated on the next `begin_frame` after an abandoned external flip, so `vkQueueSubmit2` never starts from an already-signalled binary semaphore (ADR-0110).
- **flux**: The backdrop effect clamps every sampling tap to the captured region extent, eliminating the bright seam where a sub-region glass capture wrapped neighbouring output content into its edge (ADR-0110).
- **tests**: `test_helpers_make_validation_dmabuf_device` provides a validation-enabled device with the dma-buf external-memory features, the only configuration in which a silently-tolerated dma-buf ownership defect is observable.

## [0.0.58] - 2026-10-15

### Added

- **lens, flux, glyph**: Command-level premultiplied opacity modulation, scissor-bounded Vulkan offscreen layer passes, and batched GPU glyph runs (ADR-0109).
- **lens-rs**: Exposed `NodeBuilder::opacity` API for node-level opacity control.

## [0.0.57] - 2026-10-14

## [0.0.56] - 2026-10-14

### Added

- **transit**: `transit_spring_advance_substep` bounded sub-stepping integration for harmonic springs (ADR-0108), ensuring continuous physical trajectory and velocity integration under irregular frame delivery.
- **transit-rs**: `Spring::advance_substep` binding and test coverage.
- **flux**: `flux_bucket_extent` 128-pixel extent bucketing in `<flux/math.h>` to prevent dynamic size sweeps from thrashing render-target allocations.
- **flux-rs**: `flux::bucket_extent` helper.

## [0.0.55] - 2026-10-09

### Added

- **iris**: `iris_event` and the pre-fold `iris_app_opts.on_event` interception seam (ADR-0107) — hosts can observe, drop, or rewrite native input before it folds into `lens_input`. Wired across the Wayland, Win32, and Cocoa backends (pointer, scroll, keyboard, modifiers, text/IME, touch, pen, drag-and-drop, and lifecycle events).
- **tooling**: `actionlint` gate (`ci.yml` `workflow-lint` job) so a malformed workflow file fails the pull request instead of silently disabling every job it contains.

### Fixed

- **tooling**: Repaired `.github/workflows/ci-cross.yml` — a malformed `runs-on`/`env` line made the file invalid YAML, so GitHub rejected the entire cross-platform workflow and silently disabled every cross-platform gate (native Windows, macOS/Cocoa, and the `zig-win32-check` compile job).

## [0.0.54] - 2026-10-02

### Added

- **transit**: `transit_smoothstep` cubic Hermite interpolation easing function.
- **transit-rs**: `smoothstep` wrapper and smoke tests.

### Fixed

- **iris**: Wayland repeat timer cancellation on redundant compositor modifier snapshots; explicit default repeat rate and delay initialization.

## [0.0.53] - 2026-09-28

### Changed

- **transit** (breaking, pre-1.0): every advance entry point now takes an
  explicit `reduced_motion` flag — `transit_approach`, `transit_decay`,
  `transit_hysteresis_step`, and `transit_smoother_step`. When set, the
  primitive resolves to its end state in a single step (mirroring the
  existing `transit_spring_snap_to`), so a caller never hand-rolls the
  reduced-motion rule per primitive.
- **transit-rs**: mirrors the flag on `approach`, `decay`, and the
  `Smoother`/`Hysteresis` `step` methods; adds `Default` for `Spring`,
  `Smoother`, and `Hysteresis`, `SpringParams::new`, and `dt_clamp`.

### Fixed

- **build**: `glyph` and `vista` `.pc` files now require
  `flux >= 0.0.53` instead of an unversioned `flux`, so a partially
  upgraded install fails at `pkg-config` time rather than linking against a
  newer `libflux` than the headers were built for.

## [0.0.51] - 2026-09-27

### Added
- **lens**: Flow wrap and in-tree stack layout subsystems and semantic surface tokens (ADR-0105).
  - Added multi-line flow wrapping container: `lens_wrap_begin` / `lens_wrap_end` and `lens_layout_opts.wrap` with configurable `row_gap`.
  - Added in-tree stack layering container: `lens_stack_begin` / `lens_stack_end` for composite cards, badges, and overlays.
  - Added orthogonal `align` (horizontal) and `cross` (vertical) alignment hints to base `lens_box`.
  - Established first-class semantic surface design tokens: `color_surface`, `color_surface_sunken`, `color_surface_elevated` in `lens_theme` with automatic normalization.
  - Added `lens_theme_classic(bool dark)` preset for 1px wireframe desktop style fallback.
- **lens-rs**: Added `f.wrap()` and `f.stack()` fluent container builders, `StackBuilder`, `LayoutOpts.wrap`/`row_gap`, `Theme::surface()`, `Theme::surface_sunken()`, `Theme::surface_elevated()`, and `Theme::classic(dark)`.

### Changed
- **lens**: Established minimalist borderless baseline by default (`border_width = 0.0f` across default light/dark themes).
- **lens**: Replaced legacy wireframe hacks in `TextEdit`, `Checkbox`, `Segmented`, and `Slider` tracks with direct bindings to `color_surface_sunken`.
- **lens**: Fixed replay engine bug where `border_width <= 0.0f` fell back to `1.0f`.

## [0.0.50] - 2026-09-27

### Added
- **lens**: Unified orthogonal 2D grid and 1D flex layout subsystem (ADR-0104).
  - Added multi-column and multi-row spans (`col_span`, `row_span`) and explicit grid coordinates (`grid_col`, `grid_row`) to the base `lens_box` model.
  - Added immediate-mode procedural layout modifiers: `lens_col_span`, `lens_row_span`, and `lens_grid_at`.
  - Upgraded `lens_grid_opts` with uniform `row_height` specification, horizontal (`align`) and vertical (`cross`) cell alignment, and panel container styling (`bg`, `radius`, `border`, `border_width`).
  - Implemented 2D grid placement algorithm in `grid.c` supporting dense automatic packing (Bento Grids), explicit cell placement, and multi-span deficit distribution with zero dynamic heap allocations.
  - Extracted 1D flexbox layout engine into dedicated symmetric `flex.c`, decoupling `solve.c` into a pure two-pass coordinator.
  - Added full composability and arbitrary depth nesting between 1D Flex (Row/Column) and 2D Grid containers.
- **lens-rs**: Added `GridBuilder` and `f.grid(columns)` with `.col_span()`, `.row_span()`, `.grid_at()`, `.align()`, `.cross()`, and complete fluent container chaining.

## [0.0.49] - 2026-09-27

### Changed
- **iris**: Platform-agnostic accessibility bridge, asynchronous transport, and lazy activation (ADR-0103).
  - Eliminated synchronous blocking D-Bus calls on window startup, avoiding main-thread freezes.
  - Gated accessibility connection on desktop session state (`ScreenReaderEnabled` / `IsEnabled`) with inert zero-cost degradation when inactive.
  - Replaced subshell `popen` in `a11y_prefs_linux.c` with direct in-memory Portal Settings D-Bus queries.
  - Removed duplicate per-frame `iris_a11y_update` across Wayland, Win32, and Cocoa backends.
  - Purged platform-specific AT-SPI/D-Bus details from public `<iris/a11y.h>` contract.

## [0.0.48] - 2026-09-23

### Added

- **prism**: Uncompromised dielectric optics and sub-pixel Contact Ambient Occlusion (CAO) (ADR-0102). Liquid glass is modeled as pure dielectric optical medium ($\text{IOR} \approx 1.52$), replacing monolithic smoke plate darkening with volumetric absorption along optical thickness. Added `contact_ao` (sub-pixel contact boundary crease) and `ambient_fresnel` (360-degree isotropic environmental Fresnel sheen).
- **prism**: Theme-adaptive contact rims in `acrylic.comp` and `mica.comp` that transition smoothly between specular white highlights in Dark mode and dark slate contact strokes (`0x181F2E`) in Light mode.
- **lens**: Added `contact_ao` and `ambient_fresnel` to `lens_material_recipe`. `lens_theme_for_material` now equips Light theme surfaces with subtle dark slate contact borders.

### Removed

- **prism**: Completely eliminated `plate_polarity` and `backdrop_energy` from `prism_liquid_glass_group` and `prism_liquid_glass_desc`. Liquid Glass does not pretend to have an opaque plate.
- **prism-rs**: Removed `plate_polarity` and `backdrop_energy` from Rust bindings `LiquidGlassGroup` and `LiquidGlassParams`.

## [0.0.47] - 2026-09-23

### Fixed

- **flux-scene-graph**: `sg_update_worlds` propagated world matrices by iterating the whole node array `node_count` times to converge a glTF node tree that is not topologically sorted — O(n²) matrix multiplies on every animated frame (skin-palette and camera updates included). A skinned VRM avatar (117 nodes) cost ~0.11 ms per pose and grew quadratically with rig size (600 nodes: ~5.4 ms). Nodes are now walked in one parents-before-children order, computed once from the immutable parent links (roots derived from the links, so the animation retargeter's root-less scratch scene works too) and reused every frame; the pathological all-reversed order folds in one pass. A missing/out-of-range/self parent folds as a root and an allocation failure falls back to the previous convergence loop, so malformed input stays correct and in-bounds. 117-node pose update ≈20× faster; the gain scales with node count (≈65× at 600 nodes).

## [0.0.46] - 2026-09-19

### Added

- **lens**: Tab strip drag-to-reorder (ADR-0086 parity): 14px drag threshold, hover-predicted insertion slot, and `LENS_TAB_ACTION_MOVE` emitted in post-removal coordinates; `pressed_on_tab` flag lets CSD hosts suppress window dragging on tab press. Headless `test_tab_strip` C test (42 checks).

## [0.0.45] - 2026-09-19

### Added

- **flux / prism**: Formalized physical sampling governor and bounded material dispatch architecture (ADR-0101). Codified physical Shannon-Nyquist bandwidth governor, sampling tap budgets, and bounded footprints.
- **lens**: Added `lens_theme_for_material` to synthesize material-adapted, translucent vibrant themes for UI components resting on top of physical materials (Liquid Glass, Acrylic, Mica, Frosted Glass) (ADR-0101).

### Fixed

- **prism(shaders)**: 3D Meniscus dual-rim lighting and G2 metric gradient normalization for Liquid Glass (ADR-0100). Restored unbroken specular highlights across all 4 corners and closed capsule arcs ($1.30:1$ sweet-spot ratio); eliminated ghost focus field artifacts on hero glass plates.
- **prism(shaders)**: Fixed hollow-shell shadow discontinuity across Mica, Acrylic, and Frosted compute shaders, curing misaligned ghost card outlines.
- **showcase**: Optimized `material_gallery` with circular theme toggle, left control sidebar, material-styled tab buttons, 4-up grid symmetry with unified monolithic Mica tile, and dynamic background switcher.

## [0.0.44] - 2026-09-18

### Added

- **iris / lens**: Desktop platform integration — extended function keys (F1–F24), programmatic focus activation (`iris_window_request_focus`), and MIME-typed clipboard interop across Cocoa, Wayland, and Win32 platforms (ADR-0099).
- **docs**: Added Value-Semantic Rendering (VSR) architectural explanation and data-flow model (`docs/explanation/rendering-model.md`).

## [0.0.43] - 2026-09-16

### Added

- **prism**: Added Mica foundation material (`prism_mica_pipeline`, compute shader) for wallpaper-anchored translucent surfaces (ADR-0096).
- **tooling**: Added unified verification gate `tools/check-all.sh` combining formatting, material boundaries, version lockstep, purity, symbol freshness, and documentation governance (ADR-0098).
- **governance**: Upgraded documentation governance standard to Protocol v5.1.0 (Clean-Break Architecture with 4D Semantic Tensor Core and pluggable vertical domain profiles). Integrated `tools/verify.sh` and `tools/check-docs-governance.sh` into CI validation.
- **examples**: Added canonical examples: `hello_window` (Iris), `material_gallery` (Showcase), and `motion_spring` (Anim) (ADR-0097).

### Changed

- **flux**: Converged public C contracts and safe Rust bindings with strict parameter lifetime semantics and error handling (ADR-0095).
- **lens / iris**: Publish owned scene snapshots and activate the exact snapshot after successful presentation. Input, scrollbar hit geometry, accessibility and repaint baselines follow the activated snapshot. Failed recording or submission does not publish a partial frame.
- **flux**: Retain immutable child lists with isolated state and bounded nesting and expanded execution. Crop captured host glyph coverage to the sampled region. Track referenced samplers and display lists in frame retirement queues so in-flight execution holds ownership through fence retirement.
- **flux-text**: Replace `flux_text_draw_to_encoder` with fallible `flux_text_record`; failures poison the recorder.

### Removed

- **lens**: Remove `lens_render`, `lens_draw_list`, `lens_compile_draw_list`, `lens_draw_list_submit` and `lens_notify_presented`. Use snapshot creation, submission and activation.
- **flux**: Remove native-memory DisplayList serialization and `flux_encoder_append_display_list`. Use `flux_encoder_draw_display_list` for shared child composition.
- **examples / crates**: Pruned legacy demos (`filament_plume`, `julia_morph`, `liquid_glass_study`, `particles_terrain`, `ripple_field`, `hello_triangle`, `iris/forms`, `iris/hello_app`, `iris/minimal`) and retired independent crate workspaces (ADR-0097).

### Corrected

- The 0.0.42 architecture completion claims were corrected with active tracking in [implementation status](docs/dev/architecture-implementation-status.md).

## [0.0.42] - 2026-09-14

### Added

- **flux**: Implemented clean-break DisplayList architecture with relocatable command capture, private command storage, and independent refcounted lifecycle (`flux_encoder_create`, `flux_encoder_finish`, `flux_display_list_retain`, `flux_display_list_release`) (ADR-0089 / ADR-0090).
- **flux**: Implemented orthogonal `flux_geometry` $\times$ `flux_brush` drawing model with G2-continuous squircle curvature, independent rounded image clips, and brush opacity modulation (`flux_canvas_draw_geometry`) (ADR-0091).
- **flux**: Added native Vulkan backend `save_layer` and `restore_layer` with isolated dynamic rendering passes, intermediate linear pooling, and opacity composite blitting without double-blend artifacts (ADR-0091 / ADR-0092).
- **flux**: Added production-grade binary serialization protocol for DisplayList (`flux_display_list_serialize`, `flux_display_list_deserialize`) with versioning, Adler-32 checksums, and fuzzing-grade float/bounds validation (ADR-0090).
- **flux**: Added DisplayList slice splicing (`flux_encoder_append_display_list`) with automatic GPU resource retention.
- **flux**: Added RenderPlan execution diagnostics (`flux_canvas_get_plan_stats`) tracking pass counts, peak layer depth, and transient aliasing memory savings (ADR-0089 / ADR-0092).
- **lens**: Added decoupled scene snapshots and presentation generations (`lens_snapshot_create`, `lens_snapshot_generation`, `lens_snapshot_submit`, `lens_generation`, `lens_notify_presented`, `lens_last_presented_generation`) (ADR-0094).
- **lens**: Added node-level DisplayList slice caching and incremental diffing for clean subtrees in `lens_compile_draw_list` (ADR-0094).
- **iris**: Integrated presentation handshake (`lens_notify_presented`) across Wayland, Win32, and Cocoa loops upon successful frame presentation (ADR-0084 / ADR-0094).
- **flux-rs**: Added `DisplayList` and `Encoder` safe bindings with consuming `finish(self)` and sealed `AsTarget` trait (ADR-0093).

### Changed

- **flux**: Enforced runtime exclusive target lease in `flux_canvas_begin` (`FLUX_ERROR_INVALID_STATE` on overlapping canvas bindings) and prevented concurrent CPU readback while recording (ADR-0093).
- **flux**: Rewrote `<flux/canvas_helpers.h>` convenience functions to exclusively construct `flux_geometry` $\times$ `flux_brush` and route to `flux_canvas_draw_geometry`.
- **flux-rs**: Removed `Canvas::begin` and `Canvas::begin_target` bypasses, enforcing `CanvasSession` with exclusive `&mut Target` borrow and `Frame::target(&mut self)`.

### Removed

- **flux**: Physically removed legacy `flux_shape`, `flux_shape_kind`, `flux_shape_*` helpers, and `flux_canvas_draw`.
- **workspace**: Retired and removed independent Rust planner `crates/flux-composition-graph` per ADR-0089.

## [0.0.41] - 2026-09-12

### Added

- **lens**: Added grid layout support (`lens_grid_begin`, `lens_grid_end`), `lens_space_between`, and `lens_fit`.
- **flux**: Added shape error validation, transform checks, and unified image draw support in `flux_canvas_draw`.
- **flux**: Added `flux_material_release_deferred` for retire-queued material pipeline destruction.
- **flux-text**: Added `FLUX_TEXT_VERSION_NUMBER`, `flux_text_version`, `flux_text_version_number`, and `flux_text_version_check` accessors for stack-wide version lockstep.

### Changed

- **lens**: Implemented O(1) store live-list unlink and backward-shift erasure on slot deletion.
- **flux-rs**: Enforced path arena lifetime via `'arena` parameter, made `read_pixels` return an owned `Vec<u8>`, and updated `Material::drop` to use deferred release.
- **lens-rs**: Scoped `Frame<'frame>` to prevent cross-frame handle leakage and added drop guard for `lens_end`.

### Fixed

- **flux-text**: Fixed CJK full-width bracket compression pen width calculation so trailing extents correctly reflect the compacted line.
- **iris-rs**: Fixed lifetime parameters on `Frame<'_>` closures and avoided interactive dialog hangs during test runs.
- **lens**: Hardened `test_tooltip` with CPU canvas rasterization and pixel assertions.

## [0.0.40] - 2026-09-12

### Changed

- **lens**: Increased drag threshold from 4px to 14px to prevent accidental drag initiation during clicks.
- **iris**: Synchronized accumulator cursor position on Wayland drag enter/motion events, and cleanly reset pointer state on drag cleanup.

### Added — Rust bindings

- **lens-rs**: Added `Ui::deliver_drop`, `Frame::dnd_source`, and `Frame::dnd_drop_target` bindings with `DndDropInfo`.
- **iris-rs**: Re-exported `DndDropInfo` from `lens`.

## [0.0.39] - 2026-09-12

### Added

- **lens**: `lens_consume_key` API to mark input keys as consumed, preventing bubbling and tab navigation for handled keys.
- **lens**: `lens_textedit_get_caret` and `lens_textedit_get_selection` APIs to inspect textedit caret position and selection range.

### Changed

- **lens**: Enhanced textedit navigation and selection with word boundary jumping, multi-line navigation, and UTF-8 boundary handling.

## [0.0.38] - 2026-09-12

### Added

- **prism**: Squircle `curvature` blend factor parameter on `prism_liquid_glass_group`, `prism_liquid_glass_desc`, and `prism_backdrop_layer_desc` (along with safe Rust bindings).

### Changed

- **prism**: Scale deep interior frost for high frost strengths (`frost_strength > 1.0`) in liquid glass compute shader.

## [0.0.37] - 2026-09-10

### Added

- **flux-scene-graph**: Device-free GLB parse seam (`flux_sg_parse_glb`, `flux_sg_scene_data_build`, `_free`, `_primitive_count`, `_bounds`); `flux_sg_load_glb` is now their parse+build composition.
- **api**: Every library exposes the unified `*_version` / `*_version_number` / `*_version_check` / `*_version_string` accessors, all derived from the header macros.
- **prism**: `prism_liquid_glass_max_shapes()` reports the caller-owned shape-array capacity.
- **lens**: Borrowed-string registry (ADR-0084) aborts with a diagnostic when a stack-temporary string is passed to a widget.

### Changed

- **prism** (breaking, pre-1.0): `prism_liquid_glass_group.shapes` is now a caller-owned borrowed array (`const prism_liquid_glass_shape *`) instead of the inline `shapes[2]`; the shader slot ceiling is one validated `LIQUID_GLASS_MAX_SHAPES` constant.

### Fixed

- **flux-text**: Leading combining marks no longer produce negative measured width or inverted selection rects.
- **flux-scene-graph**: Clamp chunk-header walking in `sg_glb_parse` to avoid an out-of-bounds read on a zero-length container.
- **iris**: Unpublish `g_active_pl` and `tablet_host_bridge.user` on the `app_wayland` early-return path; fix a text-atlas use-after-free under ASan.

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
