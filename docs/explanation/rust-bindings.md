# Rust Bindings

The Rust crates live under repository-root `crates/`, symmetric with the C23
libraries under `libs/`. They are organized as a single Cargo workspace whose
manifest is at the repository root
([ADR-0159](../adr/0159-unified-cargo-workspace-and-optics-facade-bindings.md),
[ADR-0163](../adr/0163-repository-root-cargo-workspace.md),
[ADR-0169](../adr/0169-root-crates-layout-optics-namespace-and-shared-build.md)),
anchored by the canonical `optics` facade crate. The root manifest is what makes
Git dependencies and repository-root `cargo`/Rust Analyzer work.

The root Meson build does not invoke Cargo. C-only consumers therefore do not
need a Rust toolchain.

## The Canonical Facade: `optics`

External consumers depend on a single crate: `optics`. Architectural layers and
capabilities are guarded by additive Cargo feature flags:

```toml
[dependencies]
# Full desktop application (defaults to app + motion)
optics = { git = "https://github.com/ming2k/optics" }

# Headless rendering foundation only (zero Wayland/windowing/UI dependencies)
optics = { git = "https://github.com/ming2k/optics", default-features = false, features = ["gfx"] }

# Headless UI layout and state machine (no OS window system)
optics = { git = "https://github.com/ming2k/optics", default-features = false, features = ["ui"] }
```

The Git form works because the Cargo workspace manifest is at the repository
root. [Use Optics from a Rust Project](../how-to/use-optics-from-rust.md) walks
through the full setup: building the C libraries, pointing the build at them,
and running the result. A registry version will be documented once the publication gate below is
met; do not assume a crates.io release exists because a version number appears
in the workspace manifest.

### Feature Flags & Subsystem Mapping

| Feature | Subsystem | Re-exported Module | Internal Crate | Underlying C Library |
| :--- | :--- | :--- | :--- | :--- |
| `gfx` | Rendering & RHI | `optics::gfx` | `optics-gfx` | `liboptics_gfx` |
| `text` | HarfBuzz Text Shaping | `optics::text` | `optics-text` | `liboptics_text` |
| `ui` | Headless Immediate UI | `optics::ui` | `optics-ui` | `liboptics_ui` |
| `window` | Native OS Window Shell | `optics::window` | `optics-window` | `liboptics_window` |
| `app` | Application Shell | `optics::app` | `optics-app` | `liboptics_app` |
| `material` | Liquid Glass & Physical BRDFs | `optics::material` | `optics-material` | `liboptics_material` |
| `scene` | 3D Scene Graph & Cameras | `optics::scene` | `optics-scene` | `liboptics_scene` |
| `svg` | Vector Art Compiler (SVG) | `optics::svg` | `optics-svg` | `liboptics_svg` |
| `motion` | Closed-Form Motion Math | `optics::motion` | `optics-motion` | Pure Safe Rust ([ADR-0158](../adr/0158-transit-header-only-and-zero-so-motion-vocabulary.md)) |

## Workspace Structure

The root `Cargo.toml` declares `members = ["crates/*"]`, centralizes internal
and build dependencies in `[workspace.dependencies]`, and owns `Cargo.lock`.
Under `crates/`, every sub-crate carries the collision-free `optics-*` prefix
and mirrors the one-way dependency graph of `libs/`:

- `optics`: The top-level canonical facade crate.
- `optics-build`: Internal build-script library shared by all `optics-*-sys`
  and wrapper crates (Meson probing, shared linking by default or static linking
  with the `static` feature, `OPTICS_STATIC` / `OPTICS_DYNAMIC` overrides, rpath
  relay, Windows DLL staging, and `bindgen` defaults). See
  [Choose a Static or Shared Link](../how-to/choose-a-static-or-shared-link.md).
- `optics-gfx-sys` / `optics-gfx`: Vulkan 1.3 RHI, 2D vector canvas, CPU fallback.
- `optics-text-sys` / `optics-text`: HarfBuzz text layout and glyph atlas.
- `optics-ui-sys` / `optics-ui`: Declarative immediate-mode UI, Flex/Grid layout, retained state machine.
- `optics-window-sys` / `optics-window`: Native Wayland, Win32, and Cocoa windowing and display pacing.
- `optics-app-sys` / `optics-app`: Application shell joining `window` and `ui`.
- `optics-material-sys` / `optics-material`: Refractive Liquid Glass, Frosted Glass, Acrylic, and Mica shaders.
- `optics-svg-sys` / `optics-svg`: Vector-art document compiler (ADR-0161, ADR-0170).
- `optics-scene-sys` / `optics-scene`: 3D scene hierarchy and glTF runtime.
- `optics-motion`: Pure Safe Rust closed-form harmonic springs, smoothing, and easing formulas (no C FFI, no `build.rs`).

## Version Lockstep

All crates share the workspace version (`0.0.85`) declared in the root
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
