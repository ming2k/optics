---
id: ADR-0172
title: "Zero-Stutter Public C API and Clean window vs. app Runtime Boundary"
status: accepted
date: 2026-11-20
scope: libs/app/, libs/window/, libs/scene/, libs/material/, crates/, examples/, tests/, tools/check_naming.py
superseded_by: null
negative_knowledge: true
---

# ADR-0172: Zero-Stutter Public C API and Clean `window` vs. `app` Runtime Boundary

- Status: Accepted
- Date: 2026-11-20
- Deciders: Optics Architecture Working Group
- Consulted: Window, UI, App, Scene, Material owners
- Informed: Core Engineering
- Amends: [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)

---

## Context and Problem Statement

Applying the unified `optics_<mod>_` namespace in [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md) exposed two structural API defects inherited from the former metaphorical names:

1. **Stuttered public identifiers (`optics_<mod>_<mod>_*`).** When `aperture_app_run`, `iris_window_close`, `vista_scene_release`, and `prism_material_quality` were mechanically prefixed with semantic module names, they became `optics_app_app_run`, `optics_window_window_close`, `optics_scene_scene_release`, and `optics_material_material_quality`.
2. **Blurred `window` vs. `app` boundary.** `libs/window` still owned `optics/window/app.h` and `optics_window_app_run`, conflating window event loops with the higher-level `app` shell. Meanwhile `libs/app` exposed only a single `run` function, forcing every GUI application to reach into `<optics/window.h>` for basic application actions like quitting (`close`), requesting an animation frame, or posting to the main thread.

---

## Decision Drivers

1. Eliminate all stuttered public identifiers (`optics_<mod>_<mod>_*` and `OPTICS_<MOD>_<MOD>_*`) across every library.
2. Give `libs/window` a pure window-and-event-loop vocabulary (`optics_window_run`, `optics_window_opts`, `optics_window_close`, `optics/window/loop.h`, `optics/window/state.h`) with no `app` terminology.
3. Give `libs/app` a complete application-level runtime facade (`optics_app_run`, `optics_app_opts`, `optics_app_quit`, `optics_app_request_frame`, `optics_app_post`, `optics_app_get_geometry`) while preserving the strict one-way dependency DAG (`window` → `gfx`, headless `ui`, `app` → `window + ui`).
4. Enforce zero stutter mechanically in `tools/check_naming.py`.

---

## Architectural Decisions

### 1. Clean `window` vs. `app` Boundary

- **`libs/window`** (`<optics/window.h>`):
  - `optics/window/app.h` → `optics/window/loop.h`; `optics/window/window.h` → `optics/window/state.h`.
  - `optics_window_app_run` / `optics_window_app_opts` → `optics_window_run` / `optics_window_opts`.
  - `optics_window_window_*` → `optics_window_*` (`optics_window_close`, `optics_window_minimize`, `optics_window_maximize`, `optics_window_fullscreen`, `optics_window_get_geometry`, etc.).
- **`libs/app`** (`<optics/app.h>`):
  - `optics_app_app_run` / `optics_app_app_opts` → `optics_app_run` / `optics_app_opts`.
  - Adds application-level runtime controls delegating to the active window loop:
    - `optics_app_quit(void)`
    - `optics_app_request_frame(void)`
    - `optics_app_post(optics_app_main_fn fn, void *user)`
    - `optics_app_get_geometry(int32_t *out_width, int32_t *out_height)`

### 2. Zero-Stutter Across `scene` and `material`

- **`libs/scene`**: `optics_scene_scene` → `optics_scene`; `optics_scene_scene_*` → `optics_scene_*`.
- **`libs/material`**: `optics_material_material_*` → `optics_material_*`; `OPTICS_MATERIAL_MATERIAL_*` → `OPTICS_MATERIAL_*`.

### 3. Mechanical Enforcement

`tools/check_naming.py` rejects any public `optics_<mod>_<mod>` or `OPTICS_<MOD>_<MOD>_` identifier in `libs/<mod>/include/optics/**/*.h`, and rejects `optics/window/app.h` or `optics_window_app_*` in `libs/window`.

---

## Consequences

### Positive
- Standard GUI apps (`hello_app`, `widget_gallery`) only include `<optics/app.h>` and `<optics/ui.h>`, with clean verbs (`optics_app_run`, `optics_app_quit`, `optics_app_request_frame`).
- Pure canvas windows (`canvas_window`) use `<optics/window.h>` (`optics_window_run`, `optics_window_close`) with zero UI or `app` concepts.
- Zero stuttered C/Rust FFI identifiers across all 9 libraries.

### Negative & Trade-offs
- Breaking C ABI and Rust `-sys` symbol rename (clean break pre-1.0).

---

## Rejected Alternatives & Negative Knowledge

### 1. Merge `libs/app` into `libs/window`
- **Why considered**: Reduces library count by one.
- **Why rejected**: Re-introduces the `window` → `ui` dependency inversion retired by [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), forcing canvas-only windows to link the entire UI/HarfBuzz/SVG stack.

---

## Links

- Amends: [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)
- Related: [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md)
- ADR Index: [ADR Index](index.md)
