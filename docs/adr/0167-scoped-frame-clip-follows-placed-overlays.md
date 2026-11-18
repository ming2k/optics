---
id: ADR-0167
title: "A scoped frame clip follows placed overlays — the clip a chrome strip needs is not a parent viewport"
status: accepted
date: 2026-11-17
scope: [lens]
superseded_by: null
negative_knowledge: true
---

# ADR-0167: A scoped frame clip follows placed overlays

- Status: Accepted
- Date: 2026-11-17
- Deciders: Optics Architecture Working Group
- Related: [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md),
  [ADR-0077](0077-anim-motion-vocabulary-library.md),
  [ADR-0158](0158-transit-header-only-and-zero-so-motion-vocabulary.md)

---

## Context and Problem Statement

Lens clips children in exactly two ways:

1. A scroll container narrows its children to its padded viewport.
2. A skin may push a command-local clip for one node's own draw commands.

Neither expresses the shape a floating chrome strip actually needs. A carousel
panel owns a fixed body, but its cards, labels, and hover washes are placed
absolutely so they can overlap, scale, and animate independently. Placed nodes
deliberately escape every ancestor clip (ADR-0060), so wrapping them in a
container with a flow viewport clips nothing. Per-node clip arithmetic at the
call site works, but every new child has to remember the same boundary, and one
forgotten label paints outside the panel onto the desktop.

The missing mechanism is a **scope**, not a parent property: "every node built
between these two calls is clipped to this rectangle, including nodes that leave
the flow and join a higher band."

## Decision

Lens gains `lens_clip_begin` / `lens_clip_end` with a `lens_clip_opts`
descriptor. A begin intersects the current effective clip with the supplied
output-logical rectangle and pushes the previous value; an end restores it. The
stack is frame-scoped (`lens_begin` resets it), so a forgotten end cannot leak
into the next frame. A push beyond the fixed depth is counted so its matching
pop stays balanced instead of popping an unrelated outer scope, and the frame
reports overflow.

The effective clip is stamped onto every node at first touch this frame. The
stamp is carried through snapshot capture, replay, ghost snapshots, and damage
change detection. Unlike a parent's flow viewport, it survives ABS placement:
the stamp is intersected after the placed node's own `place_bounds`, so an
overlay in a higher band is clipped by the scope that built it. The snapshot
stores the intersected rectangle as both the node's hit region and the clip it
passes to descendants, so rendering and hit-testing cannot disagree.

The mechanism is rectangular and additive. It does not replace a rounded body
material, and it does not replace text ellipsization: a hard clip guarantees the
boundary, while the caller still shortens a label so a glyph is not cut in half.

## Alternatives

- **Reuse the scroll viewport.** Rejected: a scroll container also owns scroll
  offset, clamping, and a scrollbar; a chrome panel is not scrollable, and ABS
  children escape the viewport by design.
- **Reuse `lens_skin_clip_push`.** Rejected: it is command-local to one node's
  skin, cannot span sibling nodes, and runs at the wrong layer.
- **Clip every child at the call site.** Rejected: correctness becomes a
  per-child discipline. The surface that forgot one label is the regression this
  ADR exists to remove, and hit-testing still has to repeat the same arithmetic.
- **Rounded clip in the same API.** Deferred: the current need is a panel
  boundary; rounded content already comes from the material's façade clip.
  Adding a radius later is a descriptor field, not a second mechanism.

## Consequences

- A chrome overlay can place its children absolutely and still guarantee they
  never paint or hit outside its body.
- The clip is a build-time stamp, so a node's cached display list is keyed on the
  effective clip and invalidates correctly when the scope changes.
- Frame-clipped nodes participate in the existing `clip_changed` damage path;
  the old and new clip extents are both repainted.
- The stack is fixed-depth and allocation-free, preserving the steady-state
  zero-allocation contract.
- Hosts must use output-logical coordinates, the same space as `lens_input`
  and the canvas pass.
