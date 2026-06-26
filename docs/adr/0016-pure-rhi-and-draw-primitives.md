# ADR-0016: Pure RHI and draw primitives — text and scene content move to sibling libraries

- Status: Accepted
- Date: 2026-06-23

## Context

[ADR-0001](0001-project-foundations.md) originally placed both **text
shaping** and **asset loading (glTF, PNG)** out of scope for the
rendering engine, on the principle that the engine accepts bytes and
producers belong above it. [ADR-0015](0015-text-layering.md) reversed
the text half: it pulled Layer-0 shaping (FreeType + HarfBuzz +
Fontconfig + FriBidi) into `libflux` as the in-tree `text` module,
superseding ADR-0001's "sibling library wrapping HarfBuzz" stance.

Three pressures now argue for returning to the original boundary.

### 1. `text` is the only module that sees another module

The application architecture states Rule 3
([application-architecture.md](../explanation/application-architecture.md)):

> Modules do not see each other. `flux_canvas`, `flux_scene`, and
> `flux_compute` all depend on `flux-core` but not on each other.

That rule enumerates three modules, and the include graph confirms they
are flat peers:

| Module | Public includes |
|--------|-----------------|
| `canvas` | `core`, `math` |
| `scene` | `core`, `math` |
| `compute` | `core` |
| `text` | `core`, `math`, **`canvas`** |

`text` includes `<flux/canvas.h>` and draws through
`flux_canvas_draw_glyph_run`. It is a **consumer of a canvas primitive**,
structurally identical to how a mesh loader consumes
`flux_scene_draw_mesh`. It is not a peer of `canvas` / `scene` /
`compute`; it sits one layer above. Rule 3 was written in the
three-module era; `text` was added later (by ADR-0015) and broke the
flat-module contract. This is the measurable layering anomaly: `text`
is the only "module" that depends on another module.

### 2. Dependency hygiene

`canvas`, `scene`, `compute`, and `effect` depend on Vulkan and threads
only. The `text` module drags in four non-Vulkan dependencies
(`freetype2`, `harfbuzz`, `fontconfig`, `fribidi`), the most CVE-active
surface in the stack. The `-Dshaping` option already admits this: the
whole module can be compiled out. A primitive that the engine draws
through should not be compilable away; the option is the tell that
`text` is a pluggable producer, not a core primitive.

### 3. Symmetry with scene content

A glTF loader and an image decoder face the same structural question as
a shaper: each is a **producer** that feeds a draw primitive. One rule
applied uniformly — all producers live in siblings — is cleaner than a
text special-case. The `flux-scene-graph` sibling (glTF) and the
`flux-text` sibling (shaping) then become symmetric: each feeds exactly
one primitive and lives one layer above it.

## Decision

`flux` is a **pure RHI and draw-primitives library**. The boundary is
the draw primitive itself: whatever the engine records into its own
command buffer through a first-class draw call stays in `libflux`;
whatever produces the data those primitives consume lives in a sibling
library one layer above.

### What stays in `libflux`

The module layer remains the flat peers of Rule 3, depending only on
`flux-core` and on Vulkan:

- `canvas` — 2D primitives, including the glyph-blit primitive
  `flux_canvas_draw_glyph_run`.
- `scene` — 3D primitives: `flux_camera`, `flux_mesh`, `flux_material`,
  `flux_scene_light`, and the `flux_scene_draw_mesh` /
  `flux_scene_draw_mesh_lit` record calls.
- `compute`, `effect` — unchanged.

`libflux` depends on Vulkan and threads only. The glyph-blit primitive
(ADR-0010), the mesh/material/camera/light primitives, and any future
material kinds (including PBR, when it ships as a `flux_material_kind`)
are engine primitives and stay.

### What moves out

Two sibling libraries, each one layer above the primitive it feeds:

- **`flux-text`** — HarfBuzz shaping. Owns FreeType, HarfBuzz,
  Fontconfig, FriBidi, the glyph atlas, BiDi run itemisation, subpixel
  positioning, and caret/selection mapping. Feeds
  `flux_canvas_draw_glyph_run`. This is the existing `text` module
  relocated; its implementation is unchanged, only its library
  boundary moves.
- **`flux-scene-graph`** — glTF 2.0 loading. Parses `.gltf` / `.glb`,
  resolves buffers and accessors, builds `flux_mesh` resources, maps
  materials to `flux_material`, and exposes a retained node tree with
  transforms for animation. Feeds `flux_scene_draw_mesh`.

```
libflux : core + canvas + scene + compute + effect   (Vulkan only)
            │  flux_canvas_draw_glyph_run   flux_scene_draw_mesh
            ▲  feeds the primitive
flux-text (HarfBuzz)            flux-scene-graph (glTF)
            ▲
flux-text-layout (paragraph composition, re-parented to flux-text)
```

