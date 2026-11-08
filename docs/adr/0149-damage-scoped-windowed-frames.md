---
id: ADR-0149
title: "Damage-scoped frames on a windowed surface — content image and write-epoch history"
status: accepted
date: 2026-11-06
scope: [flux, lens, iris, bindings]
superseded_by: null
negative_knowledge: true
---

# ADR-0149: Damage-scoped frames on a windowed surface

- Status: Accepted
- Date: 2026-11-06
- Deciders: Optics Architecture Working Group
- Consulted: `libs/flux` (swapchain, frame lifecycle, canvas backends), `libs/lens` (damage walk, change detection), `libs/iris` (all three hosts, frame mailbox), `libs/prism` (backdrop materials)
- Informed: Core Engineering
- Amends: [ADR-0144](0144-partial-paint-requires-complete-image.md), [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md), [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md)

---

## Context and Problem Statement

ADR-0143 made frame damage narrow: a one-cell text edit reports a 26×20 rect
instead of the display. ADR-0144 then discovered that this localisation could not
*reach* a swapchain present, and paid for the discovery by making every on-screen
frame a full-extent repaint.

The reason ADR-0144 gave is worth restating precisely, because the decision below
depends on it. A swapchain is not a buffer; it is a **rotation** of several
images (`minImageCount + 1`). A damage-scoped pass advances only the image it
acquired. The others keep frames from earlier presents, and the compositor
displays whichever it picks next. Measured on the material gallery before
ADR-0144, one damage rect reached the five on-screen images `31, 34, 35, 18, 18`
times — five distinct UI states in steady-state animation, read by the user as
flicker.

ADR-0144's first attempt was a per-image "has this image ever had a complete
frame" flag, and its rejection is the load-bearing part:

> Tracking initialisation says nothing about steady state. After the first full
> paint *every* image qualifies, the flag goes quiet, and from then on each
> incremental patch lands on one image and diverges it from the rest.

That rejection was correct, and it is not what this ADR reverses. The flag failed
because divergence is a **steady-state** property, so it needs a steady-state
repair. ADR-0144 could not find one, so it eliminated divergence by construction:
paint the full extent into every image. That is sound, and it costs the whole
localisation win on the only surface type that matters to a user.

The cost is not small. A terminal cell, a HUD tick, a caret blink, and a dock
spring each repaint and re-composite the full output. ADR-0143's measurement —
`{0,0,1920,1080}` → `{0,0,26,20}` — describes a saving the pipeline reports but
never collects.

ADR-0144 named the fix and deliberately left it as follow-up rather than
approximating it:

> Recovering it for swapchains needs one persistent content image copied whole
> into the acquired one; that is deliberately left as follow-up work rather than
> approximated with per-image flags.

This ADR is that follow-up, with one correction to the sketch: the copy is **not**
whole. Copying the whole content image per frame is a full-surface blit on the
critical path, which ADR-0144 also measured and rejected as costing more than the
full-extent paint it replaces. The copy has to be scoped to what actually changed
since *that* image was last presented — which requires the surface to remember
what each frame wrote. That memory is the mechanism this ADR introduces.

## Decision Drivers

1. **Localisation must reach the screen.** A damage rect that no present walks is
   a measurement, not an optimisation.
2. **Divergence must be repaired, not assumed absent.** ADR-0144 proved the flag
   fails in steady state; the repair must therefore be a function of the frame
   that actually happened, not of whether the image was ever initialised.
3. **Never under-cover.** A dropped or truncated region leaves stale pixels on
   screen; an oversized region costs work. Every degradation in this design goes
   toward covering more.
4. **The rule belongs to flux.** Three hosts must not each re-derive it, and a
   fourth (headless, a plain canvas consumer) must not omit it. `flux` owns the
   swapchain, so `flux` owns the recovery.
5. **The risky part must be provable without a GPU.** Window arithmetic fails
   silently — a wrong answer is one wrong frame, not a crash. It has to be a pure
   function with a table test.

## Architectural Decisions

### 1. A windowed surface renders into a persistent content image

`flux_surface` gains one persistent image (`content`), recreated with each
swapchain generation. Every pass that targets the default attachment renders into
it, not into the acquired swapchain image:

- usage is `COLOR_ATTACHMENT | SAMPLED | TRANSFER_SRC` — written by the output
  transform, sampled by the canvas LOAD seed, read by the present-time copy;
- it is the single buffer that accumulates damage-scoped passes exactly as a
  caller-owned `flux_image` target does, so a partial pass onto it is sound for
  the reason ADR-0144 already accepted for targets;
- `flux_surface_render_image`/`render_view`/`render_bindless`/`render_layout_slot`
  are the single definition of "which buffer does this frame own", so the frame
  core and the canvas backend cannot disagree about it;
