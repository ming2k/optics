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

## Unified Fluent Flex Containers

`lens` provides a zero-allocation fluent builder directly on `f.row()` and `f.col()`:

```rust
// Horizontal flex container with chained layout, styling, and click handling:
let (response, ()) = f
    .row()
    .gap(8.0)
    .pad(4.0)
    .items_center()
    .bg(Color::rgba(40, 70, 130, 200))
    .rounded(6.0)
    .id("my_button_row")
    .show(|f| {
        f.label("Click me");
    });

if response.clicked {
    println!("Row clicked!");
}

// Vertical flex layout:
f.col()
    .gap(12.0)
    .pad(8.0)
    .flex(1.0)
    .show_flat(|f| {
        f.label("Header");
        f.label("Content");
    });
```

## Architectural & Design Principles

1. **Micro-kernel with Orthogonal Primitives**: Core `lens` provides atomic primitives (`row`, `col`, `scroll`, `button`, `switch`, `checkbox`, `textfield`, `modal`, `place`, `canvas`). Domain-specific arrangements (such as settings rows or list items) are composed at the call site rather than baked into the engine.
2. **Immediate Facade, Retained Core**: Frame code is written in a simple immediate-mode style (`Frame`), while the underlying engine maintains cross-frame node state, damage tracking, transition curves, and accessibility trees.
3. **Zero Heap Allocations on Hot Paths**: Builders, layouts, and frame draws execute on the stack or per-frame linear arenas without per-frame dynamic heap allocation.
4. **Modern UI Nomenclature**: Standardized on modern desktop semantics (`disclosure` / `expander` over historical terms). See [ADR-0081](../../docs/adr/0081-lens-unified-fluent-flex-containers-and-component-orthogonality.md).

## License

MIT, same as lens. See [LICENSE](LICENSE).

[flux-rs]: https://github.com/ming2k/flux-rs