### Relationship to ADR-0015

This ADR **supersedes [ADR-0015](0015-text-layering.md)**. Layer-0
shaping leaves `libflux` and becomes the `flux-text` sibling; the
Layer-0 / Layer-1 split itself is retained, only the Layer-0 location
moves out of the engine. The `flux-text-layout` crate (Layer-1
paragraph composition) re-parents: it now depends on `flux-text`
instead of `flux`. Its responsibility and signature are unchanged;
only the dependency edge moves.

ADR-0001's original scope note is **restored**: text shaping again
belongs in a sibling library wrapping HarfBuzz, as ADR-0001 first
recorded.

## Scope boundaries

In scope for `libflux`:

- Draw primitives and the GPU resources they consume directly
  (`flux_mesh`, `flux_image`, `flux_sampler`, glyph-blit).
- Material **kinds** as engine pipelines (`UNLIT`, `PHONG`, future
  `PBR`). PBR is a primitive (a shader pipeline), not content; the
  PBR *parameter data* that a glTF file supplies is content and lives
  in `flux-scene-graph`.

Out of scope for `libflux`, deliberately:

- Text shaping (HarfBuzz). Lives in `flux-text`.
- Paragraph layout. Lives in `flux-text-layout`, above `flux-text`.
- Asset loading (glTF, PNG, KTX2). Lives in `flux-scene-graph` and any
  future image-decoder sibling.
- Scene graph state, skeletal / morph animation. Lives in
  `flux-scene-graph`.

## Consequences

Positive:

- Rule 3 holds without exception. `libflux` modules are flat peers that
  depend only on `flux-core` and do not see each other.
- `libflux` depends on Vulkan and threads only. The four CVE-active
  shaping dependencies are isolated in `flux-text`, upgraded and
  released independently of the engine.
- Producers are symmetric: `flux-text` feeds the glyph primitive,
  `flux-scene-graph` feeds the mesh primitive, by the same rule.
- Siblings version independently. A KHR-extension burst in glTF, or a
  HarfBuzz minor release, does not churn the engine's API surface or
  release cadence.
- `flux-text` and `flux-scene-graph` are reusable outside any UI
  toolkit (CLI converters, validators, render farms, headless tests),
  matching the embeddability property the stack values elsewhere.

Negative:

- Every UI consumer now adds a `flux-text` link line and brings
  FreeType / HarfBuzz itself. This is the cost ADR-0015 named ("a
  separate library multiplies the link / config story") and is accepted
  deliberately in exchange for boundary purity.
- Two libraries instead of one for text. Mitigated: `flux-text` is a
  single dependency declared once.
- Migration churn. The `text` module source relocates; `lens` currently
  consumes a monospace stub and does not call `flux_text_*`, so its
  build is unaffected, but any future wiring goes through `flux-text`.

## Alternatives considered

- **Keep ADR-0015 (shaping in `libflux`).** Rejected on the structural
  Rule 3 violation and on dependency hygiene. The universality argument
  ADR-0015 made ("every UI consumer already brings HarfBuzz") is real
  but does not override the flat-module contract; it is a workload
  convenience, not a structural property.
- **Roll a custom shaper instead of depending on HarfBuzz.** Rejected.
  OpenType layout (GSUB / GPOS) and complex-script shaping (Arabic,
  Indic, Khmer, Tibetan) are among the harder problems in software;
  HarfBuzz is two decades of expert work and is the de-facto standard
  (Firefox, Chrome, Android, Qt, Pango, LibreOffice). `flux` already
  delegates shaping to HarfBuzz correctly and owns only the surrounding
  host machinery (atlas, cache, BiDi itemisation, positioning). That
  machinery moves with the module; the shaping is never reimplemented.
- **Keep `text` in `libflux` but add `flux-scene-graph` as the only
  sibling.** Rejected: it preserves the Rule 3 exception and the
  asymmetric special-case this ADR exists to remove.

## See also

- [ADR-0001](0001-project-foundations.md) — project foundations; its
  text-shaping scope note is restored by this ADR.
- [ADR-0010](0010-glyph-blit-primitive.md) — the glyph-blit primitive
  that is the engine boundary `flux-text` feeds.
- [ADR-0015](0015-text-layering.md) — superseded. Layer-0 shaping moves
  to `flux-text`; Layer-1 layout (`flux-text-layout`) re-parents.
- [Application architecture](../explanation/application-architecture.md)
  — Rule 3 ("modules do not see each other") is the structural test this
  ADR enforces.
- `libs/flux-text/` — the HarfBuzz shaping sibling.
- `libs/flux-scene-graph/` — the glTF sibling.
