# lens-rs

Rust bindings to [**lens**][lens] — the C23 immediate-mode UI engine
that draws through [flux][flux]'s Vulkan canvas. Two crates:

| Crate            | Role                                                       |
|------------------|------------------------------------------------------------|
| [`lens-sys`]     | Raw bindgen FFI to `liblens`. Owns `links = "lens"`.       |
| [`lens`]         | Safe wrapper: RAII handles, `Result<T, Error>`, submodules.|

[lens]: https://github.com/ming2k/lens
[flux]: https://github.com/ming2k/flux
[`lens-sys`]: crates/lens-sys/
[`lens`]: crates/lens/

Separate from the C library by design — follows the same convention
as [flux-rs][flux-rs] and the broader ecosystem (openssl-sys, rusqlite,
gtk-rs all live outside the C source tree).

## Prerequisites

Build and install lens (which itself requires flux) first:

```sh
git clone https://github.com/ming2k/flux.git /path/to/flux
git clone https://github.com/ming2k/lens.git /path/to/lens
meson setup /path/to/flux/build /path/to/flux
meson compile -C /path/to/flux/build
meson install -C /path/to/flux/build
meson setup /path/to/lens/build /path/to/lens
meson compile -C /path/to/lens/build
meson install -C /path/to/lens/build     # puts lens.pc on PKG_CONFIG_PATH
```

Then:

```sh
cargo build
cargo test --workspace
```

## Dev mode (against non-installed meson build trees)

Skip `meson install` and link meson build trees directly:

```sh
export LENS_SOURCE_DIR=/path/to/lens
export LENS_BUILD_DIR=/path/to/lens/build
export FLUX_SOURCE_DIR=/path/to/flux
export FLUX_BUILD_DIR=/path/to/flux/build
cargo test --workspace
```

## License

MIT, same as lens. See [LICENSE](LICENSE).

[flux-rs]: https://github.com/ming2k/flux-rs