- offscreen surfaces keep rendering straight into their per-slot image. Their
  "rotation" is one image per frame slot, so partial passes already accumulate
  correctly and no recovery is needed.

The acquired swapchain image becomes a *presentation buffer*: it is transitioned
to `TRANSFER_DST` at begin and receives pixels only at submit.

### 2. The surface remembers what each frame wrote

`flux_frame` accumulates a write set — `flux_frame_damage_set`, bounded by
`FLUX_FRAME_DAMAGE_CAPACITY` (64) — from the canvas backend at the one point that
knows what reached the destination: the output-transform encode loop in
`vk_end_pass`. Each encoded damage rectangle is recorded through
`flux_frame_record_written`.

This is deliberately *not* the caller's declared damage. A caller can declare a
narrow scope and a pass can still write more (an over-wide layer composite, a
future backend that ignores the scope); recording the truth at the encode point
means the history can never under-describe what an image is missing. Over-covering
in the record costs a larger copy; under-covering costs stale pixels.

A frame that cannot describe its write set within the bound sets
`written_overflow`, which commits as "wrote everything".

### 3. The history is a bounded ring, and every degradation covers more

`flux_damage_history` (`libs/flux/src/core/damage_history.{h,c}`) holds
`FLUX_DAMAGE_HISTORY_DEPTH` (8) committed epochs. `flux_frame_submit` commits the
frame's write set before sealing the command buffer:
`flux_damage_history_commit(&s->damage_history, ...)`.

`flux_damage_history_since(h, since, ...)` answers "what must an image last
presented at epoch `since` receive", and its three answers are distinct facts:

| Situation | Answer | Why |
|---|---|---|
| `since == FLUX_DAMAGE_EPOCH_NONE` | full copy | the image holds nothing this surface would reproduce |
| `since == h->committed` | nothing (`count == 0`, `full == false`) | already current — the steady-state common case |
| `committed - since > DEPTH` | full copy | the entries it missed were overwritten; a partial union would silently drop the oldest |
| any intervening epoch was `full` | full copy | a full write has no rectangle list; nothing names what it changed |
| union exceeds the caller's capacity | full copy | a truncated list under-covers |
| otherwise | the union of the rectangles | the localisation win |

"Already current" and "never synchronised" are both empty results and are
deliberately *not* merged: one must copy nothing, the other must copy everything.
Encoding that distinction as a flag beside an empty set — rather than as a magic
rectangle — is what makes the wrong branch a compile-time-shaped mistake rather
than a silent one.

An empty or degenerate-only write set commits as `full`, for the same asymmetry:
a frame that has no narrower description of itself must not read as "nothing
changed".

### 4. Present copies the gap, per damage rect

`flux_surface_sync_content` runs from `flux_frame_submit`, after the last pass and
before `vkEndCommandBuffer`. That position is forced by information flow: it is
the first point at which the frame's full write set is known, and recording the
copy there keeps it in the frame's own submission — no second submit for the copy,
no additional semaphore, no cross-submit ordering to get wrong.

It issues one `vkCmdCopyImage` per damage rectangle (`vkCmdCopyImage` with a
`VkImageCopy` array), or one full-extent copy when the history says so, from the
content image (`TRANSFER_SRC`) into the acquired image (`TRANSFER_DST`).
`barrier_to_final` gained a `TRANSFER_DST_OPTIMAL` source case so the present
transition is correct for both paths.

Two consequences fall out and are recorded here because they are easy to get
wrong later:

- **Readback reads the content image**, not the acquired swapchain image. The
  content image is the complete frame; a damage-scoped frame's swapchain image is
  not.
- **The content image survives across frames** and is recreated only with the
  swapchain. `flux_surface_destroy_swapchain` destroys both, and the epoch history
  is reset with them because a resize moves every pixel.

### 5. The caller's damage list is a scope, not a hint

`flux_canvas_pass_desc` loses `render_offset_x/y` and `render_width/height` and
gains:

```c
typedef struct flux_damage {
    const flux_rect *rects;
    uint32_t count;
} flux_damage;

enum : uint32_t { FLUX_DAMAGE_CAPACITY = 64 };

typedef struct flux_canvas_pass_desc {
    /* ... */
    flux_damage damage;      /* empty = the full destination */
    bool clear_extent;       /* "the whole destination is new" */
};
```

A single rectangle is not a smaller version of this; it is a lossy projection of
it. The bounding box of a top HUD band and a bottom dock is the whole output, so
a pass API that accepts one rectangle *cannot express* the localisation ADR-0143
produces, no matter what the caller passes. Since this is a breaking API change
anyway, the lossy form is deleted rather than kept for callers that would misuse
it.

`clear_extent` exists because "paint everything" and "the damage set is empty"
must be expressible simultaneously: a frame can be forced full-extent while
another surface in the same frame is scoped.

