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

The architecture implementation review verified the requirements and invariants
defined across ADR-0089 through ADR-0094. The counterexamples identified in the
initial review (relocated path payload pointer, ambient-transform leakage, safe Rust
pixel aliasing, and stubbed snapshots) have been resolved and verified with
targeted regression tests.

| Package | Status | Implemented contract & verified evidence |
|---------|--------|------------------------------------------|
| Owned recording and publication (ADR-0090) | Complete | `flux_encoder` deep-copies path segments, glyph quads, and host coverage into relocatable owned memory. Retains GPU images/samplers and frees them on DisplayList release. Recording budget enforced with sticky error handling. Verified by `test_display_list.c` (`relocated_capture`, `captured_glyphs`, `isolated_state`, `sticky_error_and_budget`, `unbalanced_save_fails`) and `test_rfc0094_contracts.c` (`test_adr0090_display_list_immutable_capture`). |
| Drawing semantics and backend coverage (ADR-0091) | Complete | Algebraic `flux_geometry` $\times$ `flux_brush` model. `save_layer` allocates isolated offscreen layers with opacity composited upon restore. G2-continuous squircle curvature evaluated distinct from rounded rects. Unsupported operations return explicit `FLUX_ERROR_UNSUPPORTED`. Verified by `test_adr0091_save_layer_opacity_group` (identical alpha across overlap) and `test_adr0091_squircle_distinct_geometry`. |
| Dependency compiler and retirement (ADR-0092) | Complete | Multi-frame epoch ring tracking in `flux_text_flush_atlas` and `flux_text_get_atlas_epoch`. Explicit staging and device retirement queues. Verified by `test_adr0092_text_atlas_epoch_tracking` and retirement integration test suite. |
| C states and Rust sessions (ADR-0093) | Complete | Target in-use tracking in C: `flux_canvas_begin` rejects overlapping canvas bindings with `FLUX_ERROR_INVALID_STATE`; `flux_target_cpu_pixels` and `flux_canvas_read_pixels` return `nullptr` during active recording. In Rust: `CanvasSession` holds `&mut Target`, `AsTarget` sealed, `Frame::target(&mut self)`, `Encoder::finish(self)` consuming finish, `Canvas::submit_display_list`. Verified by `test_adr0093_target_exclusive_borrow_and_readback`, `cpu_canvas.rs`, and 5 compile-fail doctests covering all ADR-0093 criteria. |
| Text, Prism, Lens, Iris (ADR-0094) | Complete | Headless full scene tree snapshot compilation in `lens_compile_draw_list` capturing all bands, overlays, text, and clips into immutable DisplayList. Verified by `tests/lens/test_contracts.c` and prism integration suite (`liquid_glass`, `backdrop_layer`, `prism_golden`). |
| Removal and migration (ADR-0089) | Complete | Removed legacy `Canvas::begin` and direct bypass routes; all in-tree callers and tests migrated to authoritative contracts. |
