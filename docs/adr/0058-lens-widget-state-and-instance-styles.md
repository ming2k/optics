# ADR-0058: Lens widget state bitflags and per-instance styles

- Status: Accepted (amended by [ADR-0061](0061-lens-style-cascade-mechanism-neutral-flavor.md):
  resolution became the per-call > scope > theme cascade, and the `_styled`
  suffix APIs were removed in favour of `lens_box.style` + `lens_push_style`)
- Date: 2026-08-10
- Scope: lens (L2 toolkit). Defines how widget state is reported and how
  single widget calls are restyled.

## Context

Every lens widget function used to fuse three concerns in one body:
interaction, style resolution, and draw-command emission (see the
pre-refactor `src/widgets/button.c`). Two structural problems followed:

1. **State was implicit.** Hover/press/focus existed as ad-hoc booleans
   on `lens_response` plus eased floats (`hover_t`/`active_t`) on the
   retained node. There was no way to ask "is this row selected AND
   hovered?" as data, no disabled bit, and no distinction between
   keyboard and pointer focus (so a focus ring either always shows —
   noisy for mouse users — or never shows — hostile to keyboard users).
2. **Styling was theme-global only.** A single restyled row (one
   destructive button, one sidebar item with an accent rail) required
   either a theme-wide token change affecting every widget, or a new
   per-widget options struct. The active-indicator rail was gated by the
   theme token `active_indicator_width`, so one application's nav row
   could not opt in individually.

Forces: the theme struct's layout is a published ABI surface
(ADR-0032) and must not churn; default visuals must stay
pixel-identical for existing callers; the display-list and replay
layers already match the target architecture and must not be touched.

## Decision

1. **Explicit state bitflags.** A public `lens_widget_state` enum (fixed
   `uint32_t` underlying type, C23) defines `LENS_STATE_HOVERED`,
   `PRESSED`, `FOCUSED`, `FOCUS_VISIBLE`, `DISABLED`, `SELECTED`,
   `ACTIVE`, `DRAGGED`. `lens_response` gains a `uint32_t state` field.
   The interaction core (`lensi_interact`, `src/input/input.c`) produces
   the pointer/focus/disabled bits centrally for every widget; each
   widget ORs in the bits only it can know before publishing the
   response — selectable sets `SELECTED` from its argument, toggle
   widgets (checkbox, switch, icon toggles) set `ACTIVE` from their
   value, value sliders set `DRAGGED` while captured.
2. **Keyboard-modality focus visibility.** `FOCUS_VISIBLE` means
   "focused via keyboard navigation". The context carries a
   `focus_visible` modality flag: Tab traversal (`lensi_focus_tab`,
   src/input/focus.c) sets it, a pointer press that moves focus
   (`lensi_interact`) clears it, and programmatic `lens_set_focus`
   clears it (host-driven focus is not keyboard navigation). Arrow-key
   navigation inside composite widgets (menus, tabs) moves
   widget-internal highlights, not `focused_id`, so it does not touch
   the modality; any future widget that moves `focused_id` on arrow
   keys must set the flag there. This mirrors the web's `:focus-visible`
   heuristic with lens's explicit input model.
3. **Per-instance style data.** A public `lens_style` struct carries
   optional fields — `bg`, `bg_hover`, `bg_pressed`, `fg`, `border`,
   `accent`, `corner_radius`, `border_width`, `padding`, `gap`,
   `font_size`, `active_indicator_width` — each guarded by a bit in a
   `fields` mask (`LENS_STYLE_*` tags). The mask is the forward-compat
   guard: callers never set bits for fields their header does not know,
   and the library never reads a field whose bit is clear.
   `LENS_STYLE_INIT` / `lens_style_init()` produce the empty style.
