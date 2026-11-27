# Use Optics from a Rust Project

This guide adds Optics to a Rust project that lives outside the Optics
repository: it builds the C libraries, adds the matching `optics-*` crate,
points the build at those libraries, and runs the result. The Optics crates are
not on crates.io yet (see the
[publication gate](../explanation/rust-bindings.md#publication-gate)), so the
supported form is a Git dependency.

---

## 1. Install the prerequisites

The `optics-*-sys` crates generate their bindings with `bindgen` and find the
C libraries with `pkg-config`, so a Rust project needs the same toolchain as a
C one, plus:

- Rust 1.85 or newer;
- `pkg-config`;
- `libclang` (bindgen loads it at build time; set `LIBCLANG_PATH` if it is not
  on the default search path);
- the system development packages from
  [Getting Started](../tutorials/01-getting-started.md#step-1--prepare-the-environment).

---

## 2. Build the Optics C libraries

Build the C stack at the **same version** as the crates you depend on.
`optics-build` probes with `pkg-config --exact-version`, and the SONAMEs carry
the full version, so a `0.0.89` install cannot serve `0.0.90` crates.

Either keep an uninstalled build tree:

```bash
git clone https://github.com/ming2k/optics && cd optics
git checkout v0.0.90
meson setup build -Dtests=false
meson compile -C build
```

or install it into a prefix:

```bash
meson setup build -Dtests=false --prefix="$HOME/.local/optics"
meson compile -C build
meson install -C build
```

The default `default_library=both` builds both the shared objects and the
static archives, so either link mode in step 4 works.

---

## 3. Add the dependency

Pin the Git dependency to the tag or revision that matches the C build, picking
the `optics-*` crate for the layer you need:

```toml
[dependencies]
# Full desktop application: window + UI + app shell (re-exports gfx, ui, window).
optics-app = { git = "https://github.com/ming2k/optics", tag = "v0.0.90" }
```

For narrower layers, depend directly on the lower crate:

```toml
# Headless rendering only: no window system, no UI toolkit.
optics-gfx = { git = "https://github.com/ming2k/optics", tag = "v0.0.90" }
```

The [crate table](../explanation/rust-bindings.md#crate--subsystem-mapping)
lists every crate and the lower crates it re-exports (`optics_app::ui`,
`optics_app::window`, `optics_app::gfx`, ...). With `optics-app`:

```rust
fn main() {
    println!("optics {}", optics_app::gfx::version());
}
```

---

## 4. Point the build at the libraries

Cargo checks the dependency out without a Meson build tree, so tell the build
scripts where the libraries are. Use the variable that matches step 2:

| Optics build | Set |
| :--- | :--- |
| Uninstalled tree | `OPTICS_BUILD_DIR=/path/to/optics/build` |
| Installed prefix | `PKG_CONFIG_PATH=$HOME/.local/optics/lib/pkgconfig` (the libdir may be `lib64` or `lib/<triplet>`) |
| System install (`/usr`, `/usr/local`) | nothing |

Set them for every build of the project in `.cargo/config.toml`, so that
`cargo build`, `cargo test`, and Rust Analyzer all see them:

```toml
[env]
OPTICS_BUILD_DIR = "/path/to/optics/build"
```

Then choose the link mode. The default links the shared objects; the `static`
feature embeds the Optics archives instead:

```toml
optics-app = { git = "https://github.com/ming2k/optics", tag = "v0.0.90", features = ["static"] }
```

[Choose a Static or Shared Link](choose-a-static-or-shared-link.md) explains
the trade-off and the `OPTICS_STATIC` / `OPTICS_DYNAMIC` overrides.

---

## 5. Make the shared libraries loadable

A static link has nothing to load at runtime, so skip this step if you enabled
`static`.

A shared link only records the library names in your binary. Cargo does not pass
the Optics library directory on to it, so unless Optics is installed in a
directory the loader already searches, the binary builds and then fails to
start:

```text
error while loading shared libraries: liboptics_gfx.so.0.0.90: cannot open shared object file
```

Use one of:

| Option | When to use it |
| :--- | :--- |
| Install Optics into `/usr` or `/usr/local` (and run `ldconfig` if your distribution needs it) | Deployed binaries, distribution packages |
| `LD_LIBRARY_PATH=<libdir>` (Linux), `DYLD_LIBRARY_PATH=<libdir>` (macOS) | One-off runs |
| Relay `DEP_OPTICS_<MOD>_RS_RPATHS` in your crate's `build.rs` | Development and CI against an uninstalled Meson build tree (no hardcoded paths) |
| An rpath baked in via `.cargo/config.toml` | Development against an installed prefix |
| The `static` feature | Self-contained binaries |

Each native `optics-*` wrapper crate declares `links = "optics_<mod>_rs"` and
publishes the non-system library directories it linked against as a
semicolon-separated `DEP_OPTICS_<MOD>_RS_RPATHS` environment variable (empty
during a static link). A binary crate depending directly on `optics-app` can
bake those directories in automatically from its `build.rs` with zero extra
build dependencies:

```rust
// build.rs
fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    if let Ok(rpaths) = std::env::var("DEP_OPTICS_APP_RS_RPATHS") {
        let dirs: Vec<&str> = rpaths.split(';').filter(|d| !d.is_empty()).collect();
        if !dirs.is_empty() {
            if std::env::var("CARGO_CFG_TARGET_OS").as_deref() != Ok("macos") {
                println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
            }
            for dir in dirs {
                println!("cargo:rustc-link-arg=-Wl,-rpath,{dir}");
            }
        }
    }
}
```

Alternatively, for an installed prefix on Linux, bake one rpath in
`.cargo/config.toml`:

```toml
[target.'cfg(target_os = "linux")']
rustflags = ["-C", "link-arg=-Wl,-rpath,/home/me/.local/optics/lib"]
```

An uninstalled tree keeps each module in its own directory
(`build/libs/gfx`, `build/libs/ui`, ...), so `.cargo/config.toml` needs one
`-rpath` per enabled module, whereas `DEP_OPTICS_<MOD>_RS_RPATHS` (or an
installed prefix) handles all of them automatically.

On Windows, the `-sys` build scripts copy the Optics DLLs next to the binaries in
Cargo's target directory, so `cargo run` and `cargo test` work without extra
setup. A deployed binary needs the DLLs next to it or on `PATH`.

---

## 6. Build and run

```bash
cargo run
```

To confirm which link you got on Linux, inspect the binary's dependencies:

```bash
ldd target/debug/<binary> | grep liboptics   # no output: static link
```

---

## Troubleshooting

| Symptom | Cause | Fix |
| :--- | :--- | :--- |
| `pkg-config failed for optics-gfx` | The build cannot see any Optics install | Set `OPTICS_BUILD_DIR` or `PKG_CONFIG_PATH` (step 4) |
| `pkg-config failed for optics-gfx` although `pkg-config --modversion optics-gfx` succeeds | The C build and the crate revision differ: the probe requires the exact crate version | Build the C stack from the same tag as the Git dependency (step 2) |
| `Unable to find libclang` | bindgen cannot load `libclang` | Install the clang development package or set `LIBCLANG_PATH` |
| `cannot open shared object file` when the binary starts | Shared link, library directory not on the loader path | Step 5 |
| Link or mode errors naming `OPTICS_STATIC`, `OPTICS_DYNAMIC`, or the `static` feature | Link-mode request the tree cannot satisfy | [Choose a Static or Shared Link](choose-a-static-or-shared-link.md#troubleshooting) |

---

## See also

- [Rust Bindings](../explanation/rust-bindings.md): the crate layout,
  re-exports, and the publication gate.
- [Choose a Static or Shared Link](choose-a-static-or-shared-link.md): link
  modes in detail.
