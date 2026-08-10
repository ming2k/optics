# ADR-0061: Lens style cascade and the mechanism / neutral-default / flavor rule

- Status: Accepted (enforcement follow-through, 2026-08-10: the
  `active_indicator_width` theme token + `LENS_STYLE_ACTIVE_INDICATOR_WIDTH`
  atom + selectable-skin rail were a residual flavor-behind-a-flag and have
  been deleted; accent rails belong to caller skins per item 1)
- Date: 2026-08-10
- Scope: lens (L2 toolkit). Defines the styling architecture: one uniform
  atomic cascade, plus the rule deciding what the library may ship.
  Amends [ADR-0058](0058-lens-widget-state-and-instance-styles.md)
  (resolution order; `_styled` suffix APIs removed), extends
  [ADR-0059](0059-lens-widget-skins.md) (tabs migrate to skins),
  reaffirms [ADR-0032](0032-lens-theme-tokens.md) (theme tokens stay).

## Context

ADR-0058 introduced `lens_style` — an atomic, bitmask-guarded style
struct — but its reach is partial: only `lens_button_styled` /
`lens_selectable_styled` / `lens_selectable_icon_styled` accept one.
The `_styled` suffix is itself a per-widget proliferation pattern that
would grow without bound. Meanwhile specialization accumulated
elsewhere:

- `lens_tabs_opts` carries 11 fields, four of them knobs for a
  **spring-animated indicator whose physics (stiffness, damping,
  velocity state) is baked into `widgets/tabs.c`** — an opinionated
  aesthetic signature hardcoded in the widget core.
- The icon-button family has separate APIs for purely visual variants
  (`lens_icon_button_active`'s accent rail,
  `lens_icon_button_active_rounded`'s tile).
- `lens_foreground_outline` exists only as per-widget `*_outlined`
  API variants.

Two ecosystem forces make this worse than it looks:

1. **Flags are the homogenization vector.** A library's shortest path
   determines what its ecosystem looks like. When a flavor ships behind
   a knob, every caller — human or AI — enables the same knob, and
   every app grows the same bouncing indicator. Flavor shipped as a
   *flag* is copied identically; flavor shipped as *copyable source*
   is mutated by each caller into their own.
2. **Special cases rot the core.** A baked-in effect touches state,
   animation, reduced-motion, and a11y simultaneously; each is code
   paths the core maintains forever for a look most apps should not
   share.

The distinction that resolves what stays and what leaves:

- **Affordance** — communicating state ("which tab is active"). Must
  ship, in neutral form (static, theme-coloured, motion-free).
- **Flavor** — aesthetic signature (spring physics, slides, glows).
  Must not ship in the library; ships as caller-owned recipes.
- **Mechanism** — what makes flavor *possible* (skins, style atoms,
  per-node scratch state). This is the library's actual job.

## Decision

1. **The rule.** Lens ships *mechanism* and *neutral defaults*; it
   never ships *flavor* behind a flag. Every visual feature request is
   classified affordance/flavor/mechanism first: affordance → neutral
   form in the default skin; flavor → a recipe under
   `examples/showcase/` that callers copy and own; mechanism → the
   library proper.
2. **One cascade, three layers, all data.** Resolution order is fixed
   and per-field (the ADR-0058 bitmask makes partial override cheap):
   **per-call style > nearest enclosing scope > theme**. Unset fields
   fall through each layer; the resolver's derivation order (hover/
   pressed derivation, disabled dim) is unchanged.
