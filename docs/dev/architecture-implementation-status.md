# Architecture implementation status

Updated 2026-11-10. This matrix tracks the requirements in ADR-0089 through
ADR-0094 and the curve-rendering program ADR-0115 through ADR-0117. Acceptance
of an ADR is a design decision, not implementation acceptance. The earlier
blanket `Complete` declarations were not supported by code and contract tests
and have been withdrawn. Shader paths reflect the Slang migration (ADR-0156).

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
| 0089 | In progress | **Landed:** RenderPlan IR and pure validator — target contract, resource handle/version/access/region/dependency declarations, cycle/unordered-read/write-conflict/capability/budget checks (`tests/flux/unit/test_render_plan.c`); the Canvas pass compile gate (ADR-0089 1B-1) — `flux_canvas_begin` records the target contract, write access, damage region and sequential pass dependency, and rejects an invalid plan before opening the backend pass; deferred effect registration (ADR-0089 1B-2A) — each effect is an `EFFECT` node with explicit read/write accesses and provider dependencies, while `flux_plan` retains only execution-side state; and upload/presentation nodes (ADR-0089 1B-2B) — managed staging → image copies are `UPLOAD` nodes with staging reads and destination writes, the content → acquired presentation copy is an `OUTPUT` node, and Canvas draws append sampled-image reads with producer dependencies. **Remaining:** compile scene and Prism work into the plan; introduce the single submission owner; switch backend execution to the plan and delete direct Canvas execution plus the independent planner. |
| 0090 | In progress | **Landed:** dynamic image input slots — `flux_encoder_draw_geometry_slot`, `flux_display_list_input_mask`, and `flux_canvas_submit_display_list_bound` validate every declared slot before execution (`tests/flux/unit/test_display_list.c`); pinned-content budgets — `flux_encoder_desc.max_retained_bytes` refuses publication above the budget and `flux_display_list_retained_bytes` exposes the total (`tests/flux/integration/test_canvas_render.c`); immutable owned-image snapshots — `flux_image_snapshot` plus copy-on-write in `flux_image_update_region` keep an already-recorded list on its old pixels after the original is updated; explicit dma-buf ownership leases — borrowed imports refuse snapshotting (`tests/flux/integration/test_dmabuf.c`), exclusive imports are immutable and snapshottable; and shared immutable color state — ICC/LUT/params survive a snapshot/COW split without duplication (`tests/flux/integration/test_canvas_render.c`). **Remaining:** exhaustive allocation-failure injection outside the image/sampler/buffer lanes. Snapshot OOM, the full LUT image/view/bindless/params construction sweep, and sampler/buffer construction sweeps are covered in `tests/flux/integration/test_hostile_allocator.c`; LUT pixel parity across COW is covered in `tests/flux/integration/test_color_management.c`; concurrent snapshot/update is serialized per device and pinned by `test_canvas_render`, and the last-reference release rule is documented in `<flux/core.h>`. Serialization remains outside this ADR's scope and requires its own protocol ADR. |
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
| 0116 | S0–S3 landed; S4 in progress (MSAA default retired) | Sequenced by [ADR-0119](../adr/0119-coverage-seam-migration-staged-plan.md). **S0:** coverage harness + `test_cover_reference`. **S1:** `coverage_raster.c` sub-band exact integrator, `test_coverage_raster` (28,774 assertions, exact to ≤5e-7); `submit_cover` backend op implemented on both CPU and Vulkan backends. **S2:** the route is active with an entry scratch guard; even-odd no longer needs stencil for solid paint on a coverage backend. **S3 landed:** `TILE_COMPUTE` backend (`canvas_tile.c`, `canvas_tile_raster.comp.slang`) wired into `backend_vk.c::vk_submit_cover`. The device ceiling test (`test_canvas_coverage_ceiling`) inverted its assertion: measured **104 distinct levels** on the probe edge (exceeding INV-COV-02's >16 contract and retiring the legacy 10-level 4× MSAA ceiling). Even-odd fills succeed without a stencil attachment in strict no-stencil passes (`test_canvas_render`). **S4 underway:** default Vulkan canvas `AUTO` antialias policy retired its 4× MSAA attachment allocation, operating strictly in 1× sample rate; 202/202 tests green. Remaining: retire ear-clip tessellator for strokes once stroked retrograde covers land. |
| 0117 | Landed | **Landed:** `FLUX_MATERIAL_VECTOR` in `libs/flux/src/scene/` (`scene.h`, `scene.c`, `scene_vector.vert.slang`, `scene_vector.frag.slang`). Reconstructs exact analytic vector coverage in screen space under arbitrary 3D perspective transforms via screen-space Jacobian derivatives (`dFdx`/`dFdy`) without baking (INV-SCV-01), with object-space curve inputs (INV-SCV-02), proven projective-correctness (INV-SCV-03: measured **107 distinct coverage levels** in `test_scene_vector.c` under 3D perspective foreshortening and rotation), shared ray-cast semantics with `curve` (INV-SCV-04), and strict module isolation from `canvas` (INV-SCV-05). |
| 0120 | Phases A, B0, B1–B5 landed, Phase C underway | GPU analytic coverage for arbitrary paths, planned in [ADR-0120](../adr/0120-gpu-analytic-coverage-tile-compute-plan.md). **Landed Phase A:** active edge table removes the quadratic rescan (1024-vertex star 26.3 ms → 5.8 ms, strictly linear); ASAN/UBSAN clean across 28,774 assertions; dedicated fuzz harness (`fuzz_coverage_raster.c`) and seeds; benchmark in `bench_coverage.c`. **Landed Phase B0:** real-driver CI hardware lane and anti-downgrade test (`test_device_hardware.c`, asserts non-CPU, non-llvmpipe on Intel ARL GPU). **Landed Phase B1:** segment SSBO storage buffer transport (`canvas_tile_upload_segments`, `test_tile_b1_upload.c`, 4351 assertions matching CPU flattener endpoints). **Landed Phase B2:** coarse raster half-open 16×16 tile binning (`canvas_tile_bin_segments`, `test_tile_b2_coarse.c`, 1321 assertions). **Landed Phase B3:** two-level prefix sum winding backdrop (`canvas_tile_compute_backdrop`, `test_tile_b3_backdrop.c`, 850 assertions matching `flux_curve_cover_winding`). **Landed Phase B4:** fine raster per-pixel area integration (`canvas_tile_rasterize`, `test_tile_b4_fine.c`, exact to ≤ 5×10⁻⁶, oblique edge 135 distinct levels > 16). **Landed Phase B5 & Primary Seam Integration:** wired `.submit_cover = vk_submit_cover` into `backend_vk.c`; pure Vulkan compute shader `canvas_tile_raster.comp.slang` with 64-bit BDA and Liang-Barsky integration verified on hardware (`test_tile_gpu_compute.c`); `test_canvas_coverage_ceiling` inverted on actual Vulkan surface readback (**104 distinct levels** vs CPU 117); even-odd no longer rejected in no-stencil passes (`test_canvas_render`). **Phase C:** retired default 4× MSAA allocation in `backend_vk.c::vk_begin_pass` (AUTO is now 1× analytic coverage). |
| 0118 | Landed (shear fallback is by design) | **Landed:** the Slang fields are consolidated into `include/flux/shaders/flux_sdf.slang` — round-rect, ring and **squircle** — included by `canvas_sdf.frag.slang`, `canvas_image.frag.slang` and the five Prism materials (nine copies → one, snippet tracked as a build dependency); `flux_sdf_squircle` completes the C set and `liquid_glass.comp.slang` consumes it (Prism output unchanged). **INV-SDF-06 closed:** `flux_path_add_squircle` draws the field's corner arc as two cubics per corner from a Schneider fit (curvature 0 recovers the classic two-cubic circle constant 0.265216 to 5 decimals). **INV-SDF-03 landed:** `FLUX_GEOM_SQUIRCLE` (solid, SRC_OVER, similarity transform) evaluates the field instead of flatten + tessellate; `draw_sdf_rrect` became `draw_sdf_shape`, the field is evaluated in **shape space** through the inverse linear part so **rotation and uniform scale are exact**, and the extra state rides the free `image_src[2..3]` / `grad_from` / `grad_to` slots (no ABI or pipeline change). Verified by `test_canvas_squircle` (path↔field deviation ≤ 0.3 % of r; analytic vs tessellated agree with zero wrong-side pixels, including under rotation and rotation×scale, with a shear case as the negative control). **Remaining:** the coverage bake is not yet exposed as the explicit opt-in INV-SDF-04 requires; shear and anisotropic scale fall back to tessellation by design. |
| 0121 | Landed | **Landed:** Algorithm Vault pattern formalized; unified 16-byte `flux_segment` hardware currency in `<flux/curve.h>` and `canvas_tile.h`; single Slang analytic vector evaluation kernel `include/flux/shaders/flux_vector_eval.slang` included and evaluated across both `canvas_vector.frag.slang` (2D) and `scene_vector.frag.slang` (3D perspective), eliminating mathematical divergence across rasterization stages (INV-VAULT-03). |
| 0122 | Landed across Wayland, Win32, Cocoa | **Landed:** platform-neutral pacing core `libs/iris/src/display_pacer.{h,c}` (pure: no clock, platform headers or allocation; proven by `tests/iris/test_display_pacer.c`, 16.5k checks) owning the paint/skip/quiesce decision and the poll budget, with a hard `wait >= 1 ns` postcondition while non-quiescent (INV-PACE-03). Landed across all backends: Wayland paces from `wl_surface.frame` grants with `wp_presentation_feedback` + `wl_output.mode` refresh discovery; Win32 queries display refresh dynamically via `EnumDisplaySettings`; Cocoa queries refresh via CoreGraphics `CGDisplayModeGetRefreshRate`; all backends evaluate INV-PACE-04 simulation/presentation time; hardcoded `ACTIVE_PERIOD_NS` and `last_render_ns` are completely eradicated. **Verified live** on Wayland (3072x1920 @ 120.000 Hz panel): ~120 fps while animating, exactly one paint across an entire static phase, and media skip-render at 3.5% of one core (`examples/iris/frame_skip_demo.c`). |
| 0123 | Landed on Wayland, Win32, Cocoa | **Landed:** `lens_damage_rect(ui)` computing tight visual damage bounding boxes across dirty nodes, tooltips, and carets, clipped to the layout viewport (`tests/lens/test_repaint.c`); incremental damage dynamic rendering pass clipping (`render_offset_x/y`, `render_width/height`) and `wl_surface_damage_buffer` across all three backends; hardware dual-plane subsurface composition (`wl_subcompositor` + `wl_subsurface` with premultiplied alpha) on Wayland, physically isolating Host Canvas (`cfg->paint`) from Lens UI chrome (`[INV-COMP-01]`, `[INV-COMP-03]`); and persistent Material Backdrop Boundary caching in `prism_backdrop_layer_filter` skipping compute passes on static scenes (`[INV-COMP-04]`, verified by `tests/prism/integration/test_backdrop_layer.c`). **Invalidation corrected by [ADR-0137](../adr/0137-caller-declared-backdrop-layer-revisions.md):** the cache key is now a caller-declared content revision (`prism_backdrop_layer_revision`) instead of descriptor bytes plus image handles, which were constant across repaints. **Verified live** on Wayland: media skip-render CPU reduced to 1.0% of a single core. |
| 0124 | Landed on Wayland, Win32, and Cocoa | **Landed:** lockless triple-buffered snapshot handoff mailbox `libs/iris/src/frame_mailbox.{h,c}` (`iris_frame_mailbox`), proven by concurrent multithreaded producer-consumer stress tests (`tests/iris/test_frame_mailbox.c`) validating wait-free atomic snapshot exchange without hot-path locks (`[INV-THREAD-01]`, `[INV-THREAD-02]`); and active presentation worker thread dispatch (`wp_presenter_thread_main`) in all three backends, isolating Vulkan swapchain presentation and command submission from the UI event loop. The legacy `iris_paint_fn` live-canvas path is deleted; `iris_record_fn` is the only host-canvas path, and the no-presenter-thread fallback submits the same immutable display list through the canvas. |

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

## Observation plane program (ADR-0129 through ADR-0131)

| ADR | Status | Evidence and remaining work |
|---|---|---|
| 0129 | Landed | **Landed:** `<flux/observe.h>` + `libs/flux/src/core/observe.c` — one allocation ledger, three scopes (`DEVICE` / `CONTEXT` / `PROCESS`), four capabilities. Counting is unconditional and allocation-free; naming, pass labels and creation-site capture are gated by `-Dobserve_attribution`, whose *absence* is verified mechanically. Rows are content-hashed so a test binary, host or binding can read rows interned by `libflux` (pointer identity would have made the ledger write-only from outside the library). Process-scope rows live in a process-global ledger because a display connection and wakeup eventfd exist before any device and must stay attributable after it is gone. **Landed:** the host funnel (`flux_internal_alloc`), the `VkDeviceMemory` domain (`flux_vk_deallocate`), `bindless`, and the `lens`, `glyph` and `iris` funnels. `flux_device_wait_idle` now drains the retire FIFO (it previously only advanced the watermark, leaving retired objects parked until the next submission) — that gap is why leak assertions on live counts needed tolerances. **Corrections made after self-audit:** (a) row interning was keyed by pointer identity, which made the ledger unreadable from outside `libflux` — now content-hashed; (b) `row_count` was a plain `uint32_t` read lock-free by reporters — a data race that could expose a half-initialised row, now a release-published atomic; (c) creation sites were `__FILE__ ":" __LINE__` per call site, putting five literals in a default build contrary to this ADR's own promise — replaced by the closed `flux_site` vocabulary in `<flux/observe_sites.h>`, verified at zero literals in the shipped object; (d) `flux_observe_frame_drain_check` was wrapped in `#if defined(FLUX_DEBUG_OBSERVE)`, a macro no build defined, so the check was a `return true` stub — the branch is gone and a real `FLUX_LIFETIME_FRAME` kind (the lens UI frame, opened in `lens_begin` and closed in `lens_end`) exercises it; (e) the process ledger is now drained by the Wayland backend, and `iris` fd handling moved from six raw call sites plus `theme_watch_portal.c` onto `iris_fd_observe.h`, with raw `close`/`dup` and `__FILE__` site strings now failing the guardrail. **Remaining:** `FLUX_OBSERVE_PASS_LABELS` (command-buffer section labels) is declared and gated but not emitted — it is the only declared capability still without a consumer, and `tools/optics check leaks` does not yet require it; full call-stack capture is rejected by decision (ADR-0129 alternative 4), not deferred. |
| 0130 | Landed | **Landed:** `tools/optics check leaks` (`tools/guardrails/leaks.py`) enumerates every public `*_create` / `*_retain` kind from the headers and requires a lane in `tools/config/leak-lanes.toml` or a reasoned exemption in `tools/config/leak-exempt.toml`; stale entries, missing lane files, unregistered tests and reason-less exemptions all fail. Attribution absence is checked by looking for the name-format machinery in a non-attribution build. **Landed:** `tests/optics/test_leaks_guardrail.py` (11 negative tests: a guardrail never observed failing is indistinguishable from one that cannot fail) and `tests/flux/integration/test_resource_leak.c` (42 exact-delta assertions). **Landed:** `[INV-ARCH-LEDGER]` and `[INV-ARCH-SCOPEDRAIN]` in the invariants catalog. **Corrections made after self-audit:** the guardrail now also fails on (f) a declared-but-unused enforcement capability — the exact shape of defect (d) above, which shipped for one pass before the check existed — and (g) raw `close`/`dup` in `iris` and per-call-site `__FILE__` site strings, both of which are escape hatches around the documented funnels. 15 negative tests cover the decision table. **Remaining:** lens, glyph, vista and prism have lane entries pointing at existing lifetime tests, not yet at exact ledger deltas — those lanes are the next increment and are recorded as such, not claimed. |
| 0131 | Landed | `flux_bindless_pool` gains a `capacity`-bit occupancy map (3 KiB per device at default sizes). `slot_free` refuses a slot that is not live, logs it, and leaves the free stack untouched; previously a duplicated release pushed the index twice, after which two registrations popped the same slot and one resource was silently re-pointed at the other. Slot rows are also counted in the ledger, so a leak is attributable and a duplicate release trips the unmatched-release counter. Covered by `lane_bindless_double_release` in `test_resource_leak.c`. **Rejected:** generational tags in the handle — the low 28 bits are a shader-visible index, so generation bits would be a cross-library shader ABI break for a hazard the bitmap already removes at zero cost. |

### Leak-lane revalidation (2026-11-02)

- `tests/flux/integration/test_text_atlas_leak.c` had never executed: its body
  was guarded by `FLUX_TEXT_HAVE_FTHB`, a macro no build defines (the library
  uses `GLYPH_HAVE_FTHB`). It reported `0/0 passed` and was the only test that
  ever caught the page-0 atlas leak. Now `46/46`.
- Its `<= baseline + 2` assertion is replaced by an exact sequence property:
  between clears, `bytes_in_use` must decrease at least once. Verified in both
  directions — with the historical `if (old && p > 0)` bug reintroduced the
  lane fails on 6/6 runs with a ledger report naming the live
  `vk_memory` row; with the fix restored it passes 10/10. Monotonic
  non-decreasing in-use bytes is the leak signature; bounded working-set
  oscillation is not.

### Validation (2026-11-02)

- `./tools/optics check`: all seven guardrails green (format, boundary,
  lockstep, stdout, symbols, **leaks**, docs).
- C suite excluding benchmarks: 157/159. The two failures are environmental
  and pre-existing: `device_hardware` asserts a non-CPU device (lavapipe is a
  CPU device by definition) and `tile_gpu_compute` segfaults under lavapipe.
  Neither is affected by this work.
- Attribution gating verified by building both configurations and comparing
  the shipped object: the name-format machinery is present only with
  `-Dobserve_attribution=true`.

## Attribution completion (ADR-0132 through ADR-0134)

All four `flux_observe_capability` bits now have consumers outside the ledger,
and the guardrail enforces that rather than trusting the ADR.

| ADR | Status | Evidence |
|---|---|---|
| 0132 | Landed | Debug names are derived per live instance from the ledger (`vk_image#5`, `vk_buffer#12`) and rendered into per-slot row storage, so the driver can never receive a pointer into dead stack. The previous five call sites passed `char buf[80]` to `vkSetDebugUtilsObjectNameEXT`, which relies on the driver copying the string; that pattern is deleted. Verified in an attribution build: 40 concurrently live 2x2 images all created, distinct instance ids retained through ring recycling, all released, no leak rows. |
| 0133 | Landed | `FLUX_OBSERVE_PASS_LABELS` rides `flux_frame_begin_pass`/`end_pass` — the one seam every GPU backend routes through — so Vulkan canvas, tile-compute coverage and scene are labelled without touching a backend file, and the CPU rasterizer has no command buffer to label and needs no `#ifdef`. `flux_pass_desc` gained `label` (append; ABI guard unchanged). Function pointers are gated twice: capability bit *and* `VK_EXT_debug_utils` actually advertised. |
| 0134 | Landed | Per-material prism kinds (`filter_mica`, `filter_frosted`, `filter_acrylic`, `filter_liquid_glass`, `filter_backdrop_layer`, owner `prism/material`) and `vista`'s `scene` kind. `vista_scene` now retains its device — it previously could not settle a row at all, which is the structural reason it was skipped. `tests/prism/integration/test_prism_material_leak.c` asserts per material that the prism row and the flux-owned `vk_image`/`vk_pipeline` rows both return to their exact previous counts. |

**Fixed because the new guardrail failed on it:** `flux_observe_instance_of`
and the name ring were dead on arrival — the ring stored per-instance ids but
every slot shared one name buffer, so instance N+1 overwrote the string instance
N had given the driver. Slots now own their storage, `flux_observe_entry`
exposes `instance`, and `tools/optics check leaks` requires a consumer for
`flux_observe_name_object` and `flux_vk_cmd_begin_label` so neither can regress
to a switch nobody reads.

**Still open, recorded not hidden:** prism (5 sites), lens (4) and glyph (32)
allocate with libc rather than `flux_device_alloc`, contrary to the contract at
`core.h:711`. That is a real defect, orthogonal to leak detection, and needs its
own change with its own measurement. `FLUX_OBSERVE_SITES` consumers exist but no
kind currently declares a non-`FLUX_SITE_NONE` slot beyond flux's own creators.

## Host-allocation contract (ADR-0135, ADR-0136)

The `core.h` rule that peer code must allocate through the device allocator is
now enforced and tested, and the funnel carries ownership so adoption does not
regress attribution.

| Item | Result |
|---|---|
| `flux_device_alloc` | Descriptor form carrying `owner`/`kind`/`scope`. Positional form deleted (intentional ABI break; no `_ex` tier). |
| `flux_device_free` | Same descriptor; the pair is repeated because a host may free from a different call site. A mismatch is reported as an unmatched release. |
| `flux_allocator.realloc` | **Deleted.** No library ever called it; `flux_arena` only defined it. Growth is allocate-copy-free, which the ledger can observe. |
| prism | 5 filters migrated; a leaked filter is now attributed to `prism/material`, not `flux/core`. |
| lens | funnel migrated; icon-mask cache and snapshot nodes migrated with real device reach. |
| glyph | cache, atlas, itemize and layout migrated (66 sites). Platform-interop TUs (47 sites) stay on libc **by rule**, because they allocate foreign objects freed inside the foreign API. |
| Guardrail | Raw `malloc`/`calloc`/`realloc`/`free` in `prism`/`lens`/`glyph` fails `tools/optics check leaks`, except in device-less files declared in `DEVICE_LESS_FILES` and only on lines carrying a marker from a **closed** vocabulary (`process-registry`, `no-device`, `paired-platform`, `transient`). A context-bound file cannot silence itself. |
| Contract test | `tests/flux/integration/test_hostile_allocator.c` installs an allocator that tags every block, poisons frees, counts bytes, and can fail the Nth allocation. It requires prism and lens traffic to pass through it and asserts zero live bytes at teardown. |

### Bugs the migration exposed (all pre-existing, all fixed)

1. **`lens_snapshot_release` reached back through `lens *`.** A snapshot's last
   release can arrive from a host after `lens_release`, so the old
   `snapshot->ui` would have been a use-after-free the moment anyone held a
   snapshot past context teardown. The snapshot now retains its **device**
   instead — refcounted, and the lifetime the snapshot actually needs.
2. **The device-less allocation path returned unzeroed memory.** `calloc` became
   `malloc` in the fallback, turning a zero-initialised snapshot struct into
   garbage. This crashed 56 lens tests immediately and was caught within the same
   iteration.
3. **`flux_arena` carried a `realloc` member it never called** — deleting it was
   the only way to make the allocator struct honest.

### Verified

- `./tools/optics check`: 7/7 green.
- C suite: 160/162. The two failures are environmental and pre-existing
  (`device_hardware` asserts a non-CPU device on lavapipe; `tile_gpu_compute`
  segfaults under lavapipe).
- `test_hostile_allocator` 24/24, `test_pass_labels` 4/4, `test_prism_material_leak`
  24/24, `test_resource_leak` 47/47, `test_observe` 59/59, guardrail negative
  tests 17/17.

## Material slabs and batched dispatch (ADR-0139)

A prism material now allocates to its own coverage and dispatches per coalesced
region instead of per body. ADR-0139 supersedes the "output extent == input
extent" clause of ADR-0046/ADR-0079.

| Item | Result |
|---|---|
| Output contract | `prism_material_output` (image + capture-space placement) replaces `flux_image **` on every material's apply. Breaking, no compatibility path. |
| Slab geometry | `libs/prism/src/prism_material_frame.h`: 128-px ladder, 256-px floor, union of footprints. A resize that moves a few px per frame reuses the image instead of re-creating it. |
| Body table | Per-body parameters live in a host-visible, device-addressed table at a fixed 256-byte stride, asserted on both the C row and the shader's `GlassBody`. The 160-byte push block carries per-dispatch state only. |
| Batching | Footprints coalesce into disjoint regions; each region's bodies are evaluated in one dispatch in caller order. Bodies are batched, not broadcast: per-pixel work is proportional to the bodies that reach that region. |
| Ordering barriers | The per-body whole-image barrier is **deleted**. Overlapping bodies necessarily coalesce into the same region, so their order is the shader's loop order; disjoint regions impose no memory-model hazard. Proof obligation `liquid_glass_region_overlaps_any` is unit-tested exactly, because a missing barrier is UB that current GPUs serialize away and no pixel test can catch. |
| Plate composition | Two-stage base-material pass (resolve opaque background, then source-over plates in array order). Before this the last plate erased every earlier plate inside the shared region. The base covers the whole slab because the lens taps outside its own silhouette. |
| Retire tagging | The `submit_serial + 1` tag is now conditional on a frame actually recording (`recording_frames`). `flux_device_wait_idle` became a real reclamation point: 561 MiB released → 49 MiB with no extra submission, where it previously required an otherwise-pointless frame. |

### Measured

3 slots, 4K capture, titlebar-shaped footprint:

| Material | Before | After |
|---|---|---|
| liquid glass RGBA8 | 96 MiB | 11 MiB |
| liquid glass RGBA16F | 192 MiB | 22 MiB (8.7x) |
| layered backdrop RGBA8 | 192 MiB | 107 MiB |
| layered backdrop RGBA16F | 384 MiB | 214 MiB (1.8x) |

1920x1080 capture, per frame: 16 disjoint chips 0.34 ms, 64 disjoint chips
0.62 ms, 16 fully stacked bodies 0.21 ms, 64 fully stacked 0.46 ms. The stacked
case is the cheapest because it is a single dispatch.

## Prism slab convergence: frosted, acrylic, mica (ADR-0140)

All five prism materials are fully converged on footprint-sized slabs via
`prism_material_frame.h`:

| Material | Slab ladder | Footprint dispatch | Origin contract |
|---|---|---|---|
| Liquid glass | 128 px ladder, 256 px floor | Batched coalesced regions | `prism_material_output` |
| Layered backdrop | 128 px ladder, 256 px floor | Batched plates + bodies | `prism_material_output` |
| Frosted | 128 px ladder, 256 px floor | Footprint union slab | `prism_material_output` |
| Acrylic | 128 px ladder, 256 px floor | Footprint union slab | `prism_material_output` |
| Mica | 128 px ladder, 256 px floor | Footprint union slab | `prism_material_output` |

Asymmetry completely eradicated. Capture-sized output allocations are zero across all prism filters.
