---
id: ADR-0155
title: "Frame damage is a native epoch ledger, not a caller-side per-image history"
status: accepted
date: 2026-11-09
scope: [flux, canvas, bindings]
superseded_by: null
negative_knowledge: true
---

# ADR-0155: Frame damage is a native epoch ledger, not a caller-side per-image history

- Status: Accepted
- Date: 2026-11-09
- Deciders: Optics Architecture Working Group
- Amends: [ADR-0149](0149-damage-scoped-windowed-frames.md)
- Consulted: `libs/flux` core/frame/canvas, `bindings/flux-rs`, Tessera
  compositor integration

---

## Context and Problem Statement

ADR-0149 introduced the persistent content image and an epoch history for
windowed surfaces. That design answered "what must this swapchain image receive
when it returns?" for the windowed path only. Offscreen surfaces rendered
directly into their ring images and left the same question to the caller.

The split produced four concrete defects:

1. **Two histories.** Tessera maintained its own per-image
   `Vec<FrameDamage>` and fanned a presented frame's delta into every other
   image. Flux maintained the epoch history for windowed surfaces. The two
   could disagree about which image was current, and the caller-side fan-out
   had to special-case first use and `Full` to avoid circulating it forever.
2. **The callers' delta was not a first-class input.** A frame could only
   declare damage through a canvas pass. The frame itself had no answer to
   "what is new in this frame?", so a compositor had to duplicate the engine's
   bookkeeping to produce a repaint scope.
3. **Submission and presentation were conflated.** A KMS `EBUSY` abandoned a
   frame that had already been rendered. The caller collapsed the retry to
   `Full` because it had no transactional way to preserve the abandoned
   delta. One missed flip became three full-output repaints.
4. **The write-set tracker was never wired.** `flux_frame_record_written` had
   no callers and an empty commit was read as "full extent". The windowed
   content sync therefore copied every swapchain image in full on every frame —
   the exact cost ADR-0149 exists to avoid.

## Decision Drivers

- **One ledger per surface.** The engine owns the images, so it must own the
  answer to "which image is current with which content epoch".
- **A delta is not a repaint scope.** A new frame changes some pixels. An image
  may also have missed older changes. Mixing the two into one history entry
  makes first-use `Full` circulate forever.
- **Submission is the content transaction.** A rejected external flip abandons
  presentation, not the rendered pixels or their history.
- **Scanout damage is a different question.** KMS needs "what has the display
  not seen?", not "what does this image need painted?".
- **No caller-side copy of an engine invariant.** A second implementation is a
  second answer waiting to drift.

## Decision Outcome

### 1. `begin_frame` carries the current delta

`flux_frame_begin_desc` gains:

```c
flux_damage damage;  /* current changes, physical surface pixels */
bool        damage_full;
```

`damage_full` is the explicit whole-extent verdict. Empty + `damage_full=false`
is a real no-change frame (pacing, capture, history repair), not `Full`. A NULL
`desc` keeps the conservative pre-damage contract: full.

### 2. `repaint_scope` is derived, never maintained by the caller

`flux_frame_repaint_scope()` returns the exact pixels this recording frame must
paint:

- offscreen / no content image: `history_since(image_epoch) ∪ current_damage`;
- windowed content-backed: `current_damage` only, because
  `flux_surface_sync_content` repairs the acquired image from the persistent
  content image at submit;
- first use, history overflow, or current `damage_full`: whole extent.

The returned set is copied into the caller's `out_rects` buffer during the
call; the caller owns no per-image vector and retains no borrow into the frame.

### 3. Submission commits the delta; present publishes the epoch

At `flux_frame_submit`:

- the **caller's current delta** is appended to the surface's epoch history;
- `image_epoch[acquired image]` advances to that epoch;
- for a windowed content-backed surface, `sync_content` copies the epochs the
  acquired image missed from the content image, then advances its epoch.

At `flux_frame_present`, `presented_epoch` advances to the frame's epoch. At
`flux_frame_abandon`, it does not: the image is fully rendered and current, but
the display never saw it. A retry reuses the same image and is asked to paint
only its new delta.

**The delta committed is never the repaint scope.** An offscreen image writes
the older history it missed as well; re-committing those writes as new damage
would make first-use `Full` circulate around the ring forever.

### 4. Scanout damage is history since the last present

`flux_surface_scanout_damage()` returns `history_since(presented_epoch)`. It
includes submitted-but-abandoned frames and is independent of the next image
acquired. A KMS/scanout consumer passes this set as its damage hint; a canvas
pass must not reuse it as a write scope.

### 5. An empty commit is a no-change epoch

`flux_damage_history_commit(h, nullptr, 0, false)` records an epoch with no
damage. Only `full=true` widens an empty set to the whole extent. The windowed
content sync can therefore be a no-op on a pacing frame.

### 6. Invalidation is explicit

`flux_surface_invalidate_damage()` resets the history, every image epoch, and
`presented_epoch`. Call after VT switch, backend takeover, or any event that
makes the display contents unknown. The next frame is full.

## Invariants

- **INV-1 — Delta authority.** The history stores the caller's current delta,
  never the derived repaint scope and never pass-write coverage.
- **INV-2 — Per-image epoch.** An image is current exactly when its epoch is the
  history's committed epoch. The image index, not the GPU frame slot, is the key.
- **INV-3 — Submit commits, present publishes.** A submitted frame's image and
  history are valid even if the external flip is abandoned.
- **INV-4 — First-use is per-image.** A first-use `Full` repaint does not enter
  the delta history; it only initialises that image.
- **INV-5 — Scanout hint is history since present.** The repaint scope is not a
  scanout hint; the scanout set is not a pass scope.

## Consequences

### Positive

- A `Busy` retry costs only the new delta, not three full-output repaints.
- Offscreen and windowed surfaces share one damage model; the caller never
  maintains a parallel image ledger.
- Windowed content sync no longer copies the full image on every frame.
- The KMS hint is correct across abandoned submissions and ring rotation.
- `flux_frame_repaint_scope` and `flux_surface_scanout_damage` make the two
  questions distinguishable in the API instead of overloading one enum.

### Negative / Trade-offs

- `flux_frame_begin_desc` and the epoch history grow slightly; pre-1.0 full
  versioning already covers the ABI break.
- A caller that declares no damage now gets a no-op scope rather than a
  conservative full repaint; `begin_frame` / `NULL desc` remains full for
  callers that do not opt into the contract.
- The engine trusts the caller's delta. A caller that changes pixels without
  declaring them can still strand those pixels; the engine no longer attempts
  to reconstruct a write set from draw calls.

## Rejected Alternatives & Negative Knowledge

### Caller-side per-image damage vector (the old Tessera model)

- **Why considered**: keeps the engine unaware of compositor ring policy.
- **Why rejected**: it duplicates a per-image invariant the engine already owns,
  and it made first-use `Full` and retry semantics caller-specific. The two
  ledgers drifted into exactly the stale-band and rollback symptoms this record
  exists to prevent.

### Commit the repaint scope (or pass-write coverage) as the delta

- **Why considered**: the pixels were actually written, so why not record them?
- **Why rejected**: an offscreen image also writes the history it missed. Those
  pixels are already described by older epochs. Committing them again turns a
  one-time image initialisation into a permanent `Full` history entry.

### Treat an empty commit as full

- **Why considered**: under-describing is the dangerous direction.
- **Why rejected**: it forces every windowed content sync to copy the full
  image, defeating ADR-0149. `full` is now explicit; no-change is explicit.
