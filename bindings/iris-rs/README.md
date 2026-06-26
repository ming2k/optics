# iris-rs

Rust bindings to [**iris**][iris] — the C23 L3 application toolkit that
turns [lens][lens] + [flux][flux] into something you can write a
complete desktop application against. Two crates:

| Crate            | Role                                                       |
|------------------|------------------------------------------------------------|
| [`iris-sys`]     | Raw bindgen FFI to `libiris`. Owns `links = "iris"`.       |
| [`iris`]         | Safe wrapper: RAII `App`, event loop, paint callbacks.     |

[iris]: https://github.com/ming2k/iris
[lens]: https://github.com/ming2k/lens
[flux]: https://github.com/ming2k/flux
[`iris-sys`]: crates/iris-sys/
[`iris`]: crates/iris/

Separate from the C library by design — follows the same convention as
[flux-rs][flux-rs] and [lens-rs][lens-rs].

## Prerequisites

Build and install the stack bottom-up (flux → lens → iris):

```sh
git clone https://github.com/ming2k/flux.git /path/to/flux
git clone https://github.com/ming2k/lens.git /path/to/lens
git clone https://github.com/ming2k/iris.git /path/to/iris
meson setup /path/to/flux/build /path/to/flux && meson compile -C /path/to/flux/build && meson install -C /path/to/flux/build
meson setup /path/to/lens/build /path/to/lens && meson compile -C /path/to/lens/build && meson install -C /path/to/lens/build
meson setup /path/to/iris/build /path/to/iris && meson compile -C /path/to/iris/build && meson install -C /path/to/iris/build
```

Then:

```sh
cargo build
cargo test --workspace
cargo run --example hello
```

## Dev mode (against non-installed meson build trees)

Skip `meson install` and link meson build trees directly:

```sh
export IRIS_SOURCE_DIR=/path/to/iris
export IRIS_BUILD_DIR=/path/to/iris/build
export LENS_SOURCE_DIR=/path/to/lens
export LENS_BUILD_DIR=/path/to/lens/build
export FLUX_SOURCE_DIR=/path/to/flux
export FLUX_BUILD_DIR=/path/to/flux/build
cargo test --workspace
```

## License

MIT, same as iris. See [LICENSE](LICENSE).

[flux-rs]: https://github.com/ming2k/flux-rs
[lens-rs]: https://github.com/ming2k/lens-rs
