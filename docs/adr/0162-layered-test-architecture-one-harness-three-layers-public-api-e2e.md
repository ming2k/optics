---
id: ADR-0162
title: "Layered Test Architecture: One Harness, Three Layers, Public-API E2E"
status: accepted
date: 2026-11-14
scope: tests/, tools/check_test_layers.py, docs/dev/testing.md, .github/workflows/
superseded_by: null
negative_knowledge: true
---

# ADR-0162: Layered Test Architecture: One Harness, Three Layers, Public-API E2E

- Status: Accepted
- Date: 2026-11-14
- Scope: `tests/`, `tools/check_test_layers.py`, `docs/dev/testing.md`, `.github/workflows/`
- Amends: [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md), [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md), [ADR-0130](0130-leak-coverage-mechanical-gate.md), [ADR-0157](0157-display-list-serialization-protocol.md), [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md)

## Context

The tree had a large body of C tests but no test *architecture*. Four symptoms
were structural, not cosmetic:

1. **No layer taxonomy.** Meson introspection reported 186 tests: 38 `unit`,
   56 `integration`, 8 `bench`, 1 `lint`, and **83 with no layer at all**.
   Seventy-three carried only the implicit project suite (all of `lens`, `iris`
   and `transit`); `reticle` (6) and `vista` (2) invented ad-hoc suites, the
   façade test used `optics`, and the tooling selftest declared no layer. "Run the unit suite" did not mean "run the CPU-safe
   tests"; it meant "run the subset of `flux` tests someone had labelled". The
   default `meson test` mixed GPU tests, CPU tests, benchmarks, and tooling into
   one undifferentiated run.
2. **Five private assertion harnesses.** `tests/flux/test_helpers.h`,
   `tests/lens/test_helpers.h`, `tests/iris/test_helpers.h`,
   `tests/reticle/test_helpers.h`, and `tests/transit/test_helpers.h` each
   defined `CHECK`, `TEST_REPORT`, or `EXPECT` with subtly different semantics:
   some counted per translation unit, some linked process-global counters, some
   wrote stdout on success, and some returned from the caller. The prism and
   vista suites consumed the flux copy, inheriting its counters and its Vulkan
   include path. A test's reporting behaviour depended on which directory it
   lived in.
3. **No E2E layer.** Every test stopped at a library boundary. The highest
   existing `lens` tests rendered a snapshot into a CPU canvas, but no test
   proved the public application path: document input -> UI tree -> immutable
   snapshot -> serialized `DisplayList` -> pixels. `optics_facade` checked
   version macros and a transit spring; it did not run a frame.
4. **No mechanical enforcement.** Nothing prevented a new test from omitting a
   suite, a new private `test_helpers.h` from appearing, or an "E2E" test from
   including `libs/lens/src/internal.h` and compiling library sources into
   itself. Conventions without a gate rot.

The project's own invariants already require mechanical enforcement
([ADR-0130](0130-leak-coverage-mechanical-gate.md)); the test system was
exempt from the rule it applies to production code.

## Decision

We adopt one layered test architecture and delete the previous conventions
without compatibility shims.

1. **Three layers plus orthogonal lanes.**
   - `unit`: one library, deterministic, CPU-only, no Vulkan ICD, no
     compositor, no subprocess. May compile a hidden implementation TU into the
     test only when the symbol is deliberately not exported; that does not make
     it an integration test because no external boundary is crossed.
   - `integration`: a real boundary — Vulkan device, font stack, multi-library
     seam, filesystem/process boundary. Environment-dependent lanes must skip
     with Meson's exit code 77, never pass vacuously.
   - `e2e`: public umbrella API to observable output. No private headers, no
     compiled-in library sources, no compositor or GPU. The E2E suite is the
     smallest layer by design.
   - `bench`, `fuzz`, `lint`, `tooling` are orthogonal lanes, not layers. A test
     declares exactly one layer or one lane; lint/tooling tests are classified
     separately because they check the tree, not a runtime behaviour.

2. **One harness, one fixture layer.**
   - `tests/support/optics_test.{h,c}` is the only assertion/reporting
     implementation: `OT_EXPECT`, `OT_EXPECT_NEAR`, `OT_EXPECT_STR_EQ`,
     `OT_EXPECT_MEM_EQ`, `OT_REQUIRE`, `OT_SKIP`, `OT_SKIP_IF`, and
     `OT_REPORT()`.
   - Diagnostics go to `stderr`. A skip is exit code 77. A fatal precondition
     ends the test process; one binary is one test case, so this is the case
     boundary.
   - `tests/support/flux_test.{h,c}` contains Vulkan device fixtures only.
     Assertion accounting does not depend on Vulkan headers, and a CPU-only test
     does not inherit a Vulkan link line to reach an assertion macro.
   - `tests/support/lens_test.h` contains the headless lens frame protocol
     (`lens_test_end`, `lens_test_snapshot_render`), not assertions.
   - Every per-suite `test_helpers.h` and `test_helpers.c` is deleted. No
     `CHECK`/`TEST_REPORT` compatibility aliases remain.

3. **Topology is owned by `tests/meson.build`.**
   The root build calls `subdir('tests')`; that orchestrator builds
   `tests/support` first and then each library suite. Adding a new library test
   directory is a change in one topology file, not in the root Meson file.

