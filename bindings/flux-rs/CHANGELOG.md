# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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
