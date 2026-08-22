# Changelog

Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Versions
follow [semver](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Stable identities and explicit scroll-list layout.** New
  `Frame::selectable_with_id` and `Frame::push_id_int` APIs keep retained
  state attached to model ids when visible labels repeat or rows reorder.
  `Ui::has_duplicate_ids` / `Frame::has_duplicate_ids` expose the C frame
  diagnostic instead of letting duplicate sibling ids fail silently, and
  `Frame::scroll_column` makes a list's padding and row gap explicit.
- **Menu bar, menus, and menu items (ADR-0040).** New safe wrappers for
  the C menu family: `Frame::menubar` (bar of triggers; the C begin is
  unconditionally true so the body always runs), `Frame::menu` (trigger
  + popup; body runs only while open), `Frame::menu_item`,
  `Frame::menu_item_checked` (LENS_MENU_CHECKED presentation), and
  `Frame::menu_separator`, plus `Frame::menubar_close_all_open` for
  programmatic dismiss. Items close the whole stack automatically on
  click; an empty `shortcut` string maps to C's NULL (draws no hint).
  `Frame::submenu` (nested, hover-dwell), `Frame::context_menu_open` /
  `Frame::context_menu` (right-click menus anchored at a row rect) cover
  the rest of ADR-0040's surface.
- **Raw-id icon call forms.** `Frame::icon_raw`, `icon_button_raw`,
  `icon_button_raw_active`, and `icon_toggle_button_raw` take
  `sys::lens_icon_id` directly, so runtime-registered SVG glyphs
  ([`register_svg_icon`]) work in the same widget slots as the typed
  [`Icon`] enum (whose variants are compile-time built-ins only).

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

### Fixed

- **Table cell text lifetime.** The `Frame::table` / `Frame::table_ex`
  cell callback hands out a reused scratch buffer per query, but the C
  widget stored the raw pointers across the visible-row loop and the
  skin read them only after later calls had recycled the scratch —
  dangling reads drew garbled or duplicated cell text. The widget now
  copies each run into the per-frame arena when received, making the
  documented borrow contract (`lens_table_cell_fn`) real. Covered by a
  scratch-reuse regression test (C side) under ASan.

[lens]: https://github.com/ming2k/lens
[flux-rs]: https://github.com/ming2k/flux-rs
