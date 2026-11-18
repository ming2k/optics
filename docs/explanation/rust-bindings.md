# Rust Bindings

The Rust binding crates live under `bindings/crates/` in the Optics monorepo.
They are organized as a single Cargo workspace whose manifest is at the
repository root ([ADR-0159](../adr/0159-unified-cargo-workspace-and-optics-facade-bindings.md),
[ADR-0163](../adr/0163-repository-root-cargo-workspace.md)), anchored by the
canonical `optics` facade crate. The root manifest is what makes Git
dependencies and repository-root `cargo`/Rust Analyzer work.

The root Meson build does not invoke Cargo. C-only consumers therefore do not
need a Rust toolchain.

## The Canonical Facade: `optics`

External consumers depend on a single crate: `optics`. Architectural layers and capabilities
are guarded by additive Cargo feature flags:

```toml
[dependencies]
# Full desktop application (defaults to ui + platform + motion)
optics = { git = "https://github.com/ming2k/optics" }

# Headless rendering foundation only (zero Wayland/windowing/UI dependencies)
optics = { git = "https://github.com/ming2k/optics", default-features = false, features = ["canvas"] }

# Headless UI layout and state machine (no OS window system)
optics = { git = "https://github.com/ming2k/optics", default-features = false, features = ["ui"] }
```

The Git form works because the Cargo workspace manifest is at the repository
root. A registry version will be documented once the publication gate below is
met; do not assume a crates.io release exists because a version number appears
in the workspace manifest.

### Feature Flags & Architectural Layer Mapping

| Feature | Layer / Subsystem | Re-exported Module | Underlying C Library |
| :--- | :--- | :--- | :--- |
| `canvas` | Layer 0: Rendering & RHI | `optics::canvas` | `libflux.so` |
| `typography` | Layer 1: HarfBuzz Text Shaping | `optics::typography` | `libglyph.so` |
| `ui` | Layer 3: Headless Immediate UI | `optics::ui` | `liblens.so` |
| `platform` | Layer 3: Native OS Window Shell | `optics::platform` | `libiris.so` |
| `materials` | Layer 2: Liquid Glass & Physical BRDFs | `optics::materials` | `libprism.so` |
| `scene` | Layer 2: 3D Scene Graph & Cameras | `optics::scene` | `libvista.so` |
| `reticle` | Document: Vector Art Compiler (SVG) | `optics::reticle` | `libreticle.so` |
| `motion` | Layer 2: Closed-Form Motion Math | `optics::motion` | Pure Safe Rust ([ADR-0158](../adr/0158-transit-header-only-and-zero-so-motion-vocabulary.md)) |

## Workspace Structure

The root `Cargo.toml` declares `members = ["bindings/crates/*"]` and owns
`Cargo.lock`. Under `bindings/crates/`, the internal `-sys` crates manage
low-level FFI and native dynamic library linkage, while safe wrapper crates
provide idiomatic C23-safe abstractions:

- `optics`: The top-level canonical facade crate.
- `flux-sys` / `flux`: Vulkan 1.3 RHI, 2D vector canvas, CPU fallback.
- `glyph-sys` / `glyph`: HarfBuzz text layout and glyph atlas.
- `lens-sys` / `lens`: Declarative immediate-mode UI, Flex/Grid layout, retained state machine.
- `iris-sys` / `iris`: Native Wayland, Win32, and Cocoa windowing and display pacing.
- `prism-sys` / `prism`: Refractive Liquid Glass, Frosted Glass, Acrylic, and Mica shaders.
- `reticle-sys` / `reticle`: Vector-art document compiler (ADR-0161).
- `vista-sys` / `vista`: 3D scene hierarchy and glTF runtime.
- `transit`: Pure Safe Rust closed-form harmonic springs, smoothing, and easing formulas.

## Version Lockstep

All crates share the workspace version (`0.0.81`) declared in the root
`Cargo.toml`. `tools/check_versions.py` compares that version with
`meson.build`, the umbrella header, and every library header; version drift is a
lint failure, not a convention.

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

The `-sys` build scripts automatically probe `build/meson-uninstalled/*.pc` and read headers from `libs/`.
Linux uses `-Wl,-rpath` into the local build tree, macOS injects `LC_RPATH`, and Windows stages dependent DLLs adjacent to target binaries.

## Publication Gate

The `-sys` crates currently invoke `bindgen` from `build.rs` and locate the C
libraries through `pkg-config`. That is the correct out-of-tree development
model, but it means a published crate would require every consumer to have a C
toolchain, the matching Optics libraries, and `libclang` installed. Publication
to crates.io therefore requires pre-generated bindings (or an equivalent
no-`libclang` path) plus a release matrix that validates installed-prefix
linking. Until that work lands, every crate sets `publish = false` and Git
dependencies are the supported distribution form.

## Change Workflow

When a public C API changes:

1. Update the owning C header, implementation, and C test.
2. Update the matching `*-sys` declarations or bindgen allowlist.
3. Update the safe wrapper and corresponding tests.
4. Verify via `cargo test --workspace` from the repository root.
