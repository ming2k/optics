---
id: ADR-0173
title: "Retire the algorithm vault and compatibility scaffolding; strokes as coverage contours"
status: accepted
date: 2026-11-23
scope: gfx, material, window, ui, tools, docs
superseded_by: null
negative_knowledge: true
---

# ADR-0173: Retire the algorithm vault and compatibility scaffolding; strokes as coverage contours

- Status: Accepted
- Date: 2026-11-23
- Supersedes: [ADR-0053](0053-shader-embed-fallback.md) (the generated-header
  shader fallback).
- Amends: [ADR-0121](0121-algorithm-vault-and-unified-analytic-vector-infrastructure.md)
  (the "algorithm vault" is withdrawn; the analytic vector infrastructure
  stands) and [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)
  (the naming guardrail now covers live content, not only names).

## Context

A review against the repository's own rule that a retired mechanism is one no
caller can reach (`docs/dev/standards/adr.md`) found code and claims that
outlived their reasons:

1. **The ear-clip "vault"** (`optics/gfx/vault/tess_earcut.h`) was compiled,
   exported and installed as public ABI after the tessellator it came from was
   retired. Its only caller was its own unit test. It was a naive O(n²) ear
   clipper with no hole support that stalled on collinear or duplicate
   vertices.
2. **The `#embed` fallback** (ADR-0053) survived only in the tests and one
   example. The libraries embedded unconditionally, so a toolchain without
   `#embed` could not build the tree anyway, while `spv2h.py`, `__has_embed`
   probes and build-side MSVC branches suggested otherwise. Several fixtures
   were silently taking the generated-header path.
3. **Strokes** were tessellated into triangles, and each triangle was then
   re-wound and handed to the coverage rasterizer as its own three-point
   contour, through a second backend entry point (`submit_cover_triangles`)
   with its own uniform-colour restriction. Writing a pixel test for that path
   exposed two older stroker defects: at a vertex whose miter overflowed, the
   outgoing body started from the incoming segment's offsets (past a
   90-degree turn the quad twisted and drew about half the band), and join
   geometry was placed on the inner side of the turn, where the bodies hid it.
4. **Material filters** carried five copies of the same barrier, pipeline and
   slab code, and four copies of the storage-clear shader, despite a shared
   header written to prevent exactly that. The copies had already drifted.
5. **The rename was incomplete.** ADR-0170's guardrail checked directory names
   and API macros, so about 1,100 codename references remained in sources,
   tests and docs: runtime strings (the Vulkan engine name, ledger owners,
   profiler zones), comments describing functions that no longer existed, and
   docs whose code samples called removed APIs.

## Decision

1. **Delete the vault** (source, installed header, test). Git history keeps the
   algorithm.
2. **Require C23 `#embed`.** Configure fails with a message on a compiler
   without it (GCC >= 15, Clang >= 19). `spv2h.py`, every `__has_embed` probe
   and the generated headers are gone; test shaders embed through
   `--embed-dir` on the shared test dependency. Build-side MSVC branches are
   removed. The public headers keep their MSVC-compatible attribute spellings
   for C++ consumers of the shipped binaries. Hand-written `strdup` and
   overflow helpers give way to C23 `strdup` and `<stdckdint.h>`.
3. **Strokes are contours.** The stroker emits every segment body, join and cap
   as one positively oriented convex contour (a non-convex body quad is split
   into triangles), and submits them through the same `submit_cover` as fills,
   under the non-zero rule. The union of positively oriented simple polygons is
   exactly their union, so overlapping pieces never double-blend. Bodies at an
   overflowed miter end on their own segment's normal; joins go on the outer
   side. `submit_cover_triangles` is removed from both backends.
   `tests/gfx/unit/test_canvas_stroke_pieces.c` pins single coverage and the
   absence of holes for every join and cap.
4. **One material frame module** (`libs/material/src/material_frame.{h,c}`)
   owns the embedded shaders (each exactly once), lazy pipeline creation, the
   slab output, the storage clear and the barriers.
5. **The naming guardrail checks content.** `tools/check_naming.py` now fails on
   (a) a codename in any live file — identifiers, strings, comments, docs, CI
   workflows; (b) an `optics_*` name mentioned in a comment or doc that no code
   defines; (c) a repository path that does not exist. ADRs and the CHANGELOG
   are history and are exempt. `docs/reference/symbols.md` is generated from
   the headers (`tools/gen_symbols.py`) and checked for staleness.
6. **Restore the leak guardrail** (ADR-0130) as `tools/check_leaks.py`, a lint
   test, with `tools/config/leak-lanes.toml`, `leak-exempt.toml` and
   `tests/optics/test_leaks_guardrail.py`, translated to the current names.

## Alternatives Considered

- **Keep the vault as reference.** A reference that ships as ABI is a liability:
  it must be maintained, documented and kept building. The history is in git.
- **Restore the `#embed` fallback in the libraries.** Would let MSVC `cl` and old
  Apple Clang configure, at the cost of two embedding paths that must stay
  byte-identical. No supported platform needs it: Windows CI uses MinGW-w64 GCC
  and macOS CI uses LLVM clang.
- **Keep triangle strokes and only fix the two defects.** Leaves a second
  coverage entry point and a uniform-colour restriction alive for one caller,
  and pushes three times the edges through the rasterizer for round joins.
- **Ban the ambiguous words (`lens`, `glyph`, `transit`) outright.** They are
  real vocabulary here (the glass lens, a glyph run, a value in transit); only
  their codename forms are flagged.

## Consequences

- Breaking: `<optics/gfx/vault/tess_earcut.h>` and its two functions are gone.
- Building needs GCC >= 15 or Clang >= 19. Linux CI moves to clang-19
  (ubuntu-24.04 has no GCC with `#embed`); GCC coverage is the MinGW-w64 lane.
- Stroke pixels change at overflowed-miter vertices: corners are now filled
  where the old stroker left them partly empty.
- Runtime strings change: Vulkan application/engine names (`optics`), ledger
  owners (`gfx/core`, `ui/core`, `text/core`, `window/wayland`, `material`) and
  profiler zone prefixes.
- The leak-coverage guardrail (ADR-0130), deleted without replacement in commit
  `ffe1fde9`, is restored as `tools/check_leaks.py` with its ledgers and
  negative tests, so the `RAW_ALLOC_OK(...)` annotations are enforced again.
