---
id: ADR-0171
title: "Curated Examples Taxonomy and Showcase Design System"
status: accepted
date: 2026-11-20
scope: examples/, tools/probes/, meson.build, tools/check_naming.py, docs/
superseded_by: null
negative_knowledge: true
---

# ADR-0171: Curated Examples Taxonomy and Showcase Design System

- Status: Accepted
- Date: 2026-11-20
- Deciders: Optics Architecture Working Group
- Consulted: UI, Window, Graphics, Material, Scene owners
- Informed: Core Engineering
- Amends: [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)

---

## Context and Problem Statement

The `examples/` tree accumulated 24 standalone binaries across 10 directories mixing three incompatible purposes:

1. **Toy and overlapping widget demos** (`widgets.c`, `desktop_demo.c`, `fonts.c`, `dnd_demo.c`, `overlay_demo.c`, `scenarios/overlays/`) with ad-hoc spacing, inconsistent typography, and placeholder copy.
2. **Developer diagnostic probes** (`frame_skip_demo.c`, `paint_static_demo.c`) and console-only unit-style prints (`ui/headless_demo.c`, `ui/state_demo.c`, `ui/a11y_tree_demo.c`) that belong in `tests/` or `tools/probes/`, not user-facing examples.
3. **Mixed directory organization** (both per-library directories and `scenarios/` / `showcase/`), making it unclear which binaries represent canonical starting points versus flagship demos.

---

## Decision Drivers

1. Every in-tree example must either be a **minimal, copyable starter** (< 100 lines where practical, teaching one architectural path) or a **flagship showcase application** with cohesive layout, typography, and interaction craft.
2. Developer frame-pacing and repaint probes must not masquerade as user-facing examples.
3. All showcase applications must share a lightweight public-API design token header (`examples/showcase/common/design.h`) so spacing, typography, cards, and property rows stay visually consistent.
4. Enforce the `examples/` top-level taxonomy mechanically in `tools/check_naming.py`.

---

## Architectural Decisions

### 1. Two-Tier `examples/` Taxonomy

`examples/` contains exactly two top-level subdirectories:

- **`examples/getting_started/`** — minimal, single-concept starters:
  - `hello_app`: minimal `optics_app_run` GUI application (`app` + `ui`).
  - `canvas_window`: native window drawing 2D vector geometry without `ui` (`window` + `gfx`).
  - `headless_canvas`: GPU-free CPU canvas rendering to PPM (`gfx`).
  - `headless_svg`: GPU-free SVG parsing and rasterization to PPM (`svg`).
  - `compute_pass`: headless Vulkan compute shader dispatch and readback (`gfx`).
- **`examples/showcase/`** — three flagship desktop applications built over shared `examples/showcase/common/design.h`:
  - `widget_gallery`: multi-page desktop UI gallery (Typography, Controls, Layout & Data, Overlays & DnD, Motion).
  - `material_gallery`: optical surface material showcase (Liquid Glass, Frosted Glass, Acrylic, Mica).
  - `scene_studio`: interactive 3D glTF 2.0 viewer with studio toolbar and live camera/lighting inspector.

### 2. Relocate Diagnostic Probes & Remove Redundant Toy Demos

- `frame_skip_demo.c` and `paint_static_demo.c` move to `tools/probes/` as developer-only manual diagnostic probes.
- Console-only demos already covered by `tests/ui/` (`headless_demo.c`, `state_demo.c`, `a11y_tree_demo.c`) and superseded fragmented demos (`widgets.c`, `desktop_demo.c`, `fonts.c`, `dnd_demo.c`, `overlay_demo.c`, `scenarios/overlays/`, `image_animation.c`, `scene_cube.c`, `text_hello.c`, `motion_spring.c`) are deleted cleanly.

### 3. Mechanical Enforcement

`tools/check_naming.py` enforces that `examples/` contains only `getting_started` and `showcase`, preventing uncurated per-module example sprawl.

---

## Consequences

### Positive
- Example count drops from 24 fragmented binaries to 8 high-signal binaries (5 starters + 3 flagship showcases).
- Every showcase adheres to a shared 8px spacing grid, typography hierarchy, and surface-card grammar.
- First-time contributors and evaluators have an immediately obvious entry point (`getting_started/` vs `showcase/`).

### Negative & Trade-offs
- Maintaining richer flagship showcases requires keeping them in sync with `optics_ui` pattern APIs (which also acts as an early dogfooding signal).

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep one `examples/<module>/` directory per library
- **Why considered**: Mirrors `libs/<module>/` 1:1.
- **Why rejected**: Encourages artificial toy demos for libraries that are best demonstrated in combination (`text`, `motion`, `ui`, `app`), and splits overlapping features across multiple low-craft binaries.

---

## Links

- Amends: [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)
- Examples Guide: [How to Run and Explore Examples](../how-to/run-and-explore-examples.md)
- ADR Index: [ADR Index](index.md)
