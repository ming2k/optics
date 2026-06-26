# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Initial extraction from the lens monorepo.** The two Rust crates
  (`lens-sys`, `lens`) previously lived under `crates/` in the
  [lens][lens] C source tree. They are now a separate repository,
  matching the [flux-rs][flux-rs] convention.

  The in-tree build script assumed the C source root was the parent of
  `crates/lens-sys/`; that assumption no longer holds. The build script
  now takes an explicit `LENS_SOURCE_DIR` environment variable for the
  C source checkout and defaults to pkg-config installed mode otherwise.
  `LENS_BUILD_DIR` continues to point at a meson build tree for dev
  linking; `FLUX_BUILD_DIR` is honored for the transitive flux
  dependency.

[lens]: https://github.com/ming2k/lens
[flux-rs]: https://github.com/ming2k/flux-rs
