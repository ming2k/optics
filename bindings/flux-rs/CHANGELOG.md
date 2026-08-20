# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **flux: bucket the canvas target attachment pool and bound the effect
  pools.** Target attachments were pooled per exact `(slot, w, h)`; a
  size-sweeping animation (resizes, reveals, window streams) missed every
  frame, thrashing `vkCreateImage` through the LRU tail and parking up to
  sixteen full-resolution RGBA16F intermediates per slot. Entries are now
  keyed by 128-pixel buckets, with the output-transform blit scaling its
  sampling UV into the written sub-rect, so a sweep keeps hitting one
  entry. The `flux_effect_blur` intermediate/output pools likewise grew
  unbounded — one pooled image per distinct `(format, w, h)` ever seen;
  they are now LRU-evicted above sixteen entries each (release parks
  resources on the device retire queue, so in-flight batches stay safe).

### Added

- `SurfaceColorOptions::offscreen_formats` constrains an offscreen
  surface's pixel container (most preferred first), and offscreen color
  spaces now negotiate deep containers by transfer function — RGB10A2 or
  RGBA16F for PQ/HLG, RGBA16F for linear, BGRA8 unchanged for gamma
  spaces. `SurfaceInfo::format` reports the winner (the DRM-fourcc key
  for dma-buf export), and deep containers read back as clamped RGBA8.
  16F effect inputs blur in linear light when the device advertises
  rgba16f storage.
- `DeviceOptions`, `DeviceFeatures`, and `Device::new_with_options` request
  required or optional DMA-BUF capabilities without exposing Vulkan extension
  bundles. `Device::enabled_features` reports the negotiated result, and
  `Device::drm_identity` returns the selected GPU's primary/render node
  identities without raw Vulkan queries.
- `Frame::request_readback` captures the exact frame being submitted on
  windowed and offscreen surfaces. `Surface::prepare_readback` can preallocate
  staging outside the trigger frame, while `Surface::read_pixels_ready`
  exposes non-blocking completion polling. `Surface::take_readback` detaches
  the mapped staging allocation so pixel copying and normalization can move to
  a worker without blocking presentation.
- **Software (CPU) canvas backend.** `Canvas::new_cpu(w, h, scale)` creates a
  headless canvas that renders on the CPU — no GPU, device, or window — mapped
  onto the C `flux_canvas_create_cpu` / `<flux/canvas_cpu.h>` (flux ≥ 0.2.4).
- **Backend-agnostic pass + readback.** `Canvas::begin_frame(Option<&Frame>,
  clear)` / `Canvas::end` drive GPU and CPU canvases with the same drawing
  code; `Canvas::begin_cpu(clear)` is the CPU shorthand. `Canvas::read_pixels`
  returns the CPU framebuffer as premultiplied RGBA8 (`None` on GPU). Also
  `Canvas::fill_rrect`.
- `flux-sys` now binds `<flux/canvas_cpu.h>` (the `flux_canvas_*_cpu`,
  `flux_canvas_begin_frame`/`end_frame`/`read_pixels` functions and the
  `flux_canvas_backend_kind` enum).
- Integration test `tests/cpu_canvas.rs` renders headless and checks pixels.
- `BlurredImage::as_raw` exposes the borrowed blur output pointer so sibling
  binding crates (prism) can consume it.

### Removed

- **Liquid glass moved to prism-rs.** The liquid-glass material left flux's C
  API (`flux_liquid_glass_*` is now `prism_liquid_glass_*` in libprism), so
  `LiquidGlassShape`, `LiquidGlassFocus`, `LiquidGlassGroup`,
  `LiquidGlassParams`, `LiquidGlassFilter`, and `LiquidGlassImage` are gone
  from this crate. The same safe API now lives in the `prism` crate
  (`bindings/prism-rs`), with `LiquidGlassParams::glare` renamed to
  `rim_light` to match the C rename.

## [0.1.1] - 2026-06-25

### Added

- **`italic` on `flux_text::Style`.** Mirrors the new `italic` field on the
  C `flux_text_style` (flux ≥ 0.2.3). New `Style::with_italic` and
  `Style::with_weight` builders; `to_sys` forwards the flag.
- **Full-width bracket compression test** in `flux-text-layout` covering the
  half-em trim for `（…）` runs.

## [0.1.0] - 2026-06-23

### Added

- **Initial extraction from the flux monorepo.** The five Rust crates
  (`flux-sys`, `flux`, `flux-text-sys`, `flux-text`, `flux-text-layout`)
  previously lived under `crates/` in the [flux][flux] C source tree.
  They are now a separate repository, following the industry convention
  for Rust bindings to C libraries (openssl-sys, libsqlite3-sys,
  rust-curl, gtk-rs all live outside the C source).

  The in-tree build script assumed the C source root was the parent of
  `crates/flux-sys/`; that assumption no longer holds. The build scripts
  now take an explicit `FLUX_SOURCE_DIR` environment variable for the C
  source checkout and default to pkg-config installed mode otherwise
  (`FLUX_USE_INSTALLED=1` semantics, which is the openssl-sys / rusqlite
  default). `FLUX_BUILD_DIR` continues to point at a meson build tree
  for dev linking.

- **`flux_text_layout::wrap_with`** — a per-atom measure hook for the
  greedy wrapper. `wrap` (the existing API) becomes a convenience that
  measures each atom as raw source; callers whose drawn representation
  differs from the source (hidden inline markup, per-run weight/family)
  now have the extension point a future optimal-breaker will also consume.

- **Inter-script auto-space in `wrap`.** CJK↔non-CJK atom boundaries with
  no separator now contribute a ¼-em gap to the line width, mirroring
  `txt_run_autospace_em` in libflux's `text/layout.c`. Without it wrap
  would under-count and pack a line that overflows when drawn.

[flux]: https://github.com/ming2k/flux
