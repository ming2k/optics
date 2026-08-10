# ADR-0060: Lens single-tree placement and z bands — place/band supersedes parallel overlay roots

- Status: Accepted
- Date: 2026-08-10
- Scope: lens (L2 toolkit). Replaces the two-structure layering model
  (base tree + `overlay_layers[]`) with one tree where placement and
  z-order are node metadata. Supersedes [ADR-0037](0037-lens-overlay-layers.md);
  amends [ADR-0028](0028-lens-flexbox-layout.md) (measure/arrange) and
  [ADR-0035](0035-lens-accessibility-tree.md) (a11y walk).

## Context

ADR-0037 gave lens floating layers as *parallel sub-roots*: a separate
`overlay_layers[]` array with its own layout pass (`lensi_overlay_layout`),
render pass (`lensi_overlay_render`), damage pass
(`lensi_overlay_mark_dirty`), an explicit a11y visit, and an *eclipse*
patch so floating layers occlude base widgets during hit-testing. Every
tree pass carries a parallel twin, and the two sides must be kept
consistent by hand.

Two further pressures motivated revisiting the model:

1. **One tree is easier to reason about and to extend.** Damage
   propagation, the a11y parent chain, GC/lifetime, and store identity
   are all tree-structured today; floating layers opt out of each and
   pay for it with special cases.
2. **"Floating" was never only "above".** A dim backdrop, a notification
   stack, a drag ghost, a dropdown — these differ in z-band and
   lifetime, not in kind. The useful generalisation is *absolute
   placement + a z band*, which also admits layers *below* the base
   tree (decorations, backdrops) that ADR-0037 could not express.

Historical lessons that shape the design:

- **CSS numeric `z-index`** produced three decades of z-wars
  (`z-index: 999999`). Arbitrary integers mean no invariants.
- **CSS implicit stacking contexts** (`opacity < 1`, `transform`,
  `filter` create one silently) mean visual side effects change paint
  order — the single most surprising rule in CSS.
- **CSS arbitrary interleaving** (in-flow content sandwiched between
  positioned siblings) is what forces the Appendix E paint-order
  algorithm; app UI essentially never needs it.
- **The Web platform itself converged** on the "top layer" for
  `<dialog>`/popover: an ordered list, no z-index, light-dismiss
  semantics orthogonal to stacking.
- **Wayland layer-shell and macOS `NSWindow.Level`** prove closed,
  named z bands work at OS scale.
- **Flutter `Stack`/`Positioned`** proves a single tree can host
  absolutely-placed children cleanly when placement is child metadata.

## Decision

1. **One tree; placement is node metadata.** `lens_node` gains
   `place` (`LENS_PLACE_FLOW` default / `LENS_PLACE_ABS`), `band`,
   `mode`, `place_rect`, `place_bounds`, `transient`. An ABS node keeps
   its parent chain, its sibling sequence position (insertion order =
   registration order), store identity, and damage propagation — but
   does not consume space in its parent's flow. Only container
   sub-roots may be ABS; leaf widgets cannot `place`.
2. **Closed z bands; no numeric z, ever.**
   `lens_band = { LENS_BAND_BACKDROP, LENS_BAND_BASE, LENS_BAND_CHROME,
   LENS_BAND_POPUP, LENS_BAND_TOPMOST }`. FLOW nodes are always BASE.
   A node's band is set only at its `lens_place_begin` call site — no
   visual property (opacity, transform, animation) may ever change a
   band. Within a band, z-order is sibling insertion order (later =
   higher); there is no intra-band weight.
3. **Layout: one measure + one arrange, ABS-aware.**
   - *Measure* (bottom-up): ABS children are measured (their subtree
     needs its intrinsic size) but are **not** accumulated into the
     parent's main/cross extent.
   - *Arrange* (top-down): flow children are arranged by the unchanged
     ADR-0028 algorithm; then each ABS child is resolved by its `mode`
     — `LENS_PLACE_EXACT` (given rect, clamped to bounds), 
     `LENS_PLACE_ANCHORED` (probe at anchor, drop below, flip above on
     overflow, clamp), `LENS_PLACE_CENTERED` (centred on bounds) — and
     its subtree arranged recursively, followed by scroll clamping.
   The single-measure/single-arrange determinism of ADR-0028 is
   preserved.
4. **One global emission order.** After arrange, one tree walk buckets
   ABS nodes into `bands[LENS_BAND_COUNT]` lists (tree pre-order within
   each band = registration order). Render order:
   `bands[BACKDROP]` → base tree (skipping ABS nodes in-tree) →
   `bands[CHROME, POPUP, TOPMOST]` in band order. Hit-testing consumes
   the exact reverse: bands topmost-first, then the base-tree spatial
   walk, then (if interactive) BACKDROP. **The eclipse mechanism is
   deleted** — occlusion *is* the hit-test order. The a11y walk becomes
   a single structural pre-order (ADR-0035 item 6 removed): assistive
   technology wants reading (tree) order, not z-order.
5. **Clip: one rule.** ABS nodes escape all ancestor clips (including
   scroll viewports) and are clipped only by their `place_bounds`
   (default: the display). There is no containing-block/overflow
   combination semantics.
