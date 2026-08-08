# Rust Bindings

The Rust bindings live under `bindings/` in the Optics monorepo. They remain
separate Cargo workspaces so each library can keep a focused dependency graph
and package surface, while C and Rust changes can land atomically.

The root Meson build does not invoke Cargo. C-only consumers therefore do not
need a Rust toolchain.

## Workspaces and Crates

| Workspace | Crates | Role |
|-----------|--------|------|
| `bindings/flux-rs/` | `flux-sys`, `flux` | Raw FFI and safe rendering API. |
| | `flux-text-sys`, `flux-text`, `flux-text-layout` | Text FFI, shaping, and line layout. |
| | `flux-scene-graph-sys`, `flux-scene-graph` | glTF scene-graph FFI and safe wrapper. |
| `bindings/lens-rs/` | `lens-sys`, `lens` | Raw FFI and safe UI wrapper. |
| `bindings/iris-rs/` | `iris-sys`, `iris` | Raw FFI and safe application-host wrapper. |

The `*-sys` crates own native linking and bindgen output. Safe crates expose
RAII handles and Rust error types without duplicating the C implementation.

## Development Linking

Build the C stack first:

```bash
meson setup build -Dtests=true
meson compile -C build
```

Then point the binding build scripts at this checkout and its one Meson build
tree:

```bash
export FLUX_SOURCE_DIR="$PWD/libs/flux"
export LENS_SOURCE_DIR="$PWD"
export IRIS_SOURCE_DIR="$PWD"
export FLUX_BUILD_DIR="$PWD/build"
export LENS_BUILD_DIR="$PWD/build"
export IRIS_BUILD_DIR="$PWD/build"

cargo test --manifest-path bindings/flux-rs/Cargo.toml --workspace
cargo test --manifest-path bindings/lens-rs/Cargo.toml --workspace
cargo test --manifest-path bindings/iris-rs/Cargo.toml --workspace
```

The build scripts prepend `build/meson-uninstalled/` to `PKG_CONFIG_PATH` and
read public headers from `libs/`. This keeps bindgen and native linking on the
same checkout without running `meson install`.

Linking is per-platform: Linux uses `-Wl,-rpath` into the build/install lib
dir; macOS does the same (the C libraries carry `@rpath/` install names, so
the rpath is recorded as `LC_RPATH`); Windows has no rpath, so the build
scripts copy the dependent DLLs (`flux.dll`, `lens.dll`, …) next to the cargo
profile output (`target/<profile>/`, `deps/`, `examples/`) where the loader
finds them, and print a `cargo:warning` telling you to extend `PATH` if a DLL
cannot be staged.

For an installed stack, leave the source/build variables unset and make the
installed `.pc` files visible through the normal pkg-config search path.

## Change Workflow

When a public C API changes:

1. Update the owning header, implementation, and C test.
2. Update the matching `*-sys` declarations or bindgen allowlist.
3. Update the safe wrapper and add a Rust test.
4. Run the C suite and every affected Cargo workspace before committing.

The workspace READMEs contain crate-specific examples and feature notes.
