# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

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
