---
id: ADR-0144
title: "Never partial-paint a swapchain — damage-rect passes and departed subtrees"
status: accepted
date: 2026-11-04
scope: [flux, lens]
superseded_by: null
negative_knowledge: true
---

# ADR-0144: Never partial-paint a swapchain

- Status: Accepted. **Decision 1 is superseded by [ADR-0149](0149-damage-scoped-windowed-frames.md)**;
  decisions 2–4 remain in force.
- Date: 2026-11-04
- Deciders: Optics Architecture Working Group
- Consulted: `libs/flux` (canvas pass envelope, surface, Vulkan backend), `libs/lens` (repaint query, store), `libs/iris` (all three hosts)
- Informed: Core Engineering
- Amends: [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md), [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md), [ADR-0069](0069-color-management.md)

> **Superseded in part by [ADR-0149](0149-damage-scoped-windowed-frames.md).**
> Decision 1 below — "flux never partial-paints a swapchain image" — is retired.
> A damage-scoped pass on a surface is honoured again, because the divergence
> this ADR could not repair (a rotation of images accumulating different patches)
> is now repaired at every acquisition from a persistent content image plus a
> write-epoch history. The *reasoning* in this document is not retired and should
> be read as the constraint ADR-0149 had to satisfy: Defect 2 and the rejected
> per-image flag are why the repair is a steady-state mechanism rather than a
> flag, and the negative-knowledge entries about pre-filled test destinations and
> initialisation flags still bind. Decisions 2–4 (partial passes on caller-owned
> targets, the store sweep for departed subtrees, persisted paint geometry) are
> unchanged.

---

## Context and Problem Statement

ADR-0143 made frame damage narrow. Before it, a single edit inside a FLOW
container reported enough of the display that a partial pass was effectively a
full repaint, which silently covered two independent defects. Localization
exposed both, and they present as the two symptoms a user reports as "the UI is
broken": a region that never updates, and a region that flips between two
frames.

### Defect 1 — a departed subtree owns pixels no walk can reach

`lensi_store_reap` frees a node only after `LENSI_LEAVE_GRACE_FRAMES`, but it
detaches the node from its parent's `first_child`/`next_sibling` chain on the
frame it stops being built. `damage_walk` traverses exactly that chain, so it
cannot see the node — while the node's `render_rect` still describes the last
pixels it painted, and those pixels are precisely what has to be erased.

Nothing else covers them either. For a **trailing** child no sibling moves into
its place, so no surviving node's geometry overlaps the vacated band. The region
stays frozen until an unrelated full-extent repaint happens to cross it.

The viewport fallback in `lens_damage_rects` hides this only in the degenerate
frame where the damage set is empty. Any frame that legitimately names another
region — a ticking clock, a hover ease — suppresses the fallback and ships the
under-covering set to the compositor.

### Defect 2 — a damage-rect pass diverges a rotating image set

A damage-rect pass sets `renderArea` to the dirty region and pairs it with
`FLUX_LOAD_LOAD`, so the pass composites onto whatever the destination already
holds and `record_output_blit` limits itself to `out_area`. That is correct for
the *destination as a buffer*. It is not correct for a **swapchain**, which is a
rotation of several images (`caps.minImageCount + 1`).

The partial pass advances only the image it acquired. The others keep frames
from earlier presents, and the compositor displays whichever it picks next, so
the patched region alternates between two contents as the image index advances.

A first attempt tracked, per image, whether it had ever received a full-extent
frame, and widened a partial pass when it had not. Instrumenting the material
gallery showed that this is **insufficient**, and the measurement is worth
recording because the reasoning error is subtle: tracking initialisation says
nothing about steady state. After the first full paint *every* image qualifies,
the flag goes quiet, and from then on each incremental patch lands on one image
and diverges it from the rest. The trace, counting how often one damage rect
reached each of the five on-screen images, was `31, 34, 35, 18, 18` — five
distinct UI states in steady-state animation, which is exactly the flicker.

## Decision

Partial paint is only valid on a destination known to hold a complete frame.
That is a property flux owns, and `flux_canvas_begin` is the single seam every
backend and every host already routes through, so it is where the rule is
enforced.

1. **flux never partial-paints a swapchain image.** `canvas_begin_pass_impl`
   discards a requested sub-rectangle whenever the destination is a surface.
   The full extent is painted into whichever image was acquired, so every image
   in the rotation is identical by construction and no per-image state can drift.
   The rule is unconditional because the divergence is not an initialisation
   problem: per-image completeness tracking was tried and measured to be
   insufficient (see Defect 2).
2. **A caller-owned target image still takes partial passes.** It is one
   persistent buffer, so successive damage-rect passes accumulate correctly and
   the ADR-0143 localisation is preserved wherever a host renders through
   `flux_image` targets rather than straight to a swapchain.
3. **lens sweeps the store for departed subtrees.** `damage_walk_leaving`
   walks the store's live list for `LENS_NODE_LEAVING` nodes and reports their
   last-painted bounds, plus their children's, because a departed container
   paints nothing itself.
4. **That geometry is persisted by the snapshot, never recomputed.** The
   command list a node's paint bounds are derived from lives in the per-frame
   arena, which `lens_begin` rewinds and recycles immediately, and a LEAVING
   node is never touched — so `lensi_node_reset_frame` never clears its `cmds`
   and the pointer survives while the memory behind it is handed to *this*
   frame's nodes. `snapshot_node` therefore captures `paint_bounds` (via
   `lensi_node_paint_bounds`, shared with the live damage walk so the two
   cannot disagree) while the command list is alive, and activation copies it
   into the node's `render_paint_rect`.

