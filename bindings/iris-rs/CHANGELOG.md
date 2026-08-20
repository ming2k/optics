# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **A following `iris_request_animation_frame` outranks a static
  declaration on Wayland too.** The Wayland backend consumed the
  animation request before the skip decision and dropped a fresh request
  in its static branch, freezing a host that declared static and then
  asked for another frame — win32 and cocoa already force the paint. All
  three backends now apply the documented contract: the request always
  forces the next frame to paint.

### Added

- **Initial extraction from the iris monorepo.** The two Rust crates
  (`iris-sys`, `iris`) previously lived under `crates/` in the
  [iris][iris] C source tree. They are now a separate repository,
  matching the [flux-rs][flux-rs] / [lens-rs][lens-rs] convention.

  The in-tree build script assumed the C source root was the parent of
  `crates/iris-sys/`; that assumption no longer holds. The build script
  now takes an explicit `IRIS_SOURCE_DIR` environment variable for the
  C source checkout and defaults to pkg-config installed mode otherwise.
  `IRIS_BUILD_DIR` continues to point at a meson build tree for dev
  linking; `LENS_BUILD_DIR` and `FLUX_BUILD_DIR` are honored for the
  transitive lens / flux dependencies.

  The `iris` crate's `Cargo.toml` now points at `lens-rs` (extracted
  sibling) instead of `lens/crates/lens`.

[iris]: https://github.com/ming2k/iris
[flux-rs]: https://github.com/ming2k/flux-rs
[lens-rs]: https://github.com/ming2k/lens-rs
