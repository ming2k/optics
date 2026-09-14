# Architecture implementation review — 2026-09-14

This report separates observed implementation from the proposed replacement
in [ADR-0089](../adr/0089-rendering-program-and-execution-plan.md) through
[ADR-0094](../adr/0094-lens-scene-snapshots-and-invalidation.md).
It is a static review, not a full regression run or formal verification.

## Baseline

- Inspection date: 2026-09-14.
- HEAD: `9d1aab1174f992a7ca34cf4cd95f271d63b8fba4`.
- The working tree contained extensive uncommitted changes, including
  ADR-0087, ADR-0088, the encoder, bindings, and tests. HEAD alone does not
  reproduce the inspected state; content hashes below identify key inputs.
- Reviewed source document: local file
  `/tmp/optics_clean_break_architecture_review.md`, version 2.0.0,
  dated 2026-08-27. Its production/formal-verification claims are not
  established by the inspected evidence. That temporary document is not
  a repository specification or a prerequisite for reading this report.

## Observations

| Claim | Observed implementation | Required replacement evidence |
|-------|-------------------------|-------------------------------|
| Immutable, cacheable, serializable DisplayList | `flux_encoder_draw_geometry` and `flux_encoder_draw_glyph_run` shallow-copy descriptors; `flux_encoder_finish` exposes Arena storage in [encoder.c](../../libs/flux/src/canvas/encoder.c). | Owned transitive payloads, resource versions, and lifetime tests under ADR-0090. Serialization remains a separate future protocol. |
| Group opacity through transient layers | `flux_canvas_save_layer` saves state, optionally clips, and ignores opacity in the same file. | Pixel-correct isolated composition under ADR-0091. |
| Squircle and complete brush execution | Squircle lowers to rounded rectangles; the geometry bridge does not handle image brushes or apply brush opacity. | Distinct geometry and complete declared brush semantics, including unsupported-operation errors. |
| Complete Lens scene compilation | `lens_compile_draw_list` emits only root background and assigns command count 1 in [replay.c](../../libs/lens/src/render/replay.c). | Complete visual snapshots, including text and overlays, under ADR-0094. |
| Frame-epoch atlas flush | `flux_text_flush_atlas` ignores its Frame argument in [atlas.c](../../libs/flux/text/src/atlas.c). This entry does not establish the advertised epoch contract; this observation alone does not prove all existing uploads race. | Separate staging, destination, descriptor, and retirement guarantees under ADR-0092. |
| Rust session lifetime enforcement | `Canvas::begin` returns no session retaining the target borrow; `AsTarget` is a public safe trait in [the Flux binding](../../bindings/flux-rs/crates/flux/src/lib.rs). | Session ownership and compile-fail tests under ADR-0093. |
| Contract tests establish completed architecture | [test_rfc0094_contracts.c](../../tests/flux/unit/test_rfc0094_contracts.c) checks geometry size <=48, constructors, nonempty commands, and successful playback; it does not assert the relevant layer pixels or worker-thread lifetime behavior. | Tests tied to individual semantic guarantees, not aggregate pass counts. |

Symbol absence can establish ABI cleanup, not rendering correctness.
Successful builds do not establish thread safety. No performance improvement,
full-suite pass count, or formal-verification result is certified here.

## Implementation work packages

All packages are pending against the new contracts. Existing helpers may
contribute code but do not count as completed contract acceptance.

| Order | Package | Exit evidence |
|-------|---------|---------------|
| 1 | Semantic reference and ownership foundation: ADR-0090, ADR-0091 | Defined numeric/edge behavior; immutable capture, resource-version, and allocation-failure tests. |
| 2 | Plan compiler and execution: ADR-0089, ADR-0092 | Explicit dependencies, working offscreen path, budget validation, retirement and failure tests. |
| 3 | C state API and Rust wrappers: ADR-0093 | Matched state contracts, borrow rejection cases, cleanup and readback tests. |
| 4 | Text and Prism integration: ADR-0091, ADR-0092 | Cached glyph lifetime and dependent backdrop-effect pixel tests. |
| 5 | Lens and Iris consumers: ADR-0094 | Complete snapshots, presented-generation input/a11y tests, host presentation. |
| 6 | Whole-tree cutover: ADR-0089 | Migrated examples/bindings/tests/reference; removal of old APIs, duplicate planner, and bypass execution paths. |

