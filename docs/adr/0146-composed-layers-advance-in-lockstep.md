---
id: ADR-0146
title: "Composed layers advance in lockstep — synchronized subsurfaces, single present point"
status: accepted
date: 2026-11-04
scope: [iris]
superseded_by: null
negative_knowledge: true
---

# ADR-0146: Composed layers advance in lockstep

- Status: Accepted
- Date: 2026-11-04
- Deciders: Optics Architecture Working Group
- Consulted: `libs/iris` (Wayland host, subsurfaces, frame loop), `libs/flux` (surface present)
- Informed: Core Engineering
- Amends: [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md), [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md)

---

## Context and Problem Statement

A host that paints its own canvas content gets two Wayland surfaces: the parent
(base) surface for the host's content and a subsurface for lens chrome, stacked
above it (ADR-0123). The two are the same picture at two depths and must be
presented as one.

The implementation drifted from that. The subsurface was created desynchronized
(`wl_subsurface_set_desync`), which makes the surface apply its own commits
immediately rather than caching them until the parent commits, and the dual-plane
path presented each layer inside its own block:

```
[record host] -> commit host          (present #1)
[record ui]   -> commit ui            (present #2)
```

Two commits per iteration against a desynchronized subsurface means the
compositor performs **two** compositions: one when the parent commit lands, and
again when the subsurface's commit lands. The UI layer therefore updates at twice
the base layer's rate, against a base that has not changed between the two.

The visible symptom depends entirely on the content:

- **Static content** — both compositions show the same picture. Nothing to see.
- **Animated content** — the second composition shows a UI frame against a
  one-frame-old base. Because the loop then repeats, the observation is a
  persistent offset between layers combined with double-rate updates of the top
  layer: "an extremely rapid, barely visible full-window flicker".

Third-party applications built from lens components never triggered it: no
`cfg->paint`/`cfg->prepare`, so no second surface, so no second commit. Only the
in-house material showcases, which composite an animated canvas backdrop under
animated chrome, showed it. That asymmetry is the tell — it is a *composition*
scheduling property, not a material or effect property.

## Decision

1. **The UI subsurface is synchronized.** `wl_subsurface_set_sync`. The surface
   commit becomes the transaction boundary: cached subsurface state is applied
   when the parent commits, so both layers change exactly once per committed
   frame and the compositor composites them in one pass.
2. **There is one present point per iteration, UI first.** Both layers are
   *recorded* first and *presented* afterwards, together, with the **UI
   subsurface committed before the base**. Synchronized mode inverts the order
   that looks natural: a synchronized subsurface's commit only *caches* state,
   and the parent's commit is what applies the cached state of both surfaces and
   triggers one composition. Committing the base first would apply the previous
   frame's chrome next to this frame's base — reconstructing the same one-frame
   offset from the other side. Recording is independent of presentation in flux
   — a frame is submitted, and `flux_frame_present` is the commit — so deferring
   the presents costs nothing and removes the interleaving entirely.
3. **A failed submit presents nothing.** If either layer's submit fails, that
   layer's frame is dropped rather than presented, so a partial pair is never
   committed.

## Alternatives Considered

- **Keep desync and accept the doubled cadence.** Rejected: it is not a tuning
  choice, it is a wrong picture. The compositor shows a top layer that is a
  frame ahead of the layer beneath it, on every frame, forever.
- **Present the UI only every other frame.** Rejected: it halves UI
  responsiveness to make a scheduling bug look intentional, and it still
  composites two different times.
- **Merge the two surfaces into one.** Rejected: the split is deliberate
  (ADR-0123) — it lets the compositor blend host content and chrome on separate
  planes without a full-surface readback, and it is what makes the host's opaque
  canvas and lens's premultiplied chrome independent. The defect was the
  coupling of their commits, not the separation of their storage.
- **Keep desync and present both from one thread with a shared grant.** Rejected:
  desync exists to let a subsurface advance independently of its parent; using
  it and then forcing lockstep means paying for a mechanism that is deliberately
  disabled. Synchronized mode *is* that lockstep, expressed once by the protocol.

## Consequences

- Animated content composites as one consistent picture. The flicker disappears
  without touching any material, blur, or damage code.
- Both layers advance at the base layer's cadence. A chrome-only change (a hover
  fade) now waits for the host commit rather than applying instantly. This is
  the correct trade for a composed window: the alternative is a layer that
  visibly detaches from the content it is drawn over.
- The decoupled presenter thread (ADR-0124) is only started for hosts without
  `cfg->paint`/`cfg->prepare`, so it never coexists with the dual-plane path; it
  presents a single surface and is unaffected by this decision.
- Chrome-only damage no longer implies an immediate, unpaired present. Hosts that
  want the UI to advance must commit the base, which is what a frame is.

## Negative Knowledge

- **Never order a synchronized subsurface's commit after its parent's.** The
  parent commit is what applies the cached state, so the UI commit must land
  first; the base commit last. The reverse order silently presents one frame of
  stale chrome with every frame of new base — the identical defect, mirrored.
- **Never present composed layers from separate points in one iteration.** The
  interleaving is invisible in the code and invisible on screen for static
  content, and it only surfaces when both layers animate. Order the records,
  then order the presents, once.
- **Never use `wl_subsurface_set_desync` for a layer that is part of one picture.**
  Desync means "advance independently of my parent", which is the opposite of
  what a composited layer needs. It is the right tool for a video or a cursor
  overlay, not for chrome drawn over the content it annotates.
- **Never accept "the effect/material is flickering" as a diagnosis.** Bypassing
  the blur and removing all four material slabs both left the measured temporal
  delta unchanged or worse. The compositing schedule was the cause; the effects
  were where the eye noticed.
- **Never trust a whole-screen mean-delta metric for a layer-pairing defect.** A
  consistent one-frame offset between two layers does not change *how much*
  differs between two captures — only the phase between layers. Five A/B
  experiments reported "no change" for exactly this reason before the code was
  read. Phase-sensitive defects need phase-sensitive measurement.

## References

- [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md) — Layered composition on subsurfaces and damage tracking (amended: the subsurface is synchronized and both layers present at one point).
- [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md) — The decoupled presentation thread, which serves single-surface hosts only.
- [ADR-0144](0144-partial-paint-requires-complete-image.md) — Swapchain paint completeness; conflict-free because the same queue serialises submissions.