The two halves are independent: the first is a correctness rule in flux, the
second a completeness rule in lens. Neither is a heuristic, and neither depends
on the other's presence.

## Alternatives Considered

- **Full repaint whenever the acquired image index differs from the last painted
  one.** Rejected: it converts the common case into a full repaint, because with
  several images *every* frame acquires a different image. It would undo
  ADR-0143 entirely rather than make it sound.
- **Per-image "has this image had a complete frame" tracking, widening only when
  it has not.** Tried and rejected on measurement, not on taste: it fixes the
  first pass and nothing after it. Once every image is complete the flag is
  silent, and incremental patches then diverge the images (measured `31, 34, 35,
  18, 18` for one rect across five images). Correctness here is a steady-state
  property, so a flag that only describes initialisation cannot carry it.
- **Copy the last presented image into the acquired one before a partial pass.**
  Rejected for now: it is a full-surface blit on the critical path, which costs
  more than the full-extent paint it replaces. It is the shape of the eventual
  fix if the localisation win must be recovered, and it needs a
  "newest complete frame" reference that does not exist yet — a deliberate
  follow-up, not a flag away.
- **Force a full paint once per swapchain generation.** Rejected: it is only
  correct by accident of the image count and breaks silently when
  `minImageCount` changes.
- **Enforce the rule in iris instead of flux.** Rejected: three hosts would
  repeat it, and a fourth (headless, a canvas consumer) would omit it. The
  invariant belongs to flux's own swapchain.
- **Delete departed nodes immediately and paint over them.** Rejected: the leave
  grace window exists for legitimately re-entering nodes (ADR-0027), and the
  vacated pixels still need erasing when a node truly departs.

## Consequences

- No visible flicker from regional damage on a swapchain: the compositor can
  rotate through the images freely because they all hold the same frame.
- **ADR-0143's localisation does not reach a swapchain present.** A one-cell
  edit still *reports* a 26×20 rect — that part stands, and hosts that consume
  the set (`lens_damage_rects` callers) still get it — but `flux` paints the
  full extent into an on-screen image. The saving survives on
  caller-owned `flux_image` targets, where partial passes are still honoured.
  Recovering it for swapchains needs one persistent content image copied whole
  into the acquired one; that is deliberately left as follow-up work rather than
  approximated with per-image flags.
- No public ABI change: the widening lives inside `canvas_begin_pass_impl`, and
  the per-image completeness accessors that the rejected approach needed were
  removed again rather than left in the tree as dead mechanism.
- The damage walk gained an O(store capacity) store sweep per repaint frame. It
  reads an array the frame already touches and allocates nothing, so the
  zero-alloc frame-walk invariant holds.
- A host that paints its own content (`cfg->paint`) on a separate subsurface
  still presents that surface unchanged; the rule is about what flux writes into
  a swapchain image, not about how many surfaces a host composes.

## Negative Knowledge

- **Never assume the destination holds the frame that is already on screen.**
  A swapchain is a rotation of images; "the screen shows frame N" says nothing
  about the image being written now. Any pass that *preserves* pixels outside
  its area must first establish that the pixels inside its area are a complete
  frame.
- **Never trust `renderArea`-based repaint to bound the region a damaged node
  occupies.** It bounds where the renderer writes, not where stale pixels live.
  A node that left the tree owns pixels that no traversal of the tree can find.
- **Never derive "this node was a child" from its children.** `child_hash` and
  `last_child_hash` describe a node's child *sequence*, so a departed leaf — the
  most common kind — hashes to zero and reads as "never had a parent".
  Attachment is a property of the node, not of its subtree.
- **Never read damage geometry out of a node's own command list after the frame
  that built it.** `cmds` and the strings they point at are per-frame arena
  memory; `flux_arena_reset` rewinds a cursor and recycles immediately, and a
  node that stops being built is never reset, so its stale `cmds` aliases live
  allocations. Measured: adding one unrelated label elsewhere moved a departed
  row's reported damage from `{0,65 36x20}` to `{0,73 291x20}`. A stale region
  is not a safe failure — it is the same frozen-band bug this walk exists to
  fix, now with a data dependency on unrelated content.
- **Never let the "no region named" fallback stand in for damage tracking.**
  A fallback that fires only when the set is empty cannot cover an omission in a
  frame that names other regions, which is the frame where it is needed.
- **Never write a partial-paint test that pre-fills the destination.** Filling
  every image before exercising a sub-rect LOAD pass makes the pass look correct
  whatever the destination contract is, because the bug only appears on an image
  that was never completed. The reproduction has to paint *one* sub-rect onto a
  fresh surface and assert that the rest of the presented image is defined.
- **Never reason about steady state from an initialisation flag.** "Has this
  image ever had a complete frame" is true for every image one frame after the
  first full paint, and says nothing about whether the images still agree. Any
  per-image state on a rotation is a divergence source; only painting the same
  content into all of them is airtight. Measure the per-image patch counts, not
  the code path.

## References

- [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md) — The rect-list damage contract this makes sound (amended: damage is *reported* regionally, but a swapchain present paints the full extent; the localisation win applies to caller-owned targets).
- [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md) — Damage-driven repainting and partial composition.
- [ADR-0069](0069-color-management.md) — The working-space intermediate and the LOAD seed blit whose destination assumptions this bounds.
- [ADR-0027](0027-lens-retained-store.md) — The node store and its leave grace window.
