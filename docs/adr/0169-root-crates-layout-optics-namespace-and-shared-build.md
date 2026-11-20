---
id: ADR-0169
title: "Repository-Root crates/ Layout, optics-* Crate Namespace, and Shared optics-build Helper"
status: accepted
date: 2026-11-20
scope: Cargo.toml, crates/, tools/check_versions.py, .github/workflows/ci.yml, docs/
superseded_by: null
negative_knowledge: true
---

# ADR-0169: Repository-Root `crates/` Layout, `optics-*` Crate Namespace, and Shared `optics-build` Helper

- Status: Accepted
- Date: 2026-11-20
- Deciders: Optics Architecture Working Group
- Scope: `Cargo.toml`, `crates/`, `tools/check_versions.py`, `.github/workflows/ci.yml`, `docs/`
- Amends: [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md), [ADR-0163](0163-repository-root-cargo-workspace.md)

---

## Context and Problem Statement

[ADR-0163](0163-repository-root-cargo-workspace.md) relocated the Cargo workspace
manifest to the repository root, but left the crate tree nested at
`bindings/crates/` as a transitional holdover from
[ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md). That
layout accumulated four concrete structural defects:

1. **Redundant wrapper directory and semantic mismatch.** `bindings/` contained
   only a single subdirectory `crates/`. Claiming `bindings/` was a multi-language
   namespace while placing a Rust-specific term (`crates/`) directly inside it
   conflated language identity with Rust packaging and added an empty directory
   layer relative to the root `Cargo.toml`.
2. **Namespace collision on crates.io.** Sub-crates were named `flux`, `glyph`,
   `lens`, `iris`, `aperture`, `prism`, `reticle`, `vista`, and `transit` (plus
   `*-sys`). Nearly all of those short generic names are already claimed on
   crates.io, guaranteeing a breaking rename when the publication gate is met.
3. **Massive `build.rs` duplication and drift.** Eight `*-sys` crates duplicated
   over 3,500 lines of Meson uninstalled-tree discovery, `pkg-config` probing,
   static-preferred linking, `rpath` emission, and Windows DLL staging. Copies
   had already drifted (e.g. `prism-sys` omitted `OPTICS_SOURCE_DIR`; `lens` and
   `glyph` queried non-existent `DEP_FLUX_RPATHS`; `transit` retained a dead
   `build.rs` referencing a deleted `transit-sys`).
4. **Hardcoded internal path-dependency versions.** Member manifests repeated
   `version = "0.0.81"` instead of inheriting from `[workspace.dependencies]`.

---

## Decision Drivers

1. Establish a symmetric, idiomatic repository root: `libs/` for C23 libraries,
   `crates/` for Rust crates.
2. Preserve compile-time one-way DAG enforcement and per-library Cargo `links`
   uniqueness matching `libs/`.
3. Make every workspace crate name collision-free under the `optics-*` prefix
   while keeping the `optics` facade API (`optics::canvas`, `optics::ui`, etc.)
   unchanged.
4. Eliminate build-script duplication through a single shared internal crate and
   enforce layout invariants mechanically in `tools/check_versions.py`.

---

## Architectural Decisions

### 1. Elevate `bindings/crates/` to Repository-Root `crates/`

- All Rust crates live directly under `crates/` (`members = ["crates/*"]` in the
  repository-root `Cargo.toml`).
- The empty `bindings/` wrapper directory is removed. If future non-Rust
  language bindings are added, they may live under `bindings/<language>/`
  without colliding with first-class Rust crates under `crates/`.
- `tools/check_versions.py` fails the `lint` suite if `bindings/Cargo.toml`,
  `bindings/crates/`, or `crates/Cargo.toml` exists.

### 2. Prefix All Internal Sub-Crates with `optics-*`

- The canonical facade crate remains `optics` (`crates/optics/`), preserving its
  public feature flags and re-exported module paths (`optics::canvas`,
  `optics::typography`, `optics::ui`, `optics::platform`, `optics::app`,
  `optics::materials`, `optics::reticle`, `optics::scene`, `optics::motion`).
- Every sub-crate carries the `optics-*` prefix: `optics-flux`,
  `optics-flux-sys`, `optics-glyph`, `optics-glyph-sys`, `optics-iris`,
  `optics-iris-sys`, `optics-lens`, `optics-lens-sys`, `optics-aperture`,
  `optics-aperture-sys`, `optics-prism`, `optics-prism-sys`, `optics-reticle`,
  `optics-reticle-sys`, `optics-vista`, `optics-vista-sys`, and
  `optics-transit`.
- Native `-sys` crates keep `links = "<c_lib>"` (matching `lib<c_lib>`); safe
  wrapper crates publish `rpath` metadata via collision-free
  `links = "optics_<c_lib>"`.
- `optics-transit` is pure Safe Rust with zero C FFI and has no `build.rs`.

### 3. Centralize Build Plumbing in `crates/optics-build` and `[workspace.dependencies]`

- `crates/optics-build` is an internal build-dependency crate owning Meson
  build-tree discovery, `pkg-config` probing, static-first Optics archive
  linking, Unix/macOS `rpath` relay, Windows DLL staging, and `bindgen` C23 /
  Rust 2024 builder defaults.
- Each `optics-*-sys/build.rs` and wrapper `build.rs` reduces to a declarative
  invocation of `optics_build::probe_and_link` or `optics_build::relay_rpaths`.
- The root `Cargo.toml` declares `[workspace.dependencies]` for all internal
  crates and shared build dependencies (`bindgen`, `pkg-config`), so version
  metadata lives in one manifest and is validated by `tools/check_versions.py`.

---

## Consequences

### Positive
- Root tree symmetry (`libs/` alongside `crates/`) aligns with standard Rust
  workspace conventions and removes an empty nesting level.
- ~3,400 lines of duplicated build-script code are replaced by one tested crate
  (`crates/optics-build`), fixing silent `OPTICS_SOURCE_DIR` and `rpath` relay
  drift across libraries.
- Every crate name is crates.io-safe with zero breaking changes to consumers of
  the `optics` facade.
- Version bumps update `[workspace.package]` and `[workspace.dependencies]` in
  one `Cargo.toml` instead of editing every member manifest.

### Negative & Trade-offs
- Internal contributors importing sub-crates directly use `optics_*` names
  instead of bare subsystem names.

---

## Rejected Alternatives & Negative Knowledge

### 1. Rename `bindings/crates/` to `bindings/rust/`
- **Why considered**: Preserves `bindings/` as a language-keyed namespace.
- **Why rejected**: With `Cargo.toml` already at the repository root and Rust
  carrying substantial safe abstractions plus pure-Rust `optics-transit`,
  keeping crates two levels deep under `bindings/` obscures Rust's first-class
  role and fights standard Cargo workspace conventions.

### 2. Collapse all 18 crates into `optics-sys` + `optics`
- **Why considered**: Minimizes crate count and eliminates inter-crate `rpath`
  relays.
- **Why rejected**: Cargo allows only one `links` attribute per crate, so a
  single `optics-sys` cannot enforce per-C-library native link uniqueness; a
  monolithic `optics` crate also drops Rust compile-time enforcement of the
  `iris` / `lens` isolation boundary (`AGENTS.md` / ADR-0166) and degrades
  parallel incremental compilation.

---

## Links

- Amends: [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md), [ADR-0163](0163-repository-root-cargo-workspace.md)
- Related: [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md)
- Explanation: [Rust bindings](../explanation/rust-bindings.md)
- ADR Index: [ADR Index](index.md)
