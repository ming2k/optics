# ADR-0068: Frame-scoped, node-stamped opacity for lens

- Status: Accepted
- Date: 2026-08-15

## Context

Hosts animate overlays in and out with a fade. Lens had no opacity
concept, so every host hand-rolled the fade by scaling the alpha of
individual colours before building widgets. That mechanism is
uncompletable by construction: raster images emit with a hardcoded
opaque tint, several skins read slider and scrollbar colours straight
from the theme, and any text built under an unfaded theme keeps full
alpha. Elements painted through those paths stayed opaque through the
close animation and popped out at teardown — visible as stray bright
controls outliving the surface around them.

## Decision

Lens gains a frame-scoped opacity switch, `lens_set_opacity` /
`lens_opacity` (`libs/lens/include/lens/lens.h`), clamped to 0..1 with
a default of 1.0. The context switch is stamped onto every node at
build time (`lensi_store_touch` in `libs/lens/src/store/store.c`), and
emission bakes it into each draw command's colour alpha at the single
choke point (`lensi_drawlist_push` in
`libs/lens/src/render/drawlist.c`), so rects, borders, text, icons,
host images, sliders, and scrollbars fade together. The tooltip, which
paints outside the draw list, carries its own stamp taken when the
tooltip is set (`lensi_tooltip`). The switch resets to 1.0 at every
`lens_begin`, so a forgotten restore cannot dim the next frame; within
a frame the host sets it back after building the faded subtree.
Mechanism, not animation: the host owns the clock.

## Alternatives Considered

- **Host-side per-colour baking (status quo).** Rejected: every new
  widget, skin, or image call site can silently escape the fade, and
  each host duplicated the plumbing with slight variations.
- **A global alpha applied at replay/paint time.** Rejected: the
  display-list record cache replays baked segments, so a paint-time
  multiplier would need to re-walk cached content or bypass records;
  it also cannot express two subtrees at different opacities in one
  frame (staggered section reveals).
- **Library-driven appear/disappear animations.** Rejected: lens
  stores animation state but never integrates clocks itself (the
  `hover_t`/`active_t` eases aside); enter/exit pacing is host policy.

## Consequences

- The baked colours feed `hash_cmd`, so animating opacity invalidates
  display-list records frame by frame — the same re-record rate as the
  host-side baking it replaces, no new repaint hazard.
- `lens_frame_needs_repaint` observes opacity changes through the
  existing draw-list damage path; no new damage signal is needed.
- Hosts should drive one opacity per animated subtree and delete their
  per-colour fade plumbing; the Rust binding exposes the switch as
  `Frame::set_opacity` only, making the frame-scoped lifetime
  type-safe.
- `tests/lens/test_opacity.c` pins the stamping, clamping, per-node
  coexistence, and frame-reset behaviour.