### 6. Both backends honour the scope per rectangle, not per bounding box

The bounding box is only the dynamic-rendering `renderArea` (and the CPU
framebuffer clear bound). Every step that must touch *each* region iterates the
rectangles:

- **LOAD seed**: one blit per damage rect. Seeding the bounding box would decode
  the between-band pixels and then write them back.
- **Encode**: one output-transform blit per damage rect. Writing the box would
  copy the intermediate's unseeded between-band pixels over whatever the other
  widgets had put there — the corruption a "one scissor for the union" scheme
  produces.
- **Per-draw scissor**: `canvas_damage_select` narrows each draw's logical clip to
  the damage rects it intersects, and drops the draw when it intersects none.
  A draw spanning two rects gets their union (safe, and idempotent); a draw
  belonging to one band keeps that band's exact scissor, which is the common case.
- **Layers**: a layer's own pass and its composite are narrowed to the damaged
  part of their clip, so a regional update stays regional through a layer stack.
- **CPU backend**: clears and copies back per rectangle, and applies the same
  per-draw clip, so the software oracle and the GPU backend agree pixel for pixel.

### 7. Hosts pass the set through; the compositor's damage matches what was written

`iris_frame_packet`'s damage resolves into `iris_frame_damage` — a device-pixel
set plus a `full` flag. `iris_frame_damage_from` scales and clips but never
unions: collapsing to a box is exactly the information loss this pipeline exists
to avoid, so no helper performs it.

All six host paths (Wayland/Win32/Cocoa, compositor thread and single-thread) pass
the set straight to `.damage` and set `.clear_extent` when it is full. The Wayland
paths additionally damage the compositor for each rectangle written, rather than
the whole buffer: the set they damage is derived from the same resolution the pass
used, so the compositor never re-reads a band the frame left alone and never
misses one it touched.

A host with opaque canvas content (`has_custom_paint`) or a forced repaint asks
for the full extent, because its own content is not something lens can localise.

### 8. lens reports the pixels that change, including the ones outside the box

Partial rasterisation made three long-standing lens under-reports observable, and
they are fixed here rather than left for the next surprise:

- **One command-paint predictor.** `lensi_cmd_paint_extent` is the single
  definition of where a command paints, covering the cases where the painted
  extent is not `rel`: a border straddles its path by `width`, an image draws an
  outline underlay expanded by `outline_width`, an icon blits a haloed coverage
  mask, a connected tab reaches `text_size` below its box, and a tab indicator is
  re-anchored to the box's bottom edge. Before this, both `context.c` and
  `internal.h` had their own partial copies that handled only text, so an outlined
  image or icon stroke left a frozen fringe.
- **The tooltip's painted box, not its anchor.** Replay paints below the anchor
  with `2*pad` padding; the damage walk reported the anchor, so the appearing box
  and the disappearing box's band were both undamaged.
- **Ghosts by paint, not by container box**, for the same reason.
- **Clip changes and re-stacking**, tracked as `clip_changed` / `restacked` on the
  node and compared against the state the *presented* snapshot imposed
  (`prev_clip`, `prev_band`, `prev_band_order`, advanced only by
  `lens_snapshot_activate`). A scrolling viewport that shrinks and a placed node
  that changes band alter pixels that no descendant's own state describes.

`lensi_mark_subtree_changed` takes the `lens *` so it can resolve a placed node's
index in its band bucket — the order replay actually composites by, not the
sibling-disambiguation counter.

## Consequences

### Positive

- Regional damage reaches the screen. A one-cell edit now repaints one cell in the
  content image and copies one rect into the acquired image, instead of painting
  and compositing the full extent.
- The rotation cannot diverge, because divergence is repaired at every acquisition
  rather than assumed absent. The mechanism that failed ADR-0144's steady-state
  test now *is* the steady-state mechanism.
- The compositor receives the real damage set: Wayland's `wl_surface_damage_buffer`
  is called per rect, so the compositor's own damage tracking is not defeated by an
  over-wide region.
- Layer traversal, MSAA resolve and readback all follow the same render image, so
  there is no second place that can disagree about which buffer a frame owns.
- The window arithmetic is a pure module with a 32-check table test
  (`tests/flux/unit/test_damage_history.c`), so the failure modes that would
  otherwise surface as one wrong frame on one machine are caught on every build.
- The three lens under-reports that partial rasterisation exposed are fixed, and
  `test_repaint` covers them.

### Negative & Trade-offs

- One extra full-surface image per windowed surface (its size, once), and a copy
  band per present on the critical path — bounded by the damage area, not by the
  output.
- A frame that writes more than 64 disjoint regions, or an image that sat out more
  than 8 frames, falls back to a full copy. Both are bounded, and both are
  observable in the epoch history rather than guessed.
