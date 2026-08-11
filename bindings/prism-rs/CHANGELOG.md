# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Per-group material overrides and adaptive-plate inputs.**
  `LiquidGlassGroup` gains `frost_strength`, `tint_strength`, `saturation`,
  `plate_polarity`, and `backdrop_energy` as `Option<f32>`; `None` maps to
  the C header's `< 0` inherit/disabled sentinel, `Some` is used verbatim.
  `plate_polarity` pins the caller-chosen tint polarity (0 = smoke, 1 =
  pearl) uniformly for the whole body. The group-to-raw mapping is now
  public as `LiquidGlassGroup::as_raw`.
- **Backdrop statistics.** `BackdropStats` and
  `LiquidGlassFilter::stats(frame, out)` read the per-group GPU reduction
  (mean blurred-backdrop luminance and high-frequency energy) the frame
  slot last submitted — `FLUX_MAX_FRAMES_IN_FLIGHT` frames ago, stable
  while the frame records. These are the inputs `plate_polarity` and
  `backdrop_energy` consume; mapping and smoothing stay caller-side
  (ADR-0065).
- **Initial release.** The liquid-glass material moved out of libflux into
  libprism (the optics stack's material library); these bindings track it.
  `prism-sys` binds `<prism/prism.h>` at build time via bindgen, located
  through pkg-config with `PRISM_SOURCE_DIR` / `PRISM_BUILD_DIR` /
  `PRISM_USE_INSTALLED` overrides, mirroring `flux-sys`. flux types are
  shared, not duplicated: bindgen blocklists `flux_*` and the generated
  module re-exports them from `flux-sys`, so `flux_result` has exactly one
  Rust definition across the stack.
- **`prism` safe wrapper.** `LiquidGlassFilter` (RAII over
  `prism_liquid_glass_filter`), `LiquidGlassShape`, `LiquidGlassFocus`,
  `LiquidGlassGroup`, `LiquidGlassParams`, and the borrowed
  `LiquidGlassImage` output — the API that left the `flux` crate, taking
  `flux::Device` / `flux::Frame` / `flux::Image` / `flux::BlurredImage`
  directly and reusing `flux::Error`. `LiquidGlassParams::glare` is renamed
  to `rim_light`, following the C header.

### Changed

- The C `prism_liquid_glass_group` grew the five fields above; the
  push-constant block stays at 160 bytes via packing (`tint_color` +
  `shape_count` in one u32, f16 `size_reference`/`size_scale_min` in one
  u32). prism-sys' bindgen layout tests pin the new layout.
