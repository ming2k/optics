---
id: ADR-0159
title: "Unified Cargo Workspace and Optics Facade Bindings"
status: accepted
date: 2026-11-12
scope: bindings, .github/workflows/ci.yml
superseded_by: null
negative_knowledge: true
---

# ADR-0159: Unified Cargo Workspace and Optics Facade Bindings

- Status: Accepted
- Date: 2026-11-12
- Scope: `bindings/`, `.github/workflows/ci.yml`
- Amends: [ADR-0023](0023-unified-monorepo-build.md), [ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md)

## Context

When Optics migrated from a polyrepo to a unified monorepo in [ADR-0023](0023-unified-monorepo-build.md), the C build system was unified under a single root Meson build. However, the Rust bindings were left in an incomplete state: eight independent Cargo workspaces were relocated into `bindings/` (`flux-rs`, `glyph-rs`, `iris-rs`, `lens-rs`, `prism-rs`, `svg-rs`, `transit-rs`, `vista-rs`), each retaining its own `Cargo.toml`, `Cargo.lock`, independent Git repository metadata, and separate CI workflows.

This fragmented arrangement created acute engineering friction:

1. **Lockstep Version Friction & Diamond Dependencies**:
   While C libraries are versioned and built together in lockstep (ADR-0098), keeping eight distinct Cargo workspaces meant that bumping the repository version required updating dozens of `Cargo.toml` manifests and independent lockfiles. Downstream Rust consumers attempting to use multiple crates (e.g. `lens-rs` and `iris-rs`) routinely encountered version-mismatch diamond dependency conflicts on `flux-rs`.
2. **CI Matrix Proliferation & Silent Drift**:
   GitHub Actions CI had to execute an 8-way matrix build across all workspaces. Unlisted workspaces drifted out of formatting and lint compliance unnoticed (`.github/workflows/ci.yml`).
3. **Packaging Overhead for Micro-Libraries**:
   Maintaining independent crates for small modules like `transit` (pure math, ADR-0158) or `vista` incurred substantial FFI boilerplate, `build.rs` probe scripts, and metadata maintenance with zero architectural benefit.

## Decision

Execute a clean break: dismantle the eight independent Cargo workspaces and replace them with a **single, unified Cargo Workspace** under `bindings/` anchored by a canonical facade crate `optics`:

1. **Single Workspace Root (`bindings/Cargo.toml`)**:
   All crates share unified workspace dependencies, package metadata (`edition = "2024"`, `license = "MIT"`), and a single authoritative `Cargo.lock`.
2. **Canonical Facade Crate (`bindings/crates/optics`)**:
   Consumers depend on a single crate: `optics`. High-level safe abstractions are exposed under clean domain modules (`optics::canvas`, `optics::ui`, `optics::platform`, `optics::motion`, etc.).
3. **Pyramid Feature Flags for Strict Dependency Isolation**:
   To guarantee that lightweight or headless applications never pull in unwanted platform or graphics dependencies, the `optics` facade gates underlying layers via additive Cargo feature flags:
   - `default = ["ui", "platform"]` (complete desktop GUI application out of the box);
   - `canvas`: Layer 0 GPU/CPU rendering foundation (`flux`);
   - `typography`: Layer 1 HarfBuzz font shaping and layout (`glyph`);
   - `ui`: Layer 3 headless immediate-mode UI, layout, and state machine (`lens`);
   - `platform`: Layer 3 native desktop windowing and event pump (`iris`);
   - `materials`: Layer 2 physical BRDFs and dielectric glass shaders (`prism`);
   - `scene`: Layer 2 3D scene hierarchy and cameras (`vista`);
   - `svg`: Static SVG vector document compiler (`svg`);
   - `motion`: Layer 2 pure Safe Rust spring integration and easing curves (ADR-0158).
4. **Pure Safe Rust for `motion`**:
   The `motion` module implements the provable closed-form math from ADR-0077 / ADR-0158 natively in Safe Rust. It eliminates `transit-sys` and cross-language FFI overhead entirely.

## Consequences

Positive:
- **Zero Diamond Conflicts**: All Optics crates resolve to identical, synchronized versions within a single workspace.
- **Granular Pay-for-What-You-Use**: A headless backend CLI depending on `optics` with `default-features = false, features = ["canvas"]` compiles only the canvas and links zero Wayland, X11, or HarfBuzz libraries.
- **Radical CI Streamlining**: CI replaces 8-way workspace matrix runs with single commands: `cargo test --workspace` and `cargo clippy --workspace`.

Negative / Breaking Changes:
- The standalone crates (`flux-rs`, `lens-rs`, `iris-rs`, etc.) are retired and consolidated into the `optics` crate.
