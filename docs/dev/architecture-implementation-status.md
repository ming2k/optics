# Architecture implementation status

Updated 2026-10-25. This matrix tracks the requirements in ADR-0089 through
ADR-0094 and the curve-rendering program ADR-0115 through ADR-0117. Acceptance
of an ADR is a design decision, not implementation acceptance. The earlier
blanket `Complete` declarations were not supported by code and contract tests
and have been withdrawn.

## Implemented and covered

| Contract | Implementation | Evidence |
|---|---|---|
| Published command ownership | Encoder-owned paths, glyph quads and cropped host coverage; shared immutable child lists with bounded nesting and isolated child state | `tests/flux/unit/test_display_list.c`: relocated capture, glyph ownership, child ownership/placement, nesting, expanded execution budgets and sticky failures |
| No native-memory serialization protocol | Removed serialization/deserialization exports and the raw-struct format; no compatibility aliases | Installed `flux/canvas.h`, generated symbol reference |
| One Lens visual publication path | Removed `lens_render`, `lens_draw_list`, live-tree Canvas replay, and Lens's Canvas segment cache. Snapshots capture visuals, text contours, icons, images, tooltips and leaving nodes | `tests/lens/test_record_replay.c`, `test_ghost.c`, `test_button.c`; Iris Wayland/Win32/Cocoa and Rust Lens migrated |
| Explicit presentation activation | Snapshot-owned bounds, clips, overlay order and semantic strings; context and node-incarnation validation; input, scrollbar hit geometry and accessibility consume activated metadata | `tests/lens/test_snapshot.c`: delayed presentation, old geometry, old semantic content, foreign snapshot rejection, replay after UI destruction |
| Failed publication cannot publish partial visuals | Subtree compiler and text recorder propagate failures; scratch is restored; child cache replacement occurs only after successful capture | `tests/lens/test_snapshot.c`: scratch failure, empty output, successful retry |
| Unpresented work stays dirty | Visual revision and presentation baseline are separate from cache creation | `tests/lens/test_snapshot.c`: unpublished content remains dirty |
| Headless activation is explicit | Test hosts publish and activate through a fixture; build-only hashing tests do not publish fake resource handles | `tests/lens/test_helpers.h`, `test_drawlist_hash.c` |
| Frame-level resource retention & retirement | Frames track referenced samplers and display lists during recording and submission; in-flight execution holds ownership through fence retirement even if caller immediately releases handles | `tests/flux/integration/test_sampler.c`: sampler and display list frame tracking across begin/draw/release/submit/present |

`lens_snapshot_create` reads the mutable UI after `lens_end`; it must not run
concurrently with UI mutation. Retaining a published snapshot owns its captured
visuals and metadata. GPU image, sampler, and display list retention protects object
lifetime through frame completion, not arbitrary external mutation of underlying buffers.

## Remaining acceptance work

| ADR | Status | Required work before completion |
|---|---|---|
| 0089 | In progress | Introduce the authoritative target-specific RenderPlan compiler and submission owner. Integrate raster, upload, scene, compute and Prism work; migrate direct Canvas execution and remove the independent composition planner only after transferring its graph/region/budget capabilities. |
| 0090 | In progress | Immutable image/resource versions, validated imports and dynamic bindings, retained-content budgets, concurrent readers/resource updates, and allocation-failure coverage at every ownership boundary. Serialization remains outside this ADR's scope. |
| 0091 | In progress | Exhaustive backend capability validation, nested effects and group reference images, optimizer/reference equivalence and numeric edge-case tests. Lens opacity is applied once to each node's own visual group; children independently capture the build-time opacity setting. |
| 0092 | In progress | Explicit access dependencies, staging/destination/descriptor lifetimes, completion-driven retirement and budget diagnostics. Text capture still uses physical atlas allocations; logical glyph versions and compilation-time realization remain outstanding. Atlas epoch counters alone do not satisfy this contract. |
| 0093 | In progress | Complete the plan/session/submission state machines and all unsafe-import and delayed-completion cases. Existing borrow checks and owning Rust snapshots cover only part of the contract. |
| 0094 | In progress | Complete resource/font dependency keys and retained-memory budgets. Audit remaining specialized interaction state (IME/focus/virtualized widgets) against delayed presentation and complete custom-visual/animation/backend pixel coverage. |

