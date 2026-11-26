---
id: ADR-0174
title: "Pure Graphics & UI Core — Retire Format Parsers, 3D Scene, Branded Materials, and Bundled Icon Packs"
status: accepted
date: 2026-11-27
scope: [gfx, ui, architecture, build, rust]
superseded_by: null
negative_knowledge: true
---

# ADR-0174: Pure Graphics & UI Core — Retire Format Parsers, 3D Scene, Branded Materials, and Bundled Icon Packs

- Status: Accepted
- Date: 2026-11-27
- Supersedes: [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0076](0076-lens-icon-paint-runs.md), [ADR-0111](0111-lens-runtime-svg-mixed-paint-runs-and-orthogonal-icon-sizing.md), [ADR-0150](0150-svg-vector-document-compiler.md), [ADR-0161](0161-reticle-vector-compiler-and-vista-boundary.md)
- Amends: [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)

## Context

Over time, four non-core domains accreted inside the Optics repository:

1. **External document and asset format parsers (`libs/svg`, `libs/scene`)**:
   - `libs/svg` implemented a partial SVG 1.1 subset parser solely to feed `optics_ui_icon_register_svg` and downstream desktop chrome consumers. It hard-coupled `ui` (`opt_ui = opt_text and opt_svg`) while permanently omitting `<use>`, `<clipPath>`, `<mask>`, `<filter>`, `<style>`, `<text>`, and isolated group opacity.
   - `libs/scene` shipped a hand-rolled JSON and glTF `.glb` parser plus VRM 0.x/1.0 humanoid animation retargeting.
2. **Hardcoded 3D Phong/skinning forward renderer inside `libs/gfx`**:
   - `libs/gfx/src/scene/` embedded 8 SPIR-V shaders and 3D mesh/material/camera types (`<optics/gfx/scene.h>`) inside `liboptics_gfx`, even though `gfx` already exposes general Vulkan 1.3 RHI (`optics_gfx_graphics_pipeline`, `buffer`, `target`, `compute`, `effect`).
3. **Branded commercial OS shader packs (`libs/material`, `<optics/ui/material.h>`)**:
   - `libs/material` bundled Apple Liquid Glass, macOS Frosted Vibrancy, Windows Acrylic, and Windows 11 Mica shaders. Meanwhile `<optics/ui/material.h>` leaked those commercial names (`OPTICS_UI_MATERIAL_MICA`, `LIQUID_GLASS`, `optics_ui_body`) into `optics_ui_theme` and `optics_ui_style` even though zero widgets consumed them.
4. **Bundled third-party icon pack and process-global icon registry in `libs/ui`**:
   - `libs/ui/include/optics/ui/icon.h` baked 288 Feather/Material icons (including third-party brand logos such as YouTube, Twitch, Twitter, Facebook, Figma, Slack) into the C ABI (`icon_data.c`, 4,121 lines).
   - `libs/ui/src/icon_runtime.c` stored runtime icons in a process-global, non-thread-safe, never-reclaimed static array and suffered from a dual-sentinel hazard (`0 == ACTIVITY` vs `-1 == INVALID`).

## Decision

Optics is strictly a **pure, restrained, orthogonal C23 2D graphics and UI stack** composed of six libraries:

```text
app ──┬──► window ──────────────► gfx
      └──► ui ────► text ───────► gfx
motion   (header-only; libm only)
```

1. **Retire `libs/svg`, `libs/scene`, and `libs/material`** along with their Rust crates (`optics-svg*`, `optics-scene*`, `optics-material*`), Meson options, tests, and dedicated showcases. No compatibility shims are kept (pre-1.0 clean break).
2. **Retire `<optics/gfx/scene.h>` and `libs/gfx/src/scene/`**: `liboptics_gfx` owns the Vulkan 1.3 RHI, GPU/CPU 2D vector canvas, compute pipelines, general image effects (blur, backdrop, shadow), analytic curves, analytic SDF, color management, math, and the observation ledger.
3. **Remove `<optics/ui/material.h>` and material fields from `optics_ui_theme` / `optics_ui_style`**.
4. **Replace the `libs/ui` icon system with a format-agnostic, per-context vector registry**:
   - `OPTICS_UI_ICON_NONE = 0` is the single empty sentinel (C23 `{}` zero-initialization always means "no icon").
   - Only four minimal geometric chrome chevrons (`OPTICS_UI_ICON_CHEVRON_{UP,DOWN,LEFT,RIGHT}`) remain built in for `tree_view` and `datagrid`.
   - `optics_ui_icon_register(ui, desc)` / `optics_ui_icon_unregister(ui, id)` / `optics_ui_icon_info(ui, id)` manage application vector icons per `optics_ui` context with generational handles (`(gen << 16) | slot`), automatic coverage-mask eviction on unregister, full device-allocator funnel attribution, and zero process-global state.
5. **Mechanical enforcement**: `tools/check_library_purity.py` rejects `libs/svg`, `libs/scene`, `libs/material`, and branded OS material references in both `libs/gfx` and `libs/ui`.

## Consequences

- **No external format parsers in the core stack**: PNG, JPEG, WebP, SVG, JSON, glTF, and VRM decoding belong outside Optics. Callers decode with external libraries and submit `optics_gfx_image`, `optics_gfx_path`, or `optics_ui_icon_desc`.
- **Simpler dependency graph**: `ui` depends only on `gfx` and `text`; `opt_ui = opt_text`.
- **Clean multi-context isolation**: zero process-global mutable icon tables and zero leaked icon allocations.
