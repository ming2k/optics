# prism-rs

Rust bindings to [**prism**][prism] — the C23 material library of the optics
stack (analytic liquid glass), built on [flux][flux]'s public effect
runtime. Two crates:

| Crate          | Role                                                         |
|----------------|--------------------------------------------------------------|
| [`prism-sys`]  | Raw bindgen FFI to `libprism`. Owns `links = "prism"`.       |
| [`prism`]      | Safe wrapper: RAII filter handle, `Result<T, flux::Error>`.  |

[prism]: https://github.com/ming2k/prism
[flux]: https://github.com/ming2k/flux
[`prism-sys`]: crates/prism-sys/
[`prism`]: crates/prism/

flux types are not duplicated: prism's C API speaks `flux_image`,
`flux_result`, and friends, and `prism-sys` re-exports those from
[`flux-sys`][flux-rs] (bindgen blocklist + re-export) instead of generating
a second copy. There is exactly one Rust definition of `flux_result` across
the stack, so the safe crate reuses `flux::Error` unchanged and hands
`flux::Device` / `flux::Frame` / `flux::BlurredImage` to libprism without
casts.

Separate from the C library by design — follows the same convention
as [flux-rs][flux-rs] and the broader ecosystem (openssl-sys, rusqlite,
gtk-rs all live outside the C source tree).

## Prerequisites

Build and install flux and prism first (prism has `Requires: flux`):

```sh
git clone https://github.com/ming2k/flux.git /path/to/flux
meson setup /path/to/flux/build /path/to/flux
meson compile -C /path/to/flux/build
meson install -C /path/to/flux/build
git clone https://github.com/ming2k/prism.git /path/to/prism
meson setup /path/to/prism/build /path/to/prism
meson compile -C /path/to/prism/build
meson install -C /path/to/prism/build    # puts prism.pc on PKG_CONFIG_PATH
```

Then:

```sh
cargo build
cargo test --workspace
```

## Dev mode (against non-installed meson build trees)

Skip `meson install` and link meson build trees directly:

```sh
export PRISM_SOURCE_DIR=/path/to/prism
export PRISM_BUILD_DIR=/path/to/prism/build
export FLUX_SOURCE_DIR=/path/to/flux
export FLUX_BUILD_DIR=/path/to/flux/build
cargo test --workspace
```

`PRISM_BUILD_DIR/meson-uninstalled/prism-uninstalled.pc` must exist (meson
creates it at `meson setup` time). `PRISM_SOURCE_DIR/include/` is prepended
to bindgen's include path so the generated bindings match that exact
checkout. When the bindings live inside the optics monorepo both are found
automatically by walking up from the crate directory, so no exports are
needed there.

## License

MIT, same as prism. See [LICENSE](LICENSE).

[flux-rs]: https://github.com/ming2k/flux-rs
