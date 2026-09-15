# Architecture implementation status

Updated 2026-09-15. This matrix tracks the requirements in ADR-0089 through
ADR-0094. Acceptance of an ADR is a design decision, not implementation
acceptance. The earlier blanket `Complete` declarations were not supported by
code and contract tests and have been withdrawn.

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
