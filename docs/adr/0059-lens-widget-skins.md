# ADR-0059: Lens widget skins — emission as a replaceable function

- Status: Accepted (extended by [ADR-0061](0061-lens-style-cascade-mechanism-neutral-flavor.md):
  tabs migrated to the skin seam as LENS_WIDGET_TABS, and the per-call
  `*_skinned` forms were retired — the context is the single override
  granularity; completed 2026-08-10: every remaining widget family —
  label/title, separator, icon, image, progress, textfield, textarea,
  collapsing, tree, table, split, menu, dropdown, link — migrated behind
  the seam, so no widget file authors draw commands any more)
- Date: 2026-08-10
- Scope: lens (L2 toolkit). Defines how widget draw commands are produced.
  Refines ADR-0058's phase order: the "emit" phase is now a skin call.

## Context

ADR-0058 made widget state and style pure data, but the migrated widgets
still authored their own display lists: the emit sections inside
`button.c` / `selectable.c` decided how many rects to draw, where text
sits, and when a focus ring appears. Two problems followed:

1. **No full headless / reskin seam.** A host that wants a different
   visual language (or a test that wants to assert structure instead of
   pixels) had to fork widget internals; the drawing decisions were not
   data and not replaceable.
2. **Layout emitted.** `scroll_clamp_node` (layout/solve.c) pushed
   scrollbar draw commands during the clamp pass — layout code authoring
   chrome, computing draw geometry mid-layout.

Forces: default rendering must stay pixel-identical; the public API may
only grow; a replacement skin must be writable outside the library (the
Rust binding is a first-class consumer), so the skin boundary may depend
on public types only.

## Decision

1. **A plain-data record per widget per frame.** `lens_widget_record`
   (lens.h) carries: `kind` (a fixed-u32 `lens_widget_kind`: BUTTON,
   SELECTABLE, CHECKBOX, SWITCH, RADIO, SLIDER — both orientations share,
   with `content.vertical` — and ICON_BUTTON), the ADR-0058 `state` bits,
   `bounds` and `last_bounds`, the resolved `lens_style_resolved` (the
   struct moved from internal.h into the public header for this), the
   instance's `style_fields` set-mask, the eased `hover_t`/`active_t`
   floats, and a flat `lens_widget_content` payload (label + measured
   text metrics, icon id, slider ratio/orientation/error, switch
   description, icon-button variant flags) whose members are valid per
   kind, matching the kind-tagged-field convention of `lens_draw_cmd`.
   **Coordinate choice:** `bounds` is node-local — `{0,0,w,h}` with this
   frame's measured size — because draw commands are node-local (resolved
   against the node's final rect at render time) and skins run during
   build, before layout solves positions. `last_bounds` (last frame's
   arranged rect, zero on the first frame) is what interaction
   hit-tested; chrome that scales with the arranged size (the slider
   track) reads it.
2. **The skin signature and dispatch.** A skin is
   `void (*)(lens *ui, lens_node *node, const lens_widget_record *rec)`.
   Migrated widgets keep identity, store, measure, interaction, the
   eased-float updates, and accessibility semantics; they then pack the
   record and call `lensi_skin_emit` (src/skin/skin.c), which picks the
   first non-NULL of: per-call skin → context skin → built-in default.
   No `lensi_drawlist_push` of visual chrome remains in the migrated
   widget files.
3. **Default skins are the old emit sections, verbatim.**
   src/skin/{button,selectable,checkbox,switch,radio,slider,icon_button}.c
   hold the pre-skin emission code unchanged, reading the same values
   from the record — pixel-identical by construction, and pinned by
   null-override equality tests. `lens_default_skin(kind)` exposes the
   built-ins so a custom skin can wrap stock chrome instead of replacing
   it. Two exceptions to "record only": the slider skin re-derives its
   first-frame fallback from the node's internal `has_prev`/`prev_rect`
   (the record cannot distinguish "first frame" from a zero-sized rect —
   immaterial for replacement skins, exact for the default), and skins
   read theme tokens that have no `lens_style` slot yet (slider track/
   knob, scrollbar colours) straight from `ui->theme`.
4. **Two override granularities.** Context-wide: `lens_set_skin(kind,
   fn)` stores a per-kind replacement on the context; NULL restores the
   default. Per-call: `lens_button_skinned`, `lens_selectable_skinned`,
   `lens_selectable_icon_skinned` take `(style, skin)`; the terse,
   `_styled`, and `_ex` forms delegate with NULL/NULL. Per-call forms
   exist only for the ADR-0058 styled family — the value widgets
   (checkbox/switch/radio/slider) and the icon-button family are covered
   by the context granularity; adding six more near-identical entry
   points buys nothing a host has asked for.
