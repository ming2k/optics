# Use Optics from a Rust Project

This guide adds Optics to a Rust project that lives outside the Optics
repository: it builds the C libraries, adds the `optics` crate, points the
build at those libraries, and runs the result. The Optics crates are not on
crates.io yet (see the
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
the full version, so a `0.0.86` install cannot serve `0.0.87` crates.

Either keep an uninstalled build tree:

```bash
git clone https://github.com/ming2k/optics && cd optics
git checkout v0.0.87
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

Pin the Git dependency to the tag or revision that matches the C build:

```toml
[dependencies]
# Full desktop application: window + UI + app shell (default features).
optics = { git = "https://github.com/ming2k/optics", tag = "v0.0.87" }
```

Pick a narrower set of modules with `default-features = false`:

```toml
# Headless rendering only: no window system, no UI toolkit.
optics = { git = "https://github.com/ming2k/optics", tag = "v0.0.87", default-features = false, features = ["gfx"] }
```

The [feature table](../explanation/rust-bindings.md#feature-flags--subsystem-mapping)
maps each feature to its module. Each module is re-exported under its feature
name (`optics::gfx`, `optics::ui`, `optics::app`, ...):

```rust
fn main() {
    println!("optics {}", optics::gfx::version());
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
optics = { git = "https://github.com/ming2k/optics", tag = "v0.0.87", features = ["static"] }
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
error while loading shared libraries: liboptics_gfx.so.0.0.87: cannot open shared object file
```

Use one of:

| Option | When to use it |
| :--- | :--- |
| Install Optics into `/usr` or `/usr/local` (and run `ldconfig` if your distribution needs it) | Deployed binaries, distribution packages |
| `LD_LIBRARY_PATH=<libdir>` (Linux), `DYLD_LIBRARY_PATH=<libdir>` (macOS) | One-off runs |
| An rpath baked in via `.cargo/config.toml` | Development against a local build or prefix |
| The `static` feature | Self-contained binaries |

The rpath option for an installed prefix on Linux:

```toml
[target.'cfg(target_os = "linux")']
rustflags = ["-C", "link-arg=-Wl,-rpath,/home/me/.local/optics/lib"]
```

An uninstalled tree keeps each module in its own directory
(`build/libs/gfx`, `build/libs/ui`, ...), so it needs one `-rpath` per enabled
module. An installed prefix needs only one.

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

- [Rust Bindings](../explanation/rust-bindings.md): the facade, the crate
  layout, and the publication gate.
- [Choose a Static or Shared Link](choose-a-static-or-shared-link.md): link
  modes in detail.
