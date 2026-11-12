# Rust Bindings

The Rust bindings live under `bindings/` in the Optics monorepo. They are organized
as a single, unified Cargo workspace ([ADR-0159](../adr/0159-unified-cargo-workspace-and-optics-facade-bindings.md))
anchored by the canonical `optics` facade crate.

The root Meson build does not invoke Cargo. C-only consumers therefore do not
need a Rust toolchain.

## The Canonical Facade: `optics`

External consumers depend on a single crate: `optics`. Architectural layers and capabilities
are guarded by additive Cargo feature flags:

```toml
[dependencies]
# Full desktop application (defaults to ui + platform + motion)
optics = "0.0.78"

# Headless rendering foundation only (zero Wayland/windowing/UI dependencies)
optics = { version = "0.0.78", default-features = false, features = ["canvas"] }

# Headless UI layout and state machine (no OS window system)
optics = { version = "0.0.78", default-features = false, features = ["ui"] }
```

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

## Workspace Structure (`bindings/crates/`)

Under `bindings/crates/`, the internal `-sys` crates manage low-level FFI and native dynamic library linkage, while safe wrapper crates provide idiomatic C23-safe abstractions:

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

All crates within `bindings/` share the root workspace version (`0.0.78`) declared in `bindings/Cargo.toml`. Version drift between language bindings and C libraries is mechanically prohibited.

## Development Linking & Testing

Build the C stack first:

```bash
meson setup build -Dexamples=true -Dtests=true
meson compile -C build
```

Then run cargo from the unified workspace:

```bash
cargo test --manifest-path bindings/Cargo.toml --workspace
cargo clippy --manifest-path bindings/Cargo.toml --workspace --all-targets -- -D warnings
```

The `-sys` build scripts automatically probe `build/meson-uninstalled/*.pc` and read headers from `libs/`.
Linux uses `-Wl,-rpath` into the local build tree, macOS injects `LC_RPATH`, and Windows stages dependent DLLs adjacent to target binaries.

## Change Workflow

When a public C API changes:

1. Update the owning C header, implementation, and C test.
2. Update the matching `*-sys` declarations or bindgen allowlist.
3. Update the safe wrapper and corresponding tests.
4. Verify via `cargo test --manifest-path bindings/Cargo.toml --workspace`.
