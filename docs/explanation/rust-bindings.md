# Rust Bindings

The Rust crates live under repository-root `crates/`, symmetric 1:1 with the C23
libraries under `libs/`. They are organized as a single Cargo workspace whose
manifest is at the repository root
([ADR-0159](../adr/0159-unified-cargo-workspace-and-optics-facade-bindings.md),
[ADR-0163](../adr/0163-repository-root-cargo-workspace.md),
[ADR-0169](../adr/0169-root-crates-layout-optics-namespace-and-shared-build.md),
[ADR-0176](../adr/0176-retire-rust-optics-facade-crate.md)). The root manifest
is what makes Git dependencies and repository-root `cargo`/Rust Analyzer work.

The root Meson build does not invoke Cargo. C-only consumers therefore do not
need a Rust toolchain.

## Direct Orthogonal Crates

External consumers depend directly on the `optics-*` crate that matches the
layer they need; upper crates re-export the safe crates whose types appear in
their public API (`optics_app::{gfx, ui, window}`, `optics_ui::gfx`,
`optics_text::gfx`):

```toml
[dependencies]
# Full desktop application (window + UI + canvas recording)
optics-app = { git = "https://github.com/ming2k/optics" }

# Headless rendering foundation only (zero Wayland/windowing/UI dependencies)
optics-gfx = { git = "https://github.com/ming2k/optics" }

# Headless UI layout and state machine (no OS window system)
optics-ui = { git = "https://github.com/ming2k/optics" }
```

The Git form works because the Cargo workspace manifest is at the repository
root. [Use Optics from a Rust Project](../how-to/use-optics-from-rust.md) walks
through the full setup: building the C libraries, pointing the build at them,
and running the result. A registry version will be documented once the publication gate below is
met; do not assume a crates.io release exists because a version number appears
in the workspace manifest.

### Crate & Subsystem Mapping

| Crate | Subsystem | Re-exports | Underlying C Library |
| :--- | :--- | :--- | :--- |
| `optics-gfx` | Rendering & RHI | `sys` | `liboptics_gfx` |
| `optics-text` | HarfBuzz Text Shaping | `gfx`, `sys` | `liboptics_text` |
| `optics-ui` | Headless Immediate UI & Motion (`optics_ui::motion`) | `gfx`, `sys` | `liboptics_ui` |
| `optics-window` | Native OS Window Shell | `sys` | `liboptics_window` |
| `optics-app` | Application Shell | `gfx`, `ui`, `window`, `sys` | `liboptics_app` |

## Workspace Structure

The root `Cargo.toml` declares `members = ["crates/*"]`, centralizes internal
and build dependencies in `[workspace.dependencies]`, and owns `Cargo.lock`.
Under `crates/`, every crate carries the collision-free `optics-*` prefix
and mirrors the one-way dependency graph of `libs/`:

- `optics-build`: Internal build-script library shared by all `optics-*-sys`
  and wrapper crates (Meson probing, shared linking by default or static linking
  with the `static` feature, `OPTICS_STATIC` / `OPTICS_DYNAMIC` overrides, rpath
  relay, Windows DLL staging, and `bindgen` defaults). See
  [Choose a Static or Shared Link](../how-to/choose-a-static-or-shared-link.md).
- `optics-gfx-sys` / `optics-gfx`: Vulkan 1.3 RHI, 2D vector canvas, CPU fallback.
- `optics-text-sys` / `optics-text`: HarfBuzz text layout and glyph atlas.
- `optics-ui-sys` / `optics-ui`: Declarative immediate-mode UI, Flex/Grid layout, retained state machine, and pure Safe Rust closed-form motion (`optics_ui::motion`, [ADR-0177](../adr/0177-absorb-motion-into-ui.md)).
- `optics-window-sys` / `optics-window`: Native Wayland, Win32, and Cocoa windowing and display pacing.
- `optics-app-sys` / `optics-app`: Application shell joining `window` and `ui`.

## Version Lockstep

All crates share the workspace version (`0.0.91`) declared in the root
`Cargo.toml` and inherit internal versions through `[workspace.dependencies]`.
`tools/check_versions.py` compares that version with `meson.build`, the umbrella
header, and every library header; version drift or nested workspace layouts are
lint failures, not conventions.

## Development Linking & Testing

Build the C stack first:

```bash
meson setup build -Dexamples=true -Dtests=true
meson compile -C build
```

Then run cargo from the repository root:

```bash
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

The `optics-*-sys` build scripts automatically probe
`build/meson-uninstalled/*.pc` and read headers from `libs/` via `optics-build`.
Linux uses `-Wl,-rpath` into the local build tree, macOS injects `LC_RPATH`, and
Windows stages dependent DLLs adjacent to target binaries.

## Publication Gate

The `optics-*-sys` crates currently invoke `bindgen` from `build.rs` and locate
the C libraries through `pkg-config`. That is the correct out-of-tree
development model, but it means a published crate would require every consumer
to have a C toolchain, the matching Optics libraries, and `libclang` installed.
Publication to crates.io therefore requires pre-generated bindings (or an
equivalent no-`libclang` path) plus a release matrix that validates
installed-prefix linking. Until that work lands, every crate sets
`publish = false` and Git dependencies are the supported distribution form.

## Change Workflow

When a public C API changes:

1. Update the owning C header, implementation, and C test.
2. Update the matching `optics-*-sys` declarations or bindgen allowlist under `crates/`.
3. Update the safe `optics-*` wrapper and corresponding tests.
4. Verify via `cargo test --workspace` from the repository root.
