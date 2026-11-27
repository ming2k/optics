---
id: ADR-0170
title: "Semantic Module Naming and Unified optics Namespace"
status: accepted
date: 2026-11-20
scope: libs/, crates/, include/, tests/, examples/, tools/, meson.build, Cargo.toml, docs/
superseded_by: null
negative_knowledge: true
---

# ADR-0170: Semantic Module Naming and Unified `optics` Namespace

- Status: Accepted
- Date: 2026-11-20
- Deciders: Optics Architecture Working Group
- Consulted: All library owners (`gfx`, `text`, `svg`, `material`, `scene`, `motion`, `window`, `ui`, `app`)
- Informed: Core Engineering
- Amended: 2026-11-27 — the Rust `optics` facade crate is retired in favor of direct `optics-*` crates by [ADR-0176](0176-retire-rust-optics-facade-crate.md).
- Amends: [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md), [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), [ADR-0169](0169-root-crates-layout-optics-namespace-and-shared-build.md)

---

## Context and Problem Statement

As Optics matured into a complete nine-library C23 graphics and UI stack with matching Rust crates, the original metaphorical codenames (`flux`, `glyph`, `reticle`, `prism`, `vista`, `transit`, `iris`, `lens`, `aperture`) accumulated two structural defects:

1. **Severe cognitive load across nine abstract metaphors.** Beyond three or four codenames, contributors and consumers can no longer tell a module's responsibility from its name alone — `iris` vs. `lens` vs. `aperture` and `reticle` vs. `vista` vs. `prism` require constant mental translation. Even the Rust facade (`crates/optics/Cargo.toml`) had already drifted toward semantic feature names (`ui`, `scene`, `motion`, `materials`, `app`) while the C libraries and `-sys` crates retained abstract codenames.
2. **System-wide installation and linker collisions.** Installing bare `<flux/flux.h>`, `<iris/iris.h>`, `<lens/lens.h>`, `libflux.so`, `libiris.so`, `liblens.so`, `flux.pc`, `iris.pc`, and `lens.pc` into `/usr/include` and `/usr/lib` pollutes the global namespace with generic words that collide with unrelated projects. Conversely, bare semantic names (`libui.so`, `libwindow.so`, `<text/text.h>`) would collide even more readily without a brand namespace.

---

## Decision Drivers

1. Make every library's responsibility self-evident from its directory name, header path, C symbol prefix, `pkg-config` name, and Rust crate name.
2. Eliminate global include/library/pkg-config/crates.io collisions by scoping every artifact under the `optics` brand namespace.
3. Preserve the exact one-way dependency DAG and architectural boundaries established in [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md) (`window` depends on `gfx` only; `ui` is headless; `app` is the single bridge).
4. Execute a clean break with zero compatibility shims (pre-1.0 policy) and enforce the naming contract mechanically in CI (`tools/check_naming.py`).

---

## Architectural Decisions

### 1. Canonical Semantic Module Registry

Every module in the Optics stack uses one canonical semantic name across C, Meson, `pkg-config`, and Rust:

| Former codename | Canonical module | C shared library | `pkg-config` | Public header entry | C symbol prefix | Rust crates |
|---|---|---|---|---|---|---|
| `flux` | `gfx` | `liboptics_gfx` | `optics-gfx` | `<optics/gfx.h>` (`<optics/gfx/*.h>`) | `optics_gfx_` / `OPTICS_GFX_` | `optics-gfx`, `optics-gfx-sys` |
| `glyph` | `text` | `liboptics_text` | `optics-text` | `<optics/text.h>` | `optics_text_` / `OPTICS_TEXT_` | `optics-text`, `optics-text-sys` |
| `reticle` | `svg` | `liboptics_svg` | `optics-svg` | `<optics/svg.h>` (`<optics/svg/*.h>`) | `optics_svg_` / `OPTICS_SVG_` | `optics-svg`, `optics-svg-sys` |
| `prism` | `material` | `liboptics_material` | `optics-material` | `<optics/material.h>` (`<optics/material/*.h>`) | `optics_material_` / `OPTICS_MATERIAL_` | `optics-material`, `optics-material-sys` |
| `vista` | `scene` | `liboptics_scene` | `optics-scene` | `<optics/scene.h>` | `optics_scene_` / `OPTICS_SCENE_` | `optics-scene`, `optics-scene-sys` |
| `transit` | `motion` | *(header-only)* | `optics-motion` | `<optics/motion.h>` (`<optics/motion/*.h>`) | `optics_motion_` / `OPTICS_MOTION_` | `optics-motion` |
| `iris` | `window` | `liboptics_window` | `optics-window` | `<optics/window.h>` (`<optics/window/*.h>`) | `optics_window_` / `OPTICS_WINDOW_` | `optics-window`, `optics-window-sys` |
| `lens` | `ui` | `liboptics_ui` | `optics-ui` | `<optics/ui.h>` (`<optics/ui/*.h>`) | `optics_ui_` / `OPTICS_UI_` (`optics_ui` handle) | `optics-ui`, `optics-ui-sys` |
| `aperture` | `app` | `liboptics_app` | `optics-app` | `<optics/app.h>` | `optics_app_` / `OPTICS_APP_` | `optics-app`, `optics-app-sys` |