3. **`lens_box` gains `lens_style style`** (by value; `fields == 0`
   means "inherit", matching lens_box's zeroed-default convention).
   Every descriptor (`*_ex`) form thereby accepts per-call styling.
4. **Scoped style stack.** `lens_push_style(ui, style)` /
   `lens_pop_style(ui)` apply a partial style to every widget declared
   in between — the primitive from which callers build their own
   design-system scopes ("danger", "sidebar"). The stack is scoped
   (not a floating applies-to-next modifier, which ADR-0058's lens_box
   comment rejects) and resets every frame in `lens_begin`, so a
   forgotten pop cannot leak across frames.
5. **Universal coverage; `_styled` suffix APIs deleted.** Every widget
   resolves style (box + scope + theme) for the slots its visuals use —
   including widgets not yet migrated to skins, which read resolved
   values instead of raw theme tokens. `lens_button_styled`,
   `lens_selectable_styled`, `lens_selectable_icon_styled` are deleted;
   callers use the descriptor forms with `box.style` (or a scope).
6. **Outline becomes atomic.** `lens_style` gains
   `LENS_STYLE_OUTLINE_COLOR` / `LENS_STYLE_OUTLINE_WIDTH`; the
   `*_outlined` API variants and `lens_foreground_outline` are deleted.
7. **Icon buttons: state stays data, flavor leaves.**
   `lens_icon_button_active_rounded` is deleted (corner radius is an
   atom). `lens_icon_button_active` stays as the single state-bearing
   form but is reimplemented as *state + neutral tint*: the accent-rail
   treatment moves to a recipe. `lens_icon_button_badged` and
   `lens_icon_toggle_button` stay — a badge and a glyph swap are
   content variants, not styling.
8. **Tabs migrate to skins; the spring leaves the core.**
   `widgets/tabs.c` emission moves behind the ADR-0059 skin seam
   (`LENS_WIDGET_TABS`). The default skin draws a **static** indicator
   (theme accent, fixed thickness, zero animation — reduced-motion is
   trivially satisfied). `lens_tabs_opts` keeps only structural fields
   (`equal_width`); all colour/radius/indicator knobs are deleted in
   favour of the cascade. The spring indicator is reimplemented as the
   recipe `examples/showcase/tabs_spring_skin.c` — caller-copyable
   source, not a flag.
9. **Per-node scratch state (mechanism).** `lens_node` gains retained
   inline scratch storage exposed to skins as
   `float *lens_skin_scratch(lens *ui, lens_node *node)` (4 floats,
   zeroed on first touch, lives and dies with the node under the
   ADR-0038 GC). This is what lets caller-owned skins carry their own
   animation state (spring position/velocity) without any library-side
   allocation or lifetime hazard. Scratch is mechanism: the library
   provides storage, never an animation.

Implementation: `libs/lens/src/style/style.c` (cascade resolution),
`libs/lens/src/core/context.c` (scope stack + per-frame reset),
`libs/lens/include/lens/lens.h` (box field, push/pop, deletions),
`libs/lens/src/widgets/` (universal resolution; tabs skin migration;
icon-button collapse), `libs/lens/src/skin/tabs.c` (new),
`examples/showcase/tabs_spring_skin.c` (new recipe).

## Alternatives Considered

- **Grow `_styled` suffix APIs per widget.** Reject: per-widget
  proliferation without bound; two vocabularies for one concept.
- **Floating "applies to next widget" style modifiers.** Reject:
  ordering hazards (the reason lens_box documents "no floating
  modifiers"); the push/pop scope is strictly nested and frame-reset.
- **Full state × property matrix (CSS pseudo-classes).** Reject:
  combinatorial complexity; the fixed derivation order plus a few
  explicit state slots covers app needs.
- **Ship the spring as an opt-in flag.** Reject: flags are the
  homogenization vector this ADR exists to remove; the recipe carries
  the same code in caller-ownable form.
- **Delete all defaults; callers compose everything.** Reject: drops
  the usability/accessibility floor — a tab control with no active
  indication is broken UI. Neutral defaults are the consistency
  backbone; flavor is what leaves, not defaults.
- **Keep `lens_foreground_outline` variants.** Reject: per-widget API
  variants for one visual property; atomic fields reach every widget
  with no new surface.

## Consequences

Positive:

- One styling vocabulary reaches every widget: per-call (`box.style`),
  scoped (`push/pop`), global (theme), structural (skins).
- The library's visual signature shrinks to neutral defaults; apps stop
  looking identical by default, and AI-assisted callers compose atoms
  instead of enabling the same flags.
- Widget cores shrink: no physics, no flavor branches, no
  reduced-motion special cases for effects that no longer exist there.

Negative / invariants:

- New widgets must resolve style through the cascade (box + scope +
  theme) — a widget that reads raw theme tokens silently ignores caller
  scopes. Code review enforces; the resolver is the single choke point.
- Recipes are caller-owned copies: bug fixes in a recipe do not
  propagate to apps that copied it. Accepted as the price of
  anti-homogenization.
- ABI/API churn: `lens_box` grows; `_styled` APIs, `*_outlined`
  variants, `lens_foreground_outline`, and the tabs colour/indicator
  knobs are deleted. Call sites, tests, examples, and the Rust bindings
  migrate in the same commit.
- `lens_theme` is untouched (ADR-0032's ABI surface stays stable).

## References

- [ADR-0032](0032-lens-theme-tokens.md) — theme tokens, unchanged.
- [ADR-0058](0058-lens-widget-state-and-instance-styles.md) — state
  bits and per-instance styles, amended (cascade order; `_styled`
  removal).
- [ADR-0059](0059-lens-widget-skins.md) — the skin seam tabs migrates
  to.
- [ADR-0038](0038-lens-node-state-gc.md) — node lifetime governing
  scratch storage.
- [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md) — the same
  "mechanism, not flavor" philosophy applied to placement.
