---
id: ADR-0176
title: "Retire the Rust optics Facade Crate in Favor of Direct optics-* Crates"
status: accepted
date: 2026-11-27
scope: [crates, rust, build, docs]
superseded_by: null
negative_knowledge: true
---

# ADR-0176: Retire the Rust `optics` Facade Crate in Favor of Direct `optics-*` Crates

- Status: Accepted
- Date: 2026-11-27
- Deciders: Optics Architecture Working Group
- Consulted: Rust bindings, build plumbing (`optics-build`), documentation
- Informed: Core Engineering
- Amended: 2026-11-27 — standalone `optics-motion` is absorbed into `optics_ui::motion` by [ADR-0177](0177-absorb-motion-into-ui.md).
- Amends: [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md), [ADR-0163](0163-repository-root-cargo-workspace.md), [ADR-0169](0169-root-crates-layout-optics-namespace-and-shared-build.md), [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)

---

## Context and Problem Statement

[ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md) introduced a top-level `optics` facade crate to shield Rust consumers from nine abstract subsystem codenames (`flux`, `glyph`, `iris`, `lens`, `aperture`, `prism`, `reticle`, `vista`, `transit`). Since then, two structural changes removed that premise:

1. **[ADR-0170](0170-semantic-module-naming-and-optics-namespace.md) renamed every crate to its canonical semantic name** (`optics-gfx`, `optics-text`, `optics-motion`, `optics-window`, `optics-ui`, `optics-app`).
2. **[ADR-0174](0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md) pruned the stack to six orthogonal libraries** with no monolithic `liboptics.so` on the C side.

What remained in `crates/optics/` was a 27-line re-export shell (`pub use optics_* as *;`) with no prelude, no cross-crate types, and three concrete defects:

1. **Broken dynamic rpath relay for downstream consumers**: Every native wrapper crate (`optics-gfx`, `optics-text`, `optics-window`, `optics-ui`, `optics-app`) declares `links = "optics_<mod>_rs"` and calls `optics_build::relay_rpaths(..., true)` so direct dependents can read `DEP_OPTICS_<MOD>_RS_RPATHS`. `crates/optics/` declared no `links` key and passed `republish = false`, breaking dynamic builds for downstream consumers that relied on the facade.
2. **Cargo `links` anti-pattern if patched**: Fixing rpath relay on the facade would require adding a synthetic `links = "optics_rs"` key to a feature-gated umbrella crate that also carries pure-Rust `optics-motion`, imposing a Cargo global single-version lock and mandatory `build.rs` even on zero-FFI consumers solely to tunnel an environment variable.
3. **Mirrored feature-graph drift and entry-point ambiguity**: `crates/optics/Cargo.toml` duplicated the crate dependency graph as Cargo features and had already drifted (`ui = ["text", "dep:optics-ui"]` pulled in `optics-text-sys` and its bindgen run even though `optics-ui` does not depend on `optics-text`). Meanwhile, keeping both `optics` and `optics-*` forced every consumer to choose between two overlapping entry points.

---

## Decision Drivers

1. **Occam's Razor & KISS**: One crate graph as the single source of truth; no redundant wrapper layer or manually mirrored feature tree.
2. **1:1 C/Rust symmetry**: Six orthogonal C libraries under `libs/`, six corresponding safe Rust crates under `crates/`.
3. **Zero Cargo `links` hacks**: Only crates that wrap a native C library declare a `links` key; pure-Rust `optics-motion` stays free of `build.rs` and `links`.
4. **Single-dependency ergonomics for every real entry point**: Upper safe crates re-export their public dependencies directly (`optics_app::{gfx, ui, window}`, `optics_ui::gfx`, `optics_text::gfx`), matching idiomatic Rust multi-crate stacks (`eframe`/`egui`, `gtk4`/`gdk4`).

---

## Architectural Decisions

### 1. Delete `crates/optics/`
- Remove `crates/optics/` from the repository-root Cargo workspace.
- Enforce the retirement mechanically in `tools/check_versions.py` (`crates/optics` must not exist, alongside the existing `bindings/` layout checks).
- The C-side umbrella header (`include/optics/optics.h`, `optics_dep`, [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md)) is unaffected: it is a zero-cost header + `pkg-config` aggregate with no Cargo `links` or feature-graph conflict.

### 2. Direct `optics-*` Dependencies with Complete Upper-Layer Re-Exports
- External Rust consumers depend directly on the crate matching the layer they need:
  - Desktop GUI application: `optics-app`
  - Windowed canvas without UI: `optics-window` (+ `optics-gfx` if recording via safe Rust canvas APIs)
  - Headless UI: `optics-ui`
  - Text shaping: `optics-text`
  - Headless 2D canvas / Vulkan RHI: `optics-gfx`
  - Pure-Rust motion math: `optics-motion`
- Upper wrapper crates re-export the safe crates whose types appear in their public API:
  - `optics-app` exports `pub use optics_gfx as gfx;`, `pub use optics_ui as ui;`, and `pub use optics_window as window;`, plus only the UI types that appear directly in `optics-app` function signatures (`CursorHint`, `Frame`, `Input`, `Ui`). Arbitrary cherry-picked `optics-ui` symbols at the `optics-app` root are removed so one canonical module path (`optics_app::ui::*`) exists.
  - `optics-ui` exports `pub use optics_gfx as gfx;`.
  - `optics-text` exports `pub use optics_gfx as gfx;`.
- `optics-window` does not depend on `optics-gfx` and does not add an artificial dependency.

### 3. Static Linking and rpath Relay Stay Direct
- Each native wrapper crate continues to forward its `static` feature down its own Optics dependency closure (`optics-app/static` covers `app`, `window`, `ui`, and `gfx`) and republish `DEP_OPTICS_<MOD>_RS_RPATHS` via its `links = "optics_<mod>_rs"` key.

---

## Consequences

### Positive
- Eliminates an entire redundant crate, its duplicated feature tree, and the `optics` vs. `optics-*` entry-point ambiguity.
- GUI applications depending on `optics-app` get `optics_app::ui`, `optics_app::window`, `optics_app::gfx`, transitive `static` forwarding, and `DEP_OPTICS_APP_RS_RPATHS` from a single dependency line.
- Stops pulling `optics-text-sys` / bindgen into `optics-ui` and `optics-app` builds that do not use the Rust `optics-text` API.
- Pure-Rust `optics-motion` never acquires a synthetic `links` key or build script.

### Negative & Trade-offs
- Consumers combining two orthogonal branches (such as `optics-app` + `optics-motion`, or `optics-window` + `optics-text`) list two dependency lines in `Cargo.toml` instead of one crate with multiple features, and must enable `static` on each native branch (or set `OPTICS_STATIC=1`).

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep `crates/optics/` and add `links = "optics_rs"` with `relay_rpaths(..., true)`
- **Why considered**: Minimal diff (two lines) to unblock downstream dynamic rpath relay while keeping a single `optics` entry point.
- **Why rejected**: Preserves a 27-line hollow crate and a manually mirrored feature graph (which had already drifted on `ui -> text`), introduces a synthetic native `links` key onto a facade that includes pure-Rust `optics-motion`, and keeps two competing dependency styles across docs and downstream projects.

---

## Links

- Amends: [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md), [ADR-0163](0163-repository-root-cargo-workspace.md), [ADR-0169](0169-root-crates-layout-optics-namespace-and-shared-build.md), [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md)
- Related: [ADR-0174](0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md)
- Explanation: [Rust bindings](../explanation/rust-bindings.md)
- ADR Index: [ADR Index](index.md)