6. **Transient lifetime is orthogonal to stacking.** The open-set
   table, Escape-closes-top, click-outside dismissal, and same-frame
   grace are kept unchanged, driven by the `transient` flag on POPUP
   nodes instead of a separate overlay registry. Prev-frame layer ids
   are still carried across the arena reset for hit-testing (now as
   per-band prev lists).
7. **BACKDROP band.** Renders below the base tree; non-interactive by
   default (skipped by hit-testing unless explicitly flagged). Intended
   for node-driven decorations and dimming backdrops that must live in
   the lens tree (a11y-hidden, damage-tracked) rather than the host's
   paint callback.
8. **Public API.** New `lens_place_begin(ui, id, lens_place_opts)` /
   `lens_place_end(ui)` with
   `lens_place_opts { band, mode, rect, bounds, transient, layout }`
   (`layout` = the usual `lens_layout_opts` for the subtree's internal
   flexbox). `lens_place_open` / `lens_place_close` /
   `lens_place_is_open` / `lens_place_hovered` manage transient state.
   **`lens_overlay_begin`, `lens_layer_begin`, `lens_overlay_open`,
   `lens_overlay_close`, `lens_overlay_is_open`, `lens_overlay_hovered`
   are deleted.** All call sites migrate: dropdown, menu, modal,
   tooltip (widgets), iris examples, lens tests, and the Rust bindings.

Implementation: `libs/lens/src/layout/solve.c` (ABS-aware measure/
arrange), `libs/lens/src/place/` (band bucketing, placement modes,
transient state machine; `overlay/` is renamed/rewritten),
`libs/lens/src/render/replay.c` (band emission), `libs/lens/src/input/`
(band-ordered hit-test, eclipse removal),
`libs/lens/src/a11y/semantics.c` (single pre-order walk),
`libs/lens/include/lens/lens.h` (public API).

## Alternatives Considered

- **Keep two structures (status quo).** Reject: every pass keeps a
  parallel twin; extending z downwards (BACKDROP) would add a third
  path instead of generalising one.
- **Numeric z-index.** Reject: CSS's z-wars; arbitrary integers carry
  no invariants and invite layering auctions.
- **Implicit stacking contexts (visual properties change order).**
  Reject: the most surprising rule in CSS; bands change only at
  explicit `place` call sites.
- **Arbitrary in-flow/positioned interleaving.** Reject: forces a
  CSS Appendix-E-style paint order; app UI does not need it. Band
  grouping covers the real cases.
- **Per-container bands.** Reject: a POPUP clipped/ordered inside a
  scroll container breaks the escape-clip requirement; bands are
  global by construction.
- **Relative positioning (offset from flow position).** Deferred:
  harmless under this model (everything reads `final_rect`) but has no
  current use case; add later only against a real need.
- **Keep `lens_overlay_begin`/`lens_layer_begin` as sugar.** Reject:
  two vocabularies for one concept; the stack is pre-1.0 and all call
  sites are in-tree.

## Consequences

Positive:

- Eclipse is deleted; occlusion falls out of hit-test order.
- Damage and a11y unify into plain tree passes — overlay sub-roots lose
  every special case.
- Placement generalises up (CHROME/POPUP/TOPMOST) and down (BACKDROP)
  with one vocabulary.
- Z-order has exactly one source: band + insertion order. No numeric
  z, no implicit triggers, no intra-band weights.

Negative / invariants now enforced by code review:

- Every tree pass (measure, arrange, render, hit, a11y) must be
  band-aware; a new pass that forgets ABS nodes misbehaves silently.
  The band bucketing walk is the single choke point and must stay the
  only place that defines order.
- The transient state machine still exists; merging the trees does not
  remove dismissal/grace logic, it only relocates its input.
- Rust bindings (`bindings/lens-rs`) and iris examples must migrate in
  the same commit to keep the monorepo atomic.
- ADR-0037 is superseded; ADR-0028 and ADR-0035 are amended. Tests
  `test_overlay.c`, `test_layer.c`, `test_overlay_eclipse.c` are
  rewritten as place/band tests.

## References

- [ADR-0024](0024-lens-foundations.md) — headless foundations.
- [ADR-0026](0026-lens-id-system.md), [ADR-0027](0027-lens-retained-store.md)
  — identity and lifetime, unchanged.
- [ADR-0028](0028-lens-flexbox-layout.md) — two-pass layout, amended.
- [ADR-0030](0030-lens-damage-tracking.md) — damage now propagates over
  one tree.
- [ADR-0035](0035-lens-accessibility-tree.md) — walk unified, amended.
- [ADR-0037](0037-lens-overlay-layers.md) — superseded by this ADR.
- [ADR-0039](0039-lens-modal-dialog.md), [ADR-0040](0040-lens-menus.md)
  — consumers rebuilt on `lens_place_*`.
- CSS 2.1 Appendix E (paint order); HTML "top layer" (dialog/popover);
  Wayland `wlr-layer-shell` bands; Flutter `Stack`/`Positioned`.
