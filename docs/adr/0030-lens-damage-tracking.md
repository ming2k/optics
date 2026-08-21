# ADR-0030: Lens damage / redraw tracking — deferred, full-tree repaint today

- Status: Accepted (Decision #3 superseded by implementation — see the
  status note below and ADR-0017's canvas display lists)
- Date: 2026-07-21
- Amended: 2026-08-21 — subtree skip IS now delivered, via canvas
  record/replay rather than a lens-side offscreen cache
- Scope: lens (L2 toolkit). Records the draw-list hashing infrastructure
  and the current "repaint the whole tree" policy.

## Context

Each frame, lens builds a per-node draw list
([ADR-0025](0025-lens-draws-through-flux-canvas.md)) and replays it
front-to-back. In principle, only subtrees whose draw commands changed
need to be re-rendered; unchanged subtrees could be blitted from a
cached image. The question is whether to enable that optimisation now.

Forces:

1. **Per-frame arena resets the draw list.** Commands are rebuilt each
   frame; structural equality must come from a hash, not pointer
   identity.
2. **The host clears the framebuffer each frame.** Today every lens
   consumer clears and repaints the whole surface, so skipping a
   subtree leaves a blank hole rather than the previous frame's pixels.
3. **Subtree offscreen caching is a prerequisite.** Real damage tracking
   needs an offscreen cache of each unchanged subtree to blit; lens does
   not own a surface and cannot allocate that cache today.

## Decision

1. **Record resolution-deferred draw commands.** Each node owns a
   `lens_draw_cmd` list of node-relative commands (rects, borders, text,
   images, icons, clip pushes/pops). Geometry is resolved against
   `final_rect` at replay. Implemented in `libs/lens/src/render/drawlist.c`
   and `libs/lens/src/render/replay.c`.
2. **Hash the draw list per frame.** `lensi_drawlist_push` maintains a
   rolling `cmd_hash` over the commands pushed this frame; the previous
   frame's hash is kept in `last_cmd_hash`. `lensi_mark_dirty` computes
   `subtree_changed` bottom-up so unchanged subtrees are identifiable.
3. **Do not skip subtrees yet.** `lensi_render_node` computes the change
   flags but deliberately renders every subtree every frame: the host
   clears the framebuffer, so skipping paints a hole. The hash is
   computed for future use and explicitly marked as disabled in
   `replay.c` with a note tying re-enablement to a subtree offscreen
   cache.
   *(Superseded 2026-08-21: `libs/lens/src/render/replay.c` now records
   each subtree's emission into a canvas display-list segment and
   replays it when the subtree's command hash, child hash, and the
   flux-text atlas generation are unchanged — the "offscreen cache"
   this decision waited for turned out to be the canvas-side segment
   pool, not a lens-side bitmap cache. The decision text above is kept
   for the reasoning that produced the hash infrastructure.)*
4. **Draw commands live in the per-frame arena.** Text runs are copied
   into the arena on push so caller buffers may be short-lived; other
   commands are arena-allocated as the list grows.

References: `libs/lens/src/internal.h` (`lens_draw_cmd`, `lens_draw_kind`,
node `cmds`/`cmd_hash`/`last_cmd_hash`/`subtree_changed`),
`libs/lens/src/render/drawlist.c`, `libs/lens/src/render/replay.c`.

## Alternatives Considered

- **Enable skipping now and hope the host preserves the framebuffer.**
  Reject: every current host clears each frame; the result is flicker
  and blank rectangles.
- **Drop the hash infrastructure until the cache lands.** Reject: the
  per-frame cost is negligible, and having it in place means flipping
  damage tracking on is a one-line change once the offscreen cache
  exists.
- **Per-pixel damage rectangles.** Reject: requires owning the surface
  and a compositor; lens is headless.

## Consequences

Positive:

- The draw list is a pure data structure, so the overlay layer
  ([ADR-0037](0037-lens-overlay-layers.md)) reuses the same emission
  logic for its sub-roots.
- The hash infrastructure is ready for the day an offscreen subtree
  cache lands.

Negative:

- Every consumer pays full-tree repaint cost today, even for static UI.
  Acceptable at current scale; the policy is explicit and documented.
- The disabled culling code is a trap for a future contributor who
  enables it without also adding the offscreen cache.

## References

- [ADR-0025](0025-lens-draws-through-flux-canvas.md) — drawing seam.
- [ADR-0037](0037-lens-overlay-layers.md) — overlay sub-roots reuse the
  render emission.