4. **E2E is a public-API contract.**
   `tests/e2e/` contains only scenarios that link `optics_dep` (the umbrella
   façade and its public libraries) and the shared harness. The current
   scenarios are:
   - `e2e_flux_version`: umbrella version and flux runtime version in every
     feature configuration, including a flux-only distribution.
   - `e2e_public_api`: umbrella version lockstep and a complete public frame.
   - `e2e_interaction_session`: synthetic host loop across pointer, focus,
     keyboard text, accessibility export, snapshot publication, and CPU replay.
   - `e2e_document_pipeline`: SVG document -> reticle raster, SVG -> runtime
     icon -> lens snapshot -> immutable `DisplayList` -> serialized bytes ->
     deserialized list -> byte-identical CPU pixels.

   These are not allowed to include `internal.h`, `libs/*/src`, or compile a
   library source file into the test binary. A test needing either belongs in
   `unit` or `integration`.

5. **Mechanical layer governance.**
   `tools/check_test_layers.py` introspects every registered test and fails
   when:
   - a test has no layer/lane/meta classification or declares more than one
     layer;
   - a test mixes a layer with a bench/fuzz lane;
   - a test name is registered more than once;
   - the `unit` or `integration` or `e2e` layer is empty;
   - the pyramid is inverted (`unit < e2e`);
   - a private `tests/*/test_helpers.h` or `.c` reappears;
   - a legacy `CHECK`/`EXPECT`/`TEST_REPORT`/`TEST_SUMMARY` token reappears
     outside the shared harness;
   - a source under a `unit/` directory creates a Vulkan device;
   - an E2E source includes a private header or the E2E Meson file compiles a
     library source;
   - an E2E source is not registered in `tests/e2e/meson.build`, or the
     E2E Meson file does not link `optics_dep`.

   The script runs as a `lint`/`tooling` test and in CI, so the taxonomy cannot
   silently drift.

6. **CI runs the layers as explicit gates.**
   The Linux jobs run `unit + e2e + lint + tooling` as the headless gate and
   `integration` as the device gate. Cross-platform jobs run the headless gate
   without the GPU suite. Bench and fuzz remain dedicated jobs. This makes a
   broken E2E path fail its own named gate instead of being buried in a
   186-test aggregate.

## Alternatives Considered

- **Adopt an external C test framework (Unity, CMocka, criterion).** Rejected.
  The tests need precise control over process isolation, Vulkan fixtures,
  sanitizer environments, Meson skip semantics, and Windows/macOS cross-builds.
  A small in-tree harness is less machinery than integrating a framework that
  would still need a custom fixture layer.
- **Run everything through pytest or CTest.** Rejected. Meson already owns the
  build graph, sanitizer setup, shader custom targets, and per-test environment.
  Adding a second orchestrator would duplicate the graph and hide which layer a
  failure belongs to.
- **Keep per-suite helpers and only add suite labels.** Rejected. Labels solve
  reporting; they do not solve five divergent assertion semantics. The
  duplication is the rot.
- **Treat the existing `lens` GPU-free tests as E2E.** Rejected. They use
  private state and sometimes compile library internals. E2E is a public
  contract; it cannot be awarded by renaming a unit test.
- **Drive E2E through the examples and parse their stdout.** Rejected. Examples
  are interactive demonstrations with their own output contracts. A process
  wrapper would test stdout formatting, not the library pipeline, and would
  make E2E depend on the `examples` build option.
- **Provide `CHECK`/`EXPECT` aliases for an incremental migration.** Rejected
  per the zero-baggage mandate. The migration is mechanical and complete; an
  alias layer would preserve exactly the inconsistency this ADR removes.

## Consequences

- Every registered test now has a layer or lane. The current inventory is
  `unit=112`, `integration=66`, `e2e=4`, `bench=8`, plus lint/tooling.
  `glyph` has a dedicated suite: pure cache/lifecycle tests are `unit`, while
  shaping-dependent caret/selection and text-soak lanes are `integration`.
- `meson test --suite unit`, `--suite integration`, and `--suite e2e` are
  meaningful, portable commands. Cross-platform CI can run the headless layers
  without filtering by directory name.
- Environment-bound tests report Meson `SKIP` instead of a silent pass. A
  missing Vulkan ICD or validation layer is visible in the summary.
- The E2E suite exercises the public façade, the display-list serialization
  protocol, and the CPU raster path together. A change that breaks only their
  composition — the class of defect unit tests miss — now fails a named gate.
- `tests/flux` no longer owns assertion counters. The Vulkan fixture library and
  the assertion library can be linked independently.
- New tests must choose a layer. The guardrail rejects an unlabelled test, so
  “where does this test go?” is answered before the test is accepted.
- The test topology file and the guardrail add two small pieces of
  infrastructure. They replace five duplicated headers and a 73-test blind
  spot.

## References

- [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md): canvas backend seam and the headless CPU backend
- [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md): immutable snapshot and generation handshake
- [ADR-0129](0129-observation-plane-and-resource-ledger.md): observation plane and resource ledger
- [ADR-0130](0130-leak-coverage-mechanical-gate.md): mechanical leak-coverage gate
- [ADR-0157](0157-display-list-serialization-protocol.md): DisplayList serialization protocol
- [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md): unified optics façade
- [ADR-0161](0161-reticle-vector-compiler-and-vista-boundary.md): reticle vector compiler
- [Testing guide](../dev/testing.md)
- [Engineering workflow](../dev/standards/workflow.md)