- `flux_canvas_pass_desc` changed shape: `render_offset_x/y`, `render_width/height`
  are gone, replaced by `flux_damage` + `clear_extent`. This is a source-breaking
  change for every caller, including `flux-rs`; it is taken deliberately because
  keeping the single-rect form would keep a way to express a lossy scope.
- A pass whose damage set names no pixel of its destination is rejected
  (`FLUX_ERROR_INVALID_ARGUMENT`) rather than silently skipped, so a caller cannot
  believe it painted when it did not.
- The content image must be recreated on resize, which resets the epoch history
  and makes the next frame of each image a full copy. That is one frame per resize,
  which the resize path already forces anyway.

## Rejected Alternatives & Negative Knowledge

### 1. Per-image "has this image been fully painted" flags
- **Why considered**: it is the smallest possible state, and ADR-0144 rejected it
  on measurement rather than taste.
- **Why rejected**: it describes initialisation, not steady state. One frame after
  the first full paint every image qualifies and the flag is silent, so incremental
  patches diverge the rotation again (measured `31, 34, 35, 18, 18`).
  **Never reason about steady state from an initialisation flag.**

### 2. Copy the whole content image into the acquired image every frame
- **Why considered**: it is the sketch ADR-0144 named, and it is trivially correct.
- **Why rejected**: a full-surface blit per frame on the critical path. ADR-0144
  measured it as costing more than the full-extent paint it replaces, so it buys
  correctness and sells the entire win. Scoping the copy to the epoch gap is what
  makes the mechanism pay for itself.

### 3. One scissor for the damage set's bounding box
- **Why considered**: it needs no per-rect encode loop and no per-draw scissor
  selection, so it is the obvious simplification.
- **Why rejected**: within one pass the bounding box of two distant bands is
  nearly the whole output. A draw scissored to that box writes the widgets between
  the bands, and an encode of that box copies the intermediate's unseeded pixels
  over them. The result is not a slightly larger repaint — it is corruption.
  **Never treat the damage bounding box as the damage set.**

### 4. Express the damage scope in iris, or in each host
- **Why considered**: keeps the canvas API unchanged.
- **Why rejected**: three hosts would repeat it and a fourth (headless) would omit
  it, and the information needed — "what did this frame actually write" — lives at
  the encode point inside flux. A host cannot know it.

### 5. Keep `render_offset_x/y`/`render_width/height` alongside `flux_damage`
- **Why considered**: avoids a breaking change for existing callers.
- **Why rejected**: the single-rect form cannot express a set, so any caller that
  kept using it would silently lose localisation; and the project forbids carrying
  compatibility shims for their own sake. The lossy form is deleted.

### 6. Damage the compositor with a single union (or the whole buffer)
- **Why considered**: `wl_surface_damage_buffer` accepts one rect, so the union is
  the path of least resistance, and the whole buffer is what ADR-0144 did.
- **Why rejected**: the union defeats the compositor's own damage tracking for the
  same reason it defeats the rasteriser's; and the whole buffer forces the
  compositor to re-read a surface the frame barely touched. The set is available,
  so it is used.

### Negative knowledge

- **Never commit a frame with an empty write set as "nothing changed".** An empty
  set means the frame could not describe itself; reading it as a no-op is the one
  direction that leaves stale pixels on screen permanently.
- **Never compare change detection against the frame that was *built*.** A built
  frame may be superseded and never presented, so its clip or band is not what the
  user saw. `prev_clip`/`prev_band*` advance on `lens_snapshot_activate` only.
- **Never derive a command's painted extent from its `rel` rect alone.** Borders,
  image outlines, icon halos and connected tabs all paint outside it, and a frozen
  fringe is the symptom.
- **Never report a tooltip's anchor as its damage.** The painted box sits below the
  anchor; reporting the anchor leaves both the appear and the disappear undamaged.
- **Never hash a pixel dependency as a pointer.** `cmd_hash` covers an image by
  pointer identity, so a texture updated in place changes pixels with no delta.
  That remains true and is out of scope here; it is listed so the next reader does
  not assume damage is complete for animated textures.

## Links

- Amends: [ADR-0144](0144-partial-paint-requires-complete-image.md), [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md), [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md)
- Supersedes the swapchain clause of [ADR-0144](0144-partial-paint-requires-complete-image.md) decision 1 (a sub-rect pass on a surface is now honoured, because the rotation is repaired).
- Related: [ADR-0069](0069-color-management.md) (the working-space intermediate and LOAD seed whose per-rect application this narrows), [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md) (the mailbox the damage set travels through), [ADR-0027](0027-lens-retained-store.md) (the leave grace window behind departure damage).
- Standards: [Architecture](../dev/standards/architecture.md), [Invariants](../dev/standards/invariants.md), [ADR Index](index.md)
