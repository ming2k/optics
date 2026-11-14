---
id: ADR-0163
title: "Repository-Root Cargo Workspace and a Honest Git-Dependency Boundary"
status: accepted
date: 2026-11-15
scope: Cargo.toml, bindings/, .github/workflows/ci.yml, docs/
superseded_by: null
negative_knowledge: true
---

# ADR-0163: Repository-Root Cargo Workspace and a Honest Git-Dependency Boundary

- Status: Accepted
- Date: 2026-11-15
- Scope: `Cargo.toml`, `bindings/`, `.github/workflows/ci.yml`, `docs/`
- Amends: [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md)

## Context

ADR-0159 correctly collapsed eight independent Cargo workspaces into one
workspace and established the `optics` facade. That decision left the workspace
root at `bindings/Cargo.toml`.

The location is coherent for a C-first repository, but it creates two concrete
boundaries that the tree cannot solve by documentation:

1. **Git dependencies are impossible.** Cargo resolves a `git = "https://..."`
   dependency by reading `Cargo.toml` at the repository root. It has no
   subdirectory syntax. A consumer therefore cannot depend on `optics` from a
   Git checkout unless the workspace manifest is at the root.
2. **Repository-root tooling cannot discover the workspace.** Opening the
   repository root in Rust Analyzer does not scan arbitrary subdirectories; it
   needs `rust-analyzer.linkedProjects` or a Cargo manifest at the root.
   Running `cargo test` from the root likewise fails.

The original layout also encouraged a false mental model: that `bindings/` is
the Rust project. It is not. `bindings/` is the language-binding namespace; the
Rust crates happen to be its only current consumer. The workspace belongs to the
repository, not to a directory named after one binding language.

## Decision

Move the single Cargo workspace root to the repository root.

1. **One workspace, one lockfile.** `Cargo.toml` and `Cargo.lock` live at the
   repository root. `members = ["bindings/crates/*"]` names every crate. The
   former `bindings/Cargo.toml` and `bindings/Cargo.lock` are deleted; nested
   workspaces are prohibited.
2. **`bindings/` remains the binding namespace.** Crate sources stay at
   `bindings/crates/<crate>/`. The directory answers "which language binding is
   this?"; the root manifest answers "what is the workspace?". No cosmetic
   rename to `bindings/rust/` is performed while Rust is the only binding
   language.
3. **C-first build order is unchanged.** The root Meson build still does not
   invoke Cargo. Rust `-sys` build scripts continue to probe
   `build/meson-uninstalled/*.pc` or an installed prefix. Moving the manifest
   does not make Meson and Cargo co-owners of the C build graph.
4. **CI and documentation use the root.** CI runs `cargo fmt`, `cargo clippy`,
   and `cargo test` from the checkout root. Release documentation names the root
   `Cargo.toml` as the Rust version source.
5. **Published crates are a separate gate.** This ADR makes Git dependencies
   possible; it does not claim that the current `-sys` crates are ready for
   crates.io. Their build scripts invoke `bindgen` and require a C toolchain and
   `libclang` at consumer build time. Publication requires pre-generated
   bindings (or an equivalent no-libclang path) and is tracked as its own
   decision, not smuggled into this move.

## Consequences

Positive:

- A consumer can depend on the facade with a Git URL and no subdirectory
  workaround.
- `cargo test --workspace`, `cargo clippy --workspace`, and Rust Analyzer work
  from the repository root with no editor-specific `linkedProjects` file.
- The root `Cargo.lock` makes the tested Rust dependency graph explicit at the
  same level as `meson.build`.

Negative / neutral:

- The repository root gains `Cargo.toml`, `Cargo.lock`, and an ignored `target/`
  directory. This is the standard Cargo layout and is preferable to a hidden
  second workspace.
- Every command that used `--manifest-path bindings/Cargo.toml` must be updated
  in the same change. There is no compatibility shim.

## Rejected Alternatives

### 1. Keep `bindings/Cargo.toml` and document `cd bindings`

Rejected. It preserves the status quo but leaves Git dependencies impossible and
Rust Analyzer root discovery broken. Documentation cannot make Cargo look in a
subdirectory it does not support.

### 2. Add a root `Cargo.toml` that includes the existing `bindings` workspace

Rejected. Cargo does not support nested workspaces; this produces multiple
workspace roots and ambiguous lockfile ownership. The move is a relocation, not
an additional layer.

### 3. Publish the crates first and defer the workspace move

Rejected. Publication does not require a root manifest, and the current
`-sys` crates are not publish-ready without pre-generated bindings. Mixing the
two changes would hide the actual publication blocker behind a directory move.

## Links

- [ADR-0159](0159-unified-cargo-workspace-and-optics-facade-bindings.md): unified Cargo workspace and facade bindings
- [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md): C umbrella distribution
- [Rust bindings](../explanation/rust-bindings.md)
- [Testing](../dev/testing.md#rust-workspaces)
