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

- **Keyboard cursor, icons, and host-owned selection for tables
  (ADR-0066).** `TableOpts` gains a `keyboard` flag: while the table is
  focused (selectable tables now join the tab order), Up/Down/Home/End
  move a cursor row and Return activates it (Space stays host-side),
  scrolling it into
  view. New `Frame::table_ex` adds three pull callbacks: an icon per
  cell (raw `sys::lens_icon_id`, since hosts use runtime-registered SVG
  ids; only `Align::Start` columns draw one), a host-owned selection
  query (clicks then only report `clicked_row`), and a host-owned
  in/out `cursor: &mut i32` the table reads at build start and writes
  back on movement or model-shrink clamps. `TableResult` gains
  `cursor`, `cursor_changed`, `activated`, and `clicked_row`.
  `Frame::table` is unchanged apart from forwarding `opts.keyboard`.

- **Host-controlled caret/selection for text fields.** New
  `Frame::textfield_set_caret`, `Frame::textfield_set_selection`, and
  `Frame::textfield_scoped_set_selection` wrappers (C:
  `lens_textfield_set_caret`, `lens_textfield_set_selection`) move the
  caret and selection of a named text field, for hosts that
  programmatically rewrite the edit buffer (Tab completion, pre-filled
  values). Offsets are bytes; out-of-range offsets clamp to the buffer
  length and mid-character offsets snap back to a UTF-8 boundary at the
  next build. Select-all is `anchor = 0, caret = u32::MAX`.
  `TextBuf::set`'s docs now point at the caret API.

[lens]: https://github.com/ming2k/lens
[flux-rs]: https://github.com/ming2k/flux-rs
