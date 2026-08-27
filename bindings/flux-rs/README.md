# flux-rs

Rust bindings to [**flux**][flux] — the C23, Vulkan-first 2D/3D graphics
library. Eight crates provide the native wrappers and companion layers:

| Crate              | Role                                                          |
|--------------------|---------------------------------------------------------------|
| [`flux-sys`]       | Raw bindgen FFI to `libflux`. Owns `links = "flux"`.          |
| [`flux`]           | Safe wrapper: RAII handles, `Result<T, Error>`.              |
| [`flux-text-sys`]  | Raw bindgen FFI to `libflux-text` (HarfBuzz shaping sibling). |
| [`flux-text`]      | Safe wrapper over `flux-text-sys`, Layer-0 shaping surface.  |
| [`flux-text-layout`] | Pure-Rust Layer-1 line wrapping on top of `flux-text`.     |
| [`flux-scene-graph-sys`] | Raw bindgen FFI to `libflux-scene-graph`.          |
| [`flux-scene-graph`] | Safe glTF scene, material/texture, animation, bounds, and drawing layer. |
| [`flux-composition-graph`] | Pure-Rust offscreen DAG, ROI, damage, and target-lifetime planner above Flux operators. |

[flux]: https://github.com/ming2k/flux
[`flux-sys`]: crates/flux-sys/
[`flux`]: crates/flux/
[`flux-text-sys`]: crates/flux-text-sys/
[`flux-text`]: crates/flux-text/
[`flux-text-layout`]: crates/flux-text-layout/
[`flux-scene-graph-sys`]: crates/flux-scene-graph-sys/
[`flux-scene-graph`]: crates/flux-scene-graph/
[`flux-composition-graph`]: crates/flux-composition-graph/

This repository is **separate** from the C library by design — it
follows the industry convention (openssl, sqlite, curl, gtk all keep
their Rust bindings out of the C source tree). The C library has its
own release cadence and ABI stability policy; these bindings track it
via pkg-config.

## Prerequisites

Build and install flux first:

```sh
git clone https://github.com/ming2k/flux.git /path/to/flux
meson setup /path/to/flux/build /path/to/flux
meson compile -C /path/to/flux/build
meson install -C /path/to/flux/build     # puts flux.pc on PKG_CONFIG_PATH
```

Then:

```sh
cargo build
cargo test --workspace
```

## Dev mode (against a non-installed flux build)

Skip `meson install` and link a meson build tree directly:

```sh
export FLUX_SOURCE_DIR=/path/to/flux
export FLUX_BUILD_DIR=/path/to/flux/build
cargo test --workspace
```

`FLUX_BUILD_DIR/meson-uninstalled/flux-uninstalled.pc` must exist (meson
creates it at `meson setup` time). `FLUX_SOURCE_DIR/include/` is
prepended to bindgen's include path so the generated bindings match
that exact checkout.

## Headless (CPU) rendering

`Canvas::new_cpu` renders on the CPU — no GPU, device, or window — and
`read_pixels` returns premultiplied RGBA8. The drawing calls are the same
ones a GPU canvas uses; only create, the (absent) frame, and readback differ.

```rust
use flux::{rgba, Canvas};

let c = Canvas::new_cpu(256, 256, 1.0)?;
c.begin_cpu(Some(rgba(20, 20, 30, 255)))?;      // clear (or begin_frame(None, ..))
c.fill_rrect(16.0, 16.0, 224.0, 224.0, 24.0, rgba(255, 90, 40, 255));
c.end();
let (w, h, stride, px) = c.read_pixels().expect("CPU pixels");
// … encode `px` (RGBA8) to PNG, hash it, upload it, …
```

Vector only: image and glyph (text) draws are ignored on a CPU canvas
(they need a GPU-resident texture). See `tests/cpu_canvas.rs`.

## Live offscreen effects

The safe `flux` crate exposes sampleable `Image::render_target` images,
`Frame::begin_image_scene_pass` for depth-tested 3D composition, and a
frame-slot-safe `BlurFilter`. Together they support a live compositor path
without transient-pool growth or device-wide waits: Canvas background → scene
pass → Canvas foreground → fixed-cost Dual-Kawase blur → final composite.

## glTF and VRM materials

`Scene::from_glb_with_materials` decodes referenced embedded PNG/JPEG
base-color textures with bounded resource limits and creates per-primitive
UNLIT or PHONG materials for a specified render target. The loader maps sRGB
base color, UV0 plus `KHR_texture_transform`, glTF samplers, alpha modes,
alpha cutoff, and double-sided state. Use `Scene::draw_materials` to render the
installed material table; `Scene::draw` remains the one-material override.

## License

MIT, same as flux. See [LICENSE](LICENSE).
