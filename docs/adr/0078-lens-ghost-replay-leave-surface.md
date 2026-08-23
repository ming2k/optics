# ADR-0078: Ghost replay — the render surface for leave animations

- Status: Proposed
- Date: 2026-08-23
- Scope: lens (L2 toolkit). Amends the deferred note on
  [ADR-0038](0038-lens-node-state-gc.md) (leaving nodes had no render path)
  and pairs with [ADR-0068](0068-lens-frame-scoped-node-stamped-opacity.md).

## Context

ADR-0038 gave leaving nodes an 8-frame grace window so state survives a
transient absence — and recorded in its status note (2026-08-10) that the
window has **no render path**: leaving nodes are not painted, so *there is no
leave-animation surface at all*. The only way to fade a subtree out today is
for the host to keep building the dead content for its own animation, which
defeats immediate mode: the host must retain data it has already logically
deleted, keep the ids alive, and drive the fade by hand (ADR-0068 supplies
the opacity knob; nothing supplies the pixels once building stops).

The grace window's length is not the fixable part. Eight frames is 133 ms at
60 Hz and 67 ms at 120 Hz — under the 200–300 ms a comfortable exit reads at
— but even an unbounded window would change nothing while leaving nodes are
not drawn. The missing piece is *pixels from a subtree that is no longer
being built*, which is a mechanism concern and therefore lens's to own.

Forces:

1. **The host must not own retention of dead content.** Requiring callers to
   keep building deleted subtrees is the "被迫变通" the design reviews keep
   hitting; it is the same uncompletable-by-construction shape ADR-0068
   rejected for per-colour fade baking.
