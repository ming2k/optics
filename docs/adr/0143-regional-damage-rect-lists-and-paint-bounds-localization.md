---
id: ADR-0143
title: "Regional frame damage as a rect list, and paint-bounds localization"
status: accepted
date: 2026-11-03
superseded_by: null
scope: [lens, bindings, architecture]
negative_knowledge: true
---

# ADR-0143: Regional frame damage as a rect list, and paint-bounds localization

- Status: Accepted
- Date: 2026-11-03
- Scope: `libs/lens/include/lens/lens.h`, `libs/lens/src/core/context.c`, `libs/lens/src/render/replay.c`, `libs/lens/src/internal.h`, `tests/lens/test_repaint.c`, `bindings/lens-rs`.
- Implementation: Landed. Adds `lens_damage_rects` (bounded disjoint rect list) and derives `lens_damage_rect` from it. Introduces per-node `self_changed` and paint-bounds localization.
- Amends: [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md).
- Depends on: [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md), [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md).

---

## Context and Problem Statement

ADR-0123 made lens's damage query the seam that lets a host skip work: `lens_frame_needs_repaint` answers *whether*, `lens_damage_rect` answers *where*. Tessera consumes both to drive per-output partial composition, and hands the result to the KMS `FB_DAMAGE_CLIPS` hint and to Vulkan's `renderArea`.

Two properties of the query made that pipeline inert:

1. **`lens_damage_rect` returns one rectangle.** It unions the root, every placed band, the caret, the tooltip, and every ghost into a single bounding box. Two disjoint bands — a status strip at the top and a dock at the bottom — therefore reported one box spanning the entire output. Every downstream consumer inherited that union: the compositor's damage list held one rect, and the render area it derived was the whole framebuffer. A frame that changed two 40 px strips re-composited 3072×1920 pixels.

2. **A FLOW descendant reported its container's layout box.** Damage accumulated over `final_rect` of every changed node, and `subtree_changed` rolls up from all FLOW descendants. An edited label inside a stretch column reported the column — and since the root column is stretched to the display, a one-cell terminal edit reported `{0, 0, 1920, 1080}`. The root's own `final_rect` is the entire viewport, so any base-tree change was indistinguishable from a full repaint.

Measured against the shipped library (1920×1080, two `LENS_BAND_CHROME` bands at `{0,8,600,32}` and `{0,994,500,74}`):

| Frame content | `lens_damage_rect` before | Damage that actually changed |
| :--- | :--- | :--- |
| only the HUD band | `{0,8,600,32}` | `{4,12,34,20}` |
| both bands | `{0,8,600,1060}` | two strips |
| one FLOW label | `{0,0,1920,1080}` | `{0,0,36,20}` |

Consequences for the host were severe and not merely cosmetic. Because the query could only answer with a near-full-output box, Tessera's own containment heuristic (which reacts to a full-display footprint by substituting decoration bands) then *discarded* the middle of the screen, so genuine full-screen changes were clipped away. The reporting defect and the resulting mis-clipping shared one root cause: the query could not describe a set.

## Decision Drivers

1. **Damage is a set, not a box.** The union of two disjoint regions is not the region that changed. Any API that can only answer with a box forces every consumer to over-repaint, and forces a consumer that tries to compensate to guess.
2. **`[INV-ARCH-OPTS]`**: every public entry point takes exactly one typed descriptor. No `_v2`, no `_list` sibling that duplicates a scalar form.
3. **Zero allocation on the frame path.** The damage walk runs every frame; it may not allocate (`[INV-ARCH-ZEROALLOC]`).
4. **One definition of painted geometry.** Damage must report exactly the pixels replay writes. Two independent geometry resolvers is a guarantee that they will disagree.
5. **Bounded contracts.** A host sizes its buffers statically; the API must state a hard bound and degrade safely past it.

## Architectural Decisions

### 1. `lens_damage_rects` is the primary query; `lens_damage_rect` is its union

```c
enum : uint32_t {
    LENS_DAMAGE_RECTS_CAPACITY = 64,
};

typedef struct lens_damage_opts {
    flux_rect *rects;   /* nullptr or capacity == 0 asks for the count alone */
    uint32_t capacity;
} lens_damage_opts;

LENS_API uint32_t lens_damage_rects(const lens *ui, const lens_damage_opts *opts);
```

The return value is the number the damage resolves to; the written prefix is always disjoint and always inside the layout viewport. `nullptr`/zero capacity is a legal count-only query, so a host that wants an exactly-sized buffer never needs a second contract. `lens_damage_rect` is retained as the documented union of the list — a host that can scissor only once still gets the same answer the list describes, and an existing single-region consumer keeps a meaningful (if conservative) result.