### 2. Unified Install Layout (`<optics/...>`)

- Every C library under `libs/<mod>/` places its public headers under `libs/<mod>/include/optics/` (`<mod>.h` and optional `<mod>/*.h`), so `meson install` populates a single clean `/usr/include/optics/` hierarchy without polluting `/usr/include/`.
- Meson build options use canonical semantic names (`text`, `scene`, `material`, `motion`, `svg`).
- Umbrella feature macros use canonical names (`OPTICS_HAVE_TEXT`, `OPTICS_HAVE_SCENE`, `OPTICS_HAVE_MATERIAL`, `OPTICS_HAVE_MOTION`, `OPTICS_HAVE_SVG`, `OPTICS_HAVE_UI`, `OPTICS_HAVE_APP`).
- The Rust facade `crates/optics` exposes matching feature names and module re-exports (`optics::gfx`, `optics::text`, `optics::svg`, `optics::material`, `optics::scene`, `optics::motion`, `optics::window`, `optics::ui`, `optics::app`).

### 3. Clean Break & Mechanical Enforcement

- No legacy header shims, symbol aliases, or deprecated crate/option names are kept.
- `tools/check_naming.py` (registered in the `lint` and `tooling` test suites, with negative regression tests in `tests/optics/test_naming_guardrail.py`) mechanically enforces:
  1. Only canonical module names exist under `libs/`, `crates/`, `tests/`, and `examples/`.
  2. Every library's public headers live under `libs/<mod>/include/optics/`.
  3. Every public `OPTICS_<MOD>_API` declaration uses the `optics_<mod>_` prefix.
  4. Legacy codenames do not reappear in `meson_options.txt`, `libs/*/meson.build` `pkg-config` targets, or `Cargo.toml`.

---

## Consequences

### Positive
- Every directory, header, library binary, `pkg-config` package, C symbol, and Rust crate is immediately self-describing and collision-free on system installation.
- C and Rust namespaces are 1:1 symmetric (`<optics/ui.h>` ↔ `optics::ui` ↔ `liboptics_ui.so` ↔ `optics-ui.pc`).
- One-way dependency isolation (`window` → `gfx`, headless `ui`, `app` bridge) is preserved without any architectural regression.

### Negative & Trade-offs
- Breaking rename across the entire C and Rust public surface (acceptable pre-1.0).
- C identifiers are slightly longer (`optics_ui_button` vs. `lens_button`), trading a few characters for global collision safety and immediate clarity.

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep C codenames and only expose semantic names in the Rust `optics` facade
- **Why considered**: Avoids touching C source files and headers.
- **Why rejected**: Leaves C consumers, system installations (`/usr/include/flux/`, `libiris.so`), and C contributors permanently burdened by two conflicting vocabularies.

### 2. Bare semantic names without the `optics_` prefix (`libui.so`, `<window/window.h>`)
- **Why considered**: Shorter C identifiers (`ui_button`, `window_run`).
- **Why rejected**: Guaranteed header, linker, and `pkg-config` collisions in any multi-library environment.

---

## Links

- Amends: [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md), [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), [ADR-0169](0169-root-crates-layout-optics-namespace-and-shared-build.md)
- Related Standards: [Architecture](../dev/standards/architecture.md), [API Design](../dev/standards/api-design.md)
- ADR Index: [ADR Index](index.md)