2. **lens must not own a clock.** Any lifetime expressed in milliseconds
   would need a clock lens does not have; the frame counter is the only
   time base lens owns (ADR-0038's reap cadence).
3. **Bounded memory.** Ghosts are dead content kept alive only for paint;
   they must be capped harder than the node store and must never grow a
   second arena.
4. **Dead means non-interactive.** A ghost must not hit-test, focus, or
   appear in the accessibility tree; the removal announcement happens when
   the data leaves, not when the pixels fade (the WCAG-aligned rule lens
   already follows for stale `prev_rect`).

## Decision

1. **`lens_set_ghost` is the leave-animation surface.** While a subtree is
   still being built, the host pins it:

   ```c
   lens_set_ghost(ui, subtree_root_id, 1.0f);   /* arm, during the build */
   ...
   lens_set_ghost(ui, subtree_root_id, alpha);  /* fade, after removal */
   ```

   A pin on a live subtree **arms capture**; a pin on an existing ghost
   refreshes and retints it (the steady fade loop). Unknown or never-seen
   ids are ignored silently — the ghost surface is advisory, never a host
   crash.

2. **The snapshot is captured at the last live `lens_end`, intent-gated.**
   Leaving a subtree changes nothing about *that* frame's data; the moment
   to freeze pixels is the last `lens_end` in which the subtree was alive
   and arranged. `lens_end` (after `lensi_mark_dirty`) walks the leaving set
   and snapshots exactly the subtree roots **pinned this frame** with ink
   and a non-degenerate `final_rect`. Unconditional capture was tried and
   rejected by the golden record/replay tests: repainting every deleted
   widget for one frame is the stale-record artifact those tests exist to
   forbid. Pins are per-frame (cleared at `lens_begin`); arming while the
   subtree is still built is the natural call site and costs nothing when
   the id never leaves. The snapshot is a deep copy
   out of the per-frame arena into `lensi_alloc` memory: each node's
   `cmds` (with arena-owned `text` pointers copied), `final_rect`,
   `is_scroll`/clip-relevant fields, `place_bounds`, and band. The arena
   resets next `lens_begin`; the ghost must survive it.

3. **A ghost renders by re-emitting its snapshot through the same command
   emitter the live tree uses** (`lensi_render_node`'s command loop,
   factored so the draw-command switch is shared): same geometry resolution
   (`offset_rel` + `snap_rect`), same text/icon/image paths, same clip
   stack discipline — but with the node's colors passed through the
   existing `lensi_opacity_color(…, alpha)` bake so one call fades the whole
   snapshot exactly as ADR-0068 fades a live subtree. Ghosts render in band
   order after that band's live nodes, in first-registered order; a ghost
   never occludes live content of a higher band. Snapshots do not create
   canvas display-list records (their content is frozen; records would only
   add release bookkeeping for zero replay benefit, and a canvas switch
   must not need to drop them).

4. **Lifetime is frame-capped, not clock-capped: `LENSI_GHOST_MAX_FRAMES =
   64`.** Every `lens_set_ghost` call on a live ghost resets its countdown;
   at `lens_end`, ghosts not refreshed this frame count down and are freed
   at zero. The cap bounds memory regardless of host behaviour (a host that
   forgets a ghost leaks at most 64 frames of one subtree's draw list, then
   pays nothing). 64 frames covers 300 ms at 213 Hz with margin; a host
   wanting wall-clock pacing converts `ms → frames` itself
   (`ceil(ms/1000 × refresh)`) — the same clock ownership rule as ADR-0038's
   reap.

5. **Ghost state is bounded and explicit.** `lens` carries a
   `lens_ghost[LENSI_GHOST_MAX]` array (open-addressed by id, tombstone-free
   like the node store), `LENSI_GHOST_MAX = 16` concurrent ghosts. The 17th
   distinct id is refused (the call returns without effect), keeping the
   ceiling structural: a UI never legitimately has more than a handful of
   simultaneous exits, and a pathological host cannot grow lens's steady
   state.

6. **Damage and repaint are observable.** A frame with any live ghost sets
   `anim_pending`-equivalent repaint state: `lens_frame_needs_repaint`
   returns true while any ghost exists (its alpha is changing by contract —
   a host driving a fade re-calls `lens_set_ghost` every frame, and a ghost
   present but unrefreshed still counts down visibly). Rendering the same
   ghost twice in one frame is idempotent (last call wins for alpha).

7. **Ghosts are excluded from hit-testing, focus, and the a11y tree** — the
   snapshot is paint-only. The a11y removal event fires in the frame the
   data left (unchanged); the fade is decoration.

8. **Rust binding (`lens-rs`)** exposes `Ui::set_ghost(id, alpha)` guarded by
   the frame lifetime, mirroring `Frame::set_opacity`.

## Alternatives Considered

- **Extend the grace window (larger `LENSI_LEAVE_GRACE_FRAMES`).** Reject:
  leaves the actual gap — no render path — while inflating worst-case store
  retention ~8×. ADR-0038 already rejected per-node configurable grace for
  the same uniformity reason.
- **Keep-building (status quo).** Reject: forces hosts to retain logically
  deleted data, re-derive ids, and hand-drive fades; the immediate-mode
  contract inversion this ADR exists to remove.
- **Ghost from the display-list record cache.** Reject: records are
  canvas-owned and keyed to a live canvas's anchor state; a ghost must
  outlive arbitrary host frame pacing without dragging a canvas contract
  along, and freezing content into a record buys no replay (the snapshot is
  already frozen). Records also cannot re-tint cheaply — the alpha bake
  would invalidate them per frame.
- **CPU pixel copy (render the subtree to a texture, fade the texture).**
  Reject: introduces a render target, a pass, and a dependency on the
  render canvas at snapshot time; the snapshot must be capturable at
  `lens_end` (no canvas guarantees) and paintable on any later canvas.
- **Millisecond lifetimes.** Reject: lens owns no clock (force 2); frame
  counts are the only time base lens legitimately has.

## Consequences

- lens gains one public entry point, one internal snapshot table, and a
  render pass addition in band order. `lens_node` gains nothing; the ghost
  store is context-level, so the node ABI does not move.
- `lensi_render_node`'s command switch is factored into a shared emitter;
  the live path's behaviour is unchanged (records, scroll clipping,
  per-command arena rewind).
- Hosts delete their keep-building scaffolding: an exit animation becomes
  `alpha = ease(1 - t); lens_set_ghost(ui, id, alpha);` with no retained
  dead content.
- Bounded cost: ≤16 ghosts × ≤64 frames; each snapshot is one subtree's
  draw commands in `lensi_alloc` memory, freed on expiry or context
  destroy. The 17-ghost refusal is a visible ceiling, documented here.
- Tests (`tests/lens/test_ghost.c`): snapshot capture at last-live
  `lens_end`; survival across the arena reset; emission with the alpha bake
  on all four color-bearing kinds; band ordering after live nodes;
  non-interactivity (hit/focus/a11y); refresh-keepalive and the 64-frame
  expiry; the 16-ghost ceiling; NULL/unknown-id safety; canvas switch.
- ADR-0038's deferred note resolves: the grace window keeps its state
  retention role; leave *pixels* now come from ghost replay.
- Docs: `docs/reference/lens.md` (new — the lens reference page this ADR
  adds) documents the contract; symbols table gains the row.

## References

- [ADR-0038](0038-lens-node-state-gc.md) — the grace window whose render
  path this supplies; its per-node-state reclamation is untouched.
- [ADR-0068](0068-lens-frame-scoped-node-stamped-opacity.md) — the opacity
  bake (`lensi_opacity_color`) the ghost emitter reuses.
- [ADR-0030](0030-lens-damage-tracking.md) — damage tracking; ghosts mark
  the frame repaint-pending through the same query.
- [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md) — the band
  ordering ghosts join at the tail of.