5. **A narrow public emission seam.** Replacement skins draw through
   `lens_skin_rect` / `lens_skin_border` / `lens_skin_text` /
   `lens_skin_icon` — node-local rects, the draw list's span and
   centering conventions, documented at the declarations. The whole
   `lens_draw_cmd` struct stays internal; the seam is the supported way
   to write a skin from outside the library (built-in skins call
   `lensi_drawlist_push` directly — they predate the seam and need its
   full field set).
6. **Scrollbar chrome out of layout.** `scroll_clamp_node` now only
   clamps offsets, shifts content, and persists the clamped offsets.
   src/skin/scrollbar.c is a drawlist-finalize walk,
   `lensi_scrollbars_emit`, called from `lens_end` after base-tree layout
   and overlay placement and before damage tracking: it visits every
   scroll container (base tree plus floating-layer sub-roots), reads the
   *solved* rects, draws track and thumb with the same math as before,
   and persists the thumb geometry next frame's hit-testing reads (same
   write timing as the old code: after arrange, once per frame).

References: `libs/lens/include/lens/lens.h` (Widget skins section),
`libs/lens/src/skin/*.c`, `libs/lens/src/widgets/*.c` (record packing),
`libs/lens/src/layout/solve.c` (`scroll_clamp_node`),
`libs/lens/src/core/context.c` (`lens_end`),
`tests/lens/test_skin.c`.

## Alternatives Considered

- **Skins run post-layout, records queued at build.** Reject: the record
  would gain true final bounds, but every record and its strings would
  have to be arena-queued and re-dispatched, and the skin could no
  longer be a synchronous per-call override (the call site has returned
  by the time the skin runs). Node-local coordinates make build-time
  skins exact — the final position is genuinely not needed.
- **Make `lens_draw_cmd` fully public as the skin API.** Reject: clip
  push/pop, image pointers, record-invalidation fields and text-family
  internals are not a stable contract; the four-call seam covers every
  default skin's vocabulary and can grow deliberately.
- **Skins as data (a declarative template per kind).** Reject: a
  function is strictly more general, needs no interpreter, and matches
  the toolkit's C idiom; hosts that want data-driven skins can generate
  one function per template at startup.
- **Migrate tabs too.** Reject: the connected tab style draws shoulders
  that join the active tab to its neighbours and an indicator sprung
  between tabs — geometry derived from the strip, not the tab. A per-tab
  record would have to encode strip connectivity and sibling geometry,
  i.e. be isomorphic to the command list it replaces. The same argument
  keeps textfield/textarea (selection rects, IME composition, caret),
  table (virtualized row window), tree (indent disclosure state), and
  menu (hover-dwell submenu geometry) authoring their own commands: for
  them the record *is* the command list, so the skin boundary adds
  indirection without replacing a decision. They also keep working
  unchanged — the skin layer is opt-in per kind.
- **Per-call `_skinned` forms for all seven kinds.** Reject for now:
  the context granularity covers them; the per-call forms exist where
  instance styling already exists (ADR-0058). Cheap to add later if a
  host needs them.

## Consequences

Positive:

- Widget visuals are replaceable end-to-end: a host can restyle a kind
  globally or one call at a time, and can verify structure headlessly by
  capturing the record (the test suite does exactly this).
- Layout no longer emits; `solve.c` is push-free, and scrollbar chrome
  reads solved geometry like everything else.
- The lens_rs binding exposes the whole seam: `WidgetKind`, the raw
  record types, `Frame::set_skin` / `default_skin`, the `_skinned`
  methods, and `skin_rect`/`skin_border`/`skin_text`/`skin_icon`.

Negative / invariants:

- A migrated widget must not push chrome: draw commands for a kind come
  from its skin only, so overriding is total (no half-skinned widget).
  Structural calls (id, store, a11y semantics) stay in the widget.
- The record is per-frame borrowed data: skins must not retain it, the
  node, or content strings past the call.
- `lens_style` has no slots for slider/scrollbar tokens; skins read
  those from the theme. Widening the slot set (or per-kind style
  structs) is future work.
- Rust skins are raw `extern "C"` fn pointers; closure trampolines
  (boxed `FnMut` + user-data thunk) are deliberately future work — the
  C signature has no user-data pointer, so trampolines need either a
  context slot or a new signature, and neither is needed to validate the
  layer.
- `lens_link`, bare `lens_icon`, and the image buttons still author
  commands in widget files; they migrate with their families in a later
  pass (the icon-button *skin* already serves the whole
  `lens_icon_button*` family).

## References

- [ADR-0058](0058-lens-widget-state-and-instance-styles.md) — state bits
  and the resolved style the record carries.
- [ADR-0029](0029-lens-interaction-model.md) — the one-frame latency
  behind `last_bounds`.
- [ADR-0030](0030-lens-damage-tracking.md) — command hashing; skin
  output participates exactly like build-time emission.
- Prior art: Flutter's `RenderObject`/`Widget` split, Clay's custom
  element handlers, nuklear's style callbacks.