### 2. Ownership decides granularity, not the tree shape

The walk applies two rules, and only two:

- **An ABS node owns its whole subtree.** A placed band is drawn as one body; anything that changed inside it changes pixels that body owns. The walk sets `owned` and stops distinguishing below that node.
- **A FLOW node contributes only its own paint delta.** `subtree_changed` is *not* consulted for FLOW nodes: it rolls up from every flow descendant, and the root's own rect is the display.

`visual_revision != presented_revision` (drift) is therefore consulted only inside owned bodies, where it catches a body whose activated snapshot never matched it. Consulting it for a FLOW node reintroduces the full-display report through the root.

### 3. Damage reports paint bounds, not layout boxes

`final_rect` describes where a node was *arranged*, which for a stretched leaf is far larger than what it *paints*. Damage therefore accumulates `lensi_offset_rel(final_rect, cmd.rel)` over the node's draw commands, skipping `CLIP_PUSH`/`CLIP_POP` (they paint nothing; including them would re-widen every clipped subtree to its clip rect).

A leaf's accumulated bounds are then capped at its intrinsic `measured` extent. This is the one place the two disagree and the conservative direction matters: a text command is emitted with `rel.w == 0`, meaning "fill the box", which is exact for a container background but describes the whole arranged box for a label whose glyph run is a few dozen pixels wide. Capping a container would be wrong (its background really does span the box) and is why the cap keys on `is_container`.

The command-geometry resolver moved from `replay.c` into `internal.h` as `lensi_offset_rel` so the rasteriser and the damage walk share one implementation.

### 4. Bounded, normalized, and safely degraded

The walk accumulates into a fixed `LENS_DAMAGE_RECTS_CAPACITY` scratch array — no allocation on the frame path. `damage_normalize` then removes rectangles fully contained in another (overlap is safe but multiplied work) and sorts top-left first for a stable host-side order.

A frame needing more regions than the contract carries returns **one bounding box** covering the whole set rather than a truncated list. The asymmetry is deliberate and matches the rest of the damage pipeline: a dropped region leaves stale pixels on screen, while an oversized region only costs a hint. A frame that needs a repaint but names no region falls back to the viewport for the same reason.

### 5. Per-node `self_changed`

`lensi_mark_subtree_changed` now records `self_changed` (this node's own paint delta: lifecycle, geometry, opacity, eased hover/active, draw-list hash, own child sequence) alongside the rolling-up `subtree_changed`. Both are computed in the same single bottom-up pass, so the finer answer costs one extra store per node.

## Consequences

- Regional composition works: a HUD tick and a dock spring are two strips, not one nearly-full-output box, and the host's KMS damage-clip hint receives the real list.
- A one-cell text edit reports the cell. Measured: `{0,0,1920,1080}` → `{0,0,26,20}`.
- Hosts can stop maintaining their own containment heuristics for the "lens reported the whole screen" case, because lens no longer makes that claim.
- `lens_damage_rect` remains supported with unchanged semantics (union), so a single-region consumer is unaffected.
- The rect bound is 64. A pathological frame reports its bounding box; this is correct, bounded, and observable through the returned count.

## Negative Knowledge

- **Never report an arranged box as a paint bound.** `final_rect` is where layout put a node, not where it drew. A stretched leaf's box is its column; the root's is the display. Consuming layout boxes as damage made every FLOW edit a full repaint, and no amount of downstream filtering can recover the region that was actually dirty.
- **Never consult a rolling-up revision at an unowned node.** `visual_revision` and `subtree_changed` both aggregate descendants. At a FLOW node they describe the subtree, and pairing that with the node's own box is precisely the "whole display" report.
- **Never truncate a damage list to fit a buffer.** Under-reporting is a visual artifact; over-reporting is a performance hint. The contract must degrade toward the box.
- **Never union damage where the consumer can take a set.** The union is a lossy projection: a HUD strip and a dock strip have a union of nearly the whole screen, and every consumer of that union loses the localization permanently.

## References

- [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md) — Regional damage-driven repainting (amended: the query now describes a set).
- [ADR-0060](0060-lens-single-tree-placement-and-z-bands.md) — Z-ordered absolute placement and the band buckets.
- [ADR-0094](0094-lens-scene-snapshots-and-invalidation.md) — Immutable scene snapshots and the presentation handshake (`presented_revision`).
- [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md) — Single Opts Descriptor API Paradigm.