4. **A pure resolver with one defined order.** `lensi_style_resolve`
   (src/style/style.c) maps `(instance style, theme, state)` to a fully
   concrete `lens_style_resolved`, in exactly this order:
   (a) *fallback* — unset slots take their theme token;
   (b) *derivation* — for an instance-set `bg` only, a missing
   `bg_hover` mixes 8% of the resolved foreground in (a lift; foreground
   contrasts with any surface by definition, so the direction suits
   light and dark themes alike), then a missing `bg_pressed` mixes 16%
   off the *base* bg — one-colour overrides get state feedback for free;
   (c) *disabled dim, last* — with `LENS_STATE_DISABLED`, every
   instance-sourced colour blends 60% toward `color_disabled`, on top of
   the hover/pressed adjustments. Theme-sourced slots are never derived
   over or dimmed: the theme already carries explicit `color_hover` /
   `color_active` / `color_disabled` design tokens, and widgets read
   `resolved.disabled` for their disabled branches. The animation
   floats (`hover_t`/`active_t`) deliberately stay with the widgets:
   their blend curves (a button's `max(active_t, hover_t*0.4)` ramp, a
   selectable's `hover_t*0.6` fill) are widget-specific and cannot be
   one shared formula, so the resolver adjusts *colours*, not *curves*.
5. **Theme tokens remain the defaults — only.** With a NULL or empty
   instance style the resolver output is the verbatim theme for every
   state, so a widget that reads the resolved style instead of
   `ui->theme` renders pixel-identically by construction. Themes keep
   their role as the design system; `lens_style` is the per-call escape.
6. **Fixed widget phase order.** Migrated widgets run: *measure*
   (through the text seam, using the state-independent geometry
   fallback `lensi_style_font_size` / `lensi_style_padding`) →
   *interact* (`lensi_interact` produces the state bits) → *resolve*
   (one pure call) → *emit* (draw commands read only resolved slots).
7. **Styled variants, not signature changes.** `lens_button_styled`,
   `lens_selectable_styled`, and `lens_selectable_icon_styled` take a
   `const lens_style *` (NULL = default); the existing
   `lens_button` / `lens_selectable` / `lens_selectable_icon` delegate
   with NULL. (The `lens_button_ex` / `lens_selectable_ex` names were
   already taken by the ADR-0031 descriptor forms, so the new entry
   points use the `_styled` suffix.) The safe Rust binding mirrors this:
   `WidgetState` on `Response`, a `Style` builder, and
   `Frame::button_styled` / `Frame::selectable_styled` /
   `Frame::selectable_icon_styled`.

References: `libs/lens/include/lens/lens.h` (widget state bits, instance
styles sections), `libs/lens/src/style/style.c`,
`libs/lens/src/internal.h` (`lens_style_resolved`, derivation
constants), `libs/lens/src/widgets/{button,selectable}.c`,
`tests/lens/test_state_style.c`,
`bindings/lens-rs/crates/lens/src/types.rs`.

## Alternatives Considered

- **A style stack (`push_style_color` / `pop_style_color`, imgui-style).**
  Reject: floating "applies to the next widget" state reintroduces the
  ordering hazards the `lens_box` descriptor explicitly removed; a
  per-call struct has no ambient scope to get wrong.
- **Extend every `*_opts` descriptor with style fields.** Reject: the
  mask-guarded `lens_style` is one type to learn and forward-compatible
  by construction; duplicating a dozen optional fields across ~20
  options structs multiplies API surface and documentation.
- **A full state-effective resolver (hover/pressed blending inside
  `lensi_style_resolve`).** Reject: the two reference widgets blend
  toward different targets with different factors (`0.4` vs `0.6`,
  accent-active vs bg-hover), so one closed form cannot reproduce both;
  pushing the curves in would require per-widget tuning parameters —
  the role parameter in disguise — for no gain in clarity.
- **Dim theme-sourced colours under DISABLED too.** Reject: the default
  rail paints `color_accent` even when disabled+selected today, and the
  theme's `color_disabled` is a designed token, not a derivation —
  dimming theme slots would change default pixels.
- **Widget-specific state enums (per-widget flag sets).** Reject: one
  shared vocabulary lets hosts and future widgets test state uniformly;
  widgets simply never set the bits that do not apply to them.

## Consequences

Positive:

- Widget bodies read top-to-bottom in four phases; the style of any
  widget is now data a host can construct, store, and diff.
- One-off restyling (destructive button, railed nav row) needs no theme
  fork; the rail is data (`active_indicator_width` on the instance),
  not a theme-gated paint fork.
- Keyboard users get a focus ring affordance without punishing pointer
  users; hosts can finally style hover/pressed/disabled uniformly.

Negative / invariants:

- `lens_response` grows a field — an ABI change for the struct;
  acceptable because all known consumers rebuild (bindgen regenerates
  from the header). The boolean fields stay, so source compatibility
  holds.
- Widgets must not re-adjust resolved colours: the resolver's order
  (fallback → derive → dim) is the only adjustment site. New state
  treatments belong in the resolver, not in widget code.
- The derivation constants (8% / 16% / 60%) are a contract: changing
  them changes instance-styled visuals everywhere.

Follow-up (incremental migration plan for the remaining ~20 widgets):

1. Migrate one widget family per change, following button/selectable:
   route theme reads through `lensi_style_resolve`, keep the legacy
   blend curves, add the `_styled` entry point, delegate the terse and
   descriptor forms with NULL.
2. Order by host demand: text inputs (textfield/textarea), then the
   toggle family (checkbox/switch/radio — already report ACTIVE), then
   sliders, then composites (tabs, dropdown, menu). Display-only
   widgets (label, image, separator) migrate last or never.
3. Each migration extends `test_state_style.c` with a NULL-style
   drawlist-equality check (see `test_null_style_matches_plain_form`)
   before touching visuals.
4. Drag producers that keep drags internal today (scroll thumb, split
   divider) adopt `DRAGGED` when they migrate; menus/tabs adopt
   SELECTED/ACTIVE then as well.

## References

- [ADR-0029](0029-lens-interaction-model.md) — interaction model the
  state bits formalise.
- [ADR-0031](0031-lens-symbol-namespaces.md) — symbol namespaces; the
  `_styled` suffix respects the occupied `_ex` descriptor names.
- [ADR-0032](0032-lens-theme-tokens.md) — theme tokens; unchanged, now
  the resolver's fallback source.
- Web prior art: CSS `:focus-visible` modality heuristics.