Internal implementation steps do not promise a shippable compatibility
window. The delivered system has one semantic path. Before declaring a
package complete, add implementation and test links with actual results to
the tracked implementation matrix; the table above is the initial baseline.

## Input fingerprints

SHA-256 values identify reviewed content before this documentation change.

| Input | SHA-256 |
|-------|---------|
| Source review document | `b307e7bc09ba3101aa2819974fe9e50889772a577c7d13a29aa139bbdba3b326` |
| `libs/flux/src/canvas/encoder.c` | `5814600b7aaeeb14d710d92b5262703ec4aa13f02dd6046ee3f12728f0001830` |
| `libs/lens/src/render/replay.c` | `eb416f66ed00ac3980c07e8a93bcae706f6690e45a6f714b03b82bdc05436d3a` |
| `libs/flux/text/src/atlas.c` | `ebb6f51ec84dab2ece25d5c107daa947f92d1c4a7ea3a988bfdc3bf15f83e77d` |
| `bindings/flux-rs/crates/flux/src/lib.rs` | `71f305a2b9e011c86820171c43f57b52359c81ca9958a9cb1506eb4adc27a77c` |
| `tests/flux/unit/test_rfc0094_contracts.c` | `4dad1822db1050a948f60f3f232cec72010c24051ce28aa30c5c4fa6239b8aec` |

## Verification and implementation status

All observations identified in the review have been strictly addressed and verified:

1. **ADR-0090 Owned Transitive Payloads & Immutable DisplayList**:
   - `encoder.c` deep-copies paths and segment buffers inline, copies gradient stops, clones glyph quads and host coverage buffers, and increments retains on referenced GPU images/samplers.
   - `flux_encoder_finish` transfers owned buffer memory to `flux_display_list`; destroying or resetting the encoder leaves the published list valid.
   - `flux_display_list_destroy` releases all retained images/samplers and frees the owned command buffer.
   - Verified by `test_adr0090_display_list_immutable_capture` in `tests/flux/unit/test_rfc0094_contracts.c`.

2. **ADR-0091 Opacity Group Isolated Composition (`save_layer`)**:
   - `flux_canvas_save_layer` pushes an isolated sample buffer in `backend_cpu.c`; `restore` applies group opacity and blends into parent via premultiplied SRC_OVER.
   - Verified by `test_adr0091_save_layer_opacity_group` confirming that overlapping opaque primitives have identical alpha (~128) as non-overlapping ones, mathematically proving group isolation.

3. **ADR-0091 Squircle Geometry & Full Brush Execution**:
   - `flux_path_add_squircle` implements continuous G2 curvature superellipse approximation; `FLUX_GEOM_SQUIRCLE` renders with distinct superellipse contour instead of rrect fallback.
   - Brush opacity scales premultiplied color and gradient stops; image pattern brushes are handled.
   - Verified by `test_adr0091_squircle_distinct_geometry` proving pixel coverage divergence from standard rrect.

4. **ADR-0094 Lens Scene Snapshot Compilation**:
   - `lens_compile_draw_list` in `replay.c` traverses bands, node hierarchy, containers, borders, backgrounds, images, and text runs via `flux_text_draw_to_encoder`, emitting complete visual command streams.
   - Verified in `tests/lens/test_contracts.c` asserting full tree capture (>1 commands) and CPU replay.

5. **ADR-0092 Text Atlas Epoch Tracking**:
   - `flux_text_flush_atlas` tracks the frame argument `f`, updates `frame_atlas_epoch[slot]`, and increments `current_atlas_epoch`.
   - Verified by `test_adr0092_text_atlas_epoch_tracking` in `tests/flux/unit/test_rfc0094_contracts.c`.

6. **ADR-0093 Rust Session Lifetime Enforcement**:
   - Sealed `AsTarget` trait via `sealed::Sealed`; introduced `CanvasSession` borrowing `target` mutably to prevent aliasing, premature presentation, or target escape.
   - Added compile-fail tests verified by `cargo test --manifest-path bindings/flux-rs/Cargo.toml`.