## Curve-rendering program (ADR-0115 through ADR-0117)

The geometry kernel (ADR-0115) has landed and is tested, and its **flattening
consumer is migrated** (`geometry_flatten.c` is deleted; `curve_flatten.c`
samples each curve at the kernel's Wang segment count). The tessellation and
stroking consumers are not migrated, so `geometry_tess.c`, `geometry_stroke.c`, the pass-scoped MSAA path
(ADR-0071), the icon coverage bake (ADR-0112), and the stencil fill fallback
(ADR-0014) are still the live rendering code. ADR-0116 and ADR-0117 are
design-only. The records below distinguish landed work from the replacement
still owed.

| ADR | Status | Required work before completion |
|---|---|---|
| 0115 | Kernel + flattening landed; stroker retained by decision | **Landed:** `flux_curve` (`libs/flux/src/curve/curve.c`, `include/flux/curve.h`) — exact-bound cubic reduction with inflection/cusp splitting, Wang segment counts, analytic offset, closed-form retrograde interval, ray/winding coverage primitives, and the `flux_curve_cover` value, plus a supersampled `flux_curve_cover_coverage` used for cross-route validation; verified by `tests/flux/unit/test_curve.c` (191 assertions). **Landed:** the flattening consumer — `geometry_flatten.c` (recursive De Casteljau) is **deleted**; `curve_flatten.c` samples each curve at the kernel's Wang count, measurably faster with ~45% fewer points for the same 0.25 px screen tolerance. **Closed by decision, not deferred:** the segment-based stroker rewrite (`geometry_stroke.c` retained). Both improvements it was meant to deliver were measured — exact source curvature is *worse* (r=2 retrograde 0 → 28) and min-of-both reproduces the baseline exactly — so the polyline stroker is the correct stroker for a polyline rasterizer (ADR-0115 Options H and I). Revisit only when ADR-0116 makes the fill path stop emitting polylines. |
| 0116 | S0–S3 landed; S4 in progress (MSAA default retired) | Sequenced by [ADR-0119](../adr/0119-coverage-seam-migration-staged-plan.md). **S0:** coverage harness + `test_cover_reference`. **S1:** `coverage_raster.c` sub-band exact integrator, `test_coverage_raster` (28,774 assertions, exact to ≤5e-7); `submit_cover` backend op implemented on both CPU and Vulkan backends. **S2:** the route is active with an entry scratch guard; even-odd no longer needs stencil for solid paint on a coverage backend. **S3 landed:** `TILE_COMPUTE` backend (`canvas_tile.c`, `canvas_tile_raster.comp`) wired into `backend_vk.c::vk_submit_cover`. The device ceiling test (`test_canvas_coverage_ceiling`) inverted its assertion: measured **104 distinct levels** on the probe edge (exceeding INV-COV-02's >16 contract and retiring the legacy 10-level 4× MSAA ceiling). Even-odd fills succeed without a stencil attachment in strict no-stencil passes (`test_canvas_render`). **S4 underway:** default Vulkan canvas `AUTO` antialias policy retired its 4× MSAA attachment allocation, operating strictly in 1× sample rate; 202/202 tests green. Remaining: retire ear-clip tessellator for strokes once stroked retrograde covers land. |
| 0117 | Landed | **Landed:** `FLUX_MATERIAL_VECTOR` in `libs/flux/src/scene/` (`scene.h`, `scene.c`, `scene_vector.vert`, `scene_vector.frag`). Reconstructs exact analytic vector coverage in screen space under arbitrary 3D perspective transforms via screen-space Jacobian derivatives (`dFdx`/`dFdy`) without baking (INV-SCV-01), with object-space curve inputs (INV-SCV-02), proven projective-correctness (INV-SCV-03: measured **107 distinct coverage levels** in `test_scene_vector.c` under 3D perspective foreshortening and rotation), shared ray-cast semantics with `curve` (INV-SCV-04), and strict module isolation from `canvas` (INV-SCV-05). |
| 0120 | Phases A, B0, B1–B5 landed, Phase C underway | GPU analytic coverage for arbitrary paths, planned in [ADR-0120](../adr/0120-gpu-analytic-coverage-tile-compute-plan.md). **Landed Phase A:** active edge table removes the quadratic rescan (1024-vertex star 26.3 ms → 5.8 ms, strictly linear); ASAN/UBSAN clean across 28,774 assertions; dedicated fuzz harness (`fuzz_coverage_raster.c`) and seeds; benchmark in `bench_coverage.c`. **Landed Phase B0:** real-driver CI hardware lane and anti-downgrade test (`test_device_hardware.c`, asserts non-CPU, non-llvmpipe on Intel ARL GPU). **Landed Phase B1:** segment SSBO storage buffer transport (`canvas_tile_upload_segments`, `test_tile_b1_upload.c`, 4351 assertions matching CPU flattener endpoints). **Landed Phase B2:** coarse raster half-open 16×16 tile binning (`canvas_tile_bin_segments`, `test_tile_b2_coarse.c`, 1321 assertions). **Landed Phase B3:** two-level prefix sum winding backdrop (`canvas_tile_compute_backdrop`, `test_tile_b3_backdrop.c`, 850 assertions matching `flux_curve_cover_winding`). **Landed Phase B4:** fine raster per-pixel area integration (`canvas_tile_rasterize`, `test_tile_b4_fine.c`, exact to ≤ 5×10⁻⁶, oblique edge 135 distinct levels > 16). **Landed Phase B5 & Primary Seam Integration:** wired `.submit_cover = vk_submit_cover` into `backend_vk.c`; pure Vulkan compute shader `canvas_tile_raster.comp` with 64-bit BDA and Liang-Barsky integration verified on hardware (`test_tile_gpu_compute.c`); `test_canvas_coverage_ceiling` inverted on actual Vulkan surface readback (**104 distinct levels** vs CPU 117); even-odd no longer rejected in no-stencil passes (`test_canvas_render`). **Phase C:** retired default 4× MSAA allocation in `backend_vk.c::vk_begin_pass` (AUTO is now 1× analytic coverage). |
| 0118 | Landed (shear fallback is by design) | **Landed:** the GLSL fields are consolidated into `include/flux/shaders/flux_sdf.glsl` — round-rect, ring and **squircle** — included by `canvas_sdf.frag`, `canvas_image.frag` and the five Prism materials (nine copies → one, snippet tracked as a build dependency); `flux_sdf_squircle` completes the C set and `liquid_glass.comp` consumes it (Prism output unchanged). **INV-SDF-06 closed:** `flux_path_add_squircle` draws the field's corner arc as two cubics per corner from a Schneider fit (curvature 0 recovers the classic two-cubic circle constant 0.265216 to 5 decimals). **INV-SDF-03 landed:** `FLUX_GEOM_SQUIRCLE` (solid, SRC_OVER, similarity transform) evaluates the field instead of flatten + tessellate; `draw_sdf_rrect` became `draw_sdf_shape`, the field is evaluated in **shape space** through the inverse linear part so **rotation and uniform scale are exact**, and the extra state rides the free `image_src[2..3]` / `grad_from` / `grad_to` slots (no ABI or pipeline change). Verified by `test_canvas_squircle` (path↔field deviation ≤ 0.3 % of r; analytic vs tessellated agree with zero wrong-side pixels, including under rotation and rotation×scale, with a shear case as the negative control). **Remaining:** the coverage bake is not yet exposed as the explicit opt-in INV-SDF-04 requires; shear and anisotropic scale fall back to tessellation by design. |

The code changes above are internal migration steps. They do not establish
whole-architecture completion, a compatibility window, performance improvement,
formal verification, or completion of the remaining GPU lifetime contracts.

## Validation (2026-09-15)

- C suite excluding benchmarks: 124/124 passed.
- ASan/UBSan: DisplayList, snapshot and scrollbar contract tests passed;
  ghost and replay tests also passed in the earlier sanitizer run.
- Flux and Lens Rust workspaces: tests and doctests passed.
- Windows cross-compilation: changed encoder, snapshot, scrollbar and Iris
  Win32 translation units passed. Native macOS runtime behavior was not tested.
- Full library clang-tidy scan found one encoder dead store; it was removed
  and the encoder then passed a focused scan. No other library findings.
- Generated symbol reference freshness and whitespace checks passed.

These checks cover the implemented contracts above; they are not evidence
for the unimplemented RenderPlan or GPU retirement contracts.
