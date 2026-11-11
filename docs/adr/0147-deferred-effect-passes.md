---
id: ADR-0147
title: "Deferred effect passes — register, require, flush, prune"
status: accepted
date: 2026-11-04
scope: [flux, prism]
superseded_by: null
negative_knowledge: true
---

# ADR-0147: Deferred effect passes

- Status: Accepted
- Date: 2026-11-04
- Deciders: Optics Architecture Working Group
- Consulted: `libs/flux` (effect runtime, frame submission, canvas sampling seam), `libs/prism` (material filters)
- Informed: Core Engineering
- Extends: [ADR-0089](0089-rendering-program-and-execution-plan.md), [ADR-0091](0091-drawing-group-and-effect-semantics.md), [ADR-0092](0092-resource-planning-and-retirement.md)

---

## Context and Problem Statement

Applying an effect to a frame was unconditional GPU work. A host called
`flux_blur_filter_apply`, the runtime recorded four compute dispatches into the
frame's command buffer, and that was that. Whether anything ever *sampled* the
result was irrelevant — nothing in the API connected the effect to its consumer.

The consequence was measured, not hypothetical. The material showcase computed a
4-tap Dual-Kawase pyramid over a 2560×1600 capture every frame, including while
the app sat in its `Raw Backdrop` view, where **no material samples the blur at
all**. The GPU did that work because it had been asked to, and there was no
mechanism by which an effect could learn it was unwanted.

Two forces make this a real problem rather than a missing micro-optimisation:

1. **The engine cannot infer liveness from image handles**, right for the same
   reason [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md) exists:
   a pooled blur output hands back the same `flux_image *` every frame, so
   identity says nothing about whether a consumer will appear.
2. **The host is the only party that knows the answer**, and the burden must not
   fall on it. A "call this only when needed" rule in prose is exactly the class
   of admonition this project replaces with mechanism
   ([`docs/dev/standards/invariants.md`](../dev/standards/invariants.md) §1:
   mechanisms over exhortations). Every future host would have to re-derive the
   rule correctly, and one that gets it wrong pays silently in GPU time.

A third consideration shaped the response. [ADR-0089](0089-rendering-program-and-execution-plan.md)
already commits Optics to dependency-driven execution and a `RenderPlan`
compiler. That program is the long-term authority and is *not* replaced here.
This ADR adopts the smallest mechanism that delivers liveness-driven pruning
today, in the shape the eventual plan compiler subsumes rather than contradicts.

## Decision

An effect **registers** a pass; it does not record one.

### Phasing — state this, do not let the document outrun the code

This ADR landed in two phases, and the second one is where the value lives. Say
so plainly, because the failure mode this family of records exists to prevent is
a document that describes more than the code does.

| Phase | Content | Status |
|-------|---------|--------|
| 1 | Scheduling mechanism: register / require / flush / prune, wired into the two reusable flux filters (`flux_blur_filter_apply`, `flux_shadow_filter_apply`) | **Landed** |
| 2 | The five Prism materials register passes too, so their dispatches are also prunable | **Landed** (see §"Prism participates") |

Phase 1 alone removes a measured defect (an unsampled full-resolution blur
pyramid running every frame), so it was worth landing on its own. It does *not*
by itself justify the sentence "all GPU work is now prunable": five Prism
materials — measured at 4.8 ms/frame for 64 liquid-glass bodies on a 1024×1024
target — recorded their dispatches eagerly in that phase, and the reason was
structural rather than an oversight: `flux_plan_register` lived in
`libs/flux/src/core/plan.h`, an internal header, and `prism` is a separate
library that includes only `<flux/...>` public headers. Phase 1 could not have
reached them without opening the mechanism across the library boundary, which is
exactly what Phase 2 does.

### Prism participates (Phase 2)

The seam is `flux_frame_register_pass` / `flux_frame_require_pass`, declared in
`<flux/vulkan.h>` because a pass callback takes a `VkCommandBuffer` and a
backend-agnostic caller must never see it. `prism` already depends on that
header, so the mechanism crosses the boundary without `flux` learning what a
material is (ADR-0063 intact) and without `prism` seeing the plan's internals
(`flux_frame` stays opaque — flux copies the caller's parameters into its arena
itself).

All five materials register: liquid glass, frosted, acrylic, mica, and the
layered backdrop. Each snapshots its dispatch parameters into the plan arena at
apply time and returns its output handle immediately, exactly as before. The
shared adapter lives in `prism_material_frame.h` next to the slab geometry and
body table, for the same reason those do: five private copies would drift.

Two properties the migration had to preserve, both verified by the existing
suites rather than assumed:

- **Outputs are identical.** `test_prism_golden` stays at `max_delta = 2`,
  `test_liquid_glass` passes all 303 assertions, and each material's own
  integration test passes unchanged.
- **Registration failure degrades to eager recording.** An exhausted arena or
  full plan records the pass inline, so pruning can never cost correctness.

`test_plan_prune` now also asserts the Phase 2 property directly: a liquid-glass
apply whose output nothing draws must leave its pass unexecuted while the blur it
samples still runs. Disabling pruning (marking every pass required at flush)
fails that assertion, so it is load-bearing rather than decorative.

### What framegraph does NOT and should not govern

Deferral applies to **compute work recorded outside a render pass**. Three
categories are structurally outside it, and saying so is part of the contract:

| Category | Why | Consequence |
|----------|-----|-------------|
| Canvas draws (`canvas/backend_vk.c`) | Graphics work must be recorded *inside* a render pass; a plan pass must be recorded *outside* one (Vulkan forbids compute inside rendering). | The canvas pass sits *on* a plan boundary (`flux_canvas_begin` flushes), but its inner draws are inline by necessity. |
| Scene 3D draws (`flux_scene_draw_mesh`) | Same reason. | Same. |
| One-shot primitives (`flux_effect_blur`, `flux_effect_shadow`) | They take a bare `VkCommandBuffer` and no `flux_frame`, so there is no plan to register with. | They cannot be pruned. They **can** be made safe, and were: see below. |

The upper layers need nothing from this mechanism and got nothing from it, which
is not an omission: an audit of `lens`, `vista`, `glyph`, `iris` and `transit`
finds **zero** `flux_compute_dispatch` and **zero** `VkCommandBuffer` references.
They produce display lists, snapshots, meshes and glyph runs — CPU-side value
data — and never record GPU commands. Framegraph's influence on them is
therefore indirect: whether their draws survive depends on whether the images
they sample have a live producer, not on anything they call.

### The unmanaged path must still be safe

Not being prunable is acceptable. Reading an unwritten image is not, and before
this was fixed the unmanaged path did exactly that: feeding a deferred blur
output into `flux_effect_blur` was **accepted**, so the exact-Gaussian primitive
sampled last frame's pixels under this frame's handle. Verified by running that
case, then verified fixed by the same case plus its converse.

The fix is a single flag on the image (`deferred_pending`), owned by the plan —
registration sets it, execution and pruning clear it — and a public check,
`flux_frame_reject_pending_inputs`, for recording paths that hold only an image
and no frame. Both one-shot primitives consult it after validating their
descriptor and before dereferencing the image, so a garbage pointer in a
validation test still receives the argument error it expects rather than a
segfault.

The alternative — deleting the one-shot primitives — was rejected on evidence,
not taste: they are not duplicates. `flux_effect_blur` is an exact Gaussian with
per-pixel kernel weights; the reusable filter is a four-pass Dual-Kawase
pyramid. They are different operators for different jobs, and the one-shot forms
are also the only entry point that needs no `flux_frame` (Rust's
`effect_shadow` is such a consumer). Deleting them would remove capability while
leaving the stale-read class of bug reachable from any custom compute pass.

1. **Registration is bookkeeping, not recording.**
   `flux_plan_register` appends a pass record — a record function, arena-copied
   parameters, input images, output images — to the frame's plan. The caller
   still receives its output image handle immediately, and every input and output
   is retained by the frame exactly as before. `flux_blur_filter_apply` and its
   siblings keep their existing signatures and ownership contracts
   ([ADR-0090](0090-display-list-resource-ownership.md) is untouched).

2. **Building is on demand.** The first consumer that requires an output marks
   its pass required and builds the pass body into a secondary command buffer
   owned by the frame slot. Repeat requires are idempotent. Producers a pass
   reads are built first, so a chain (blur → material → surface) resolves in
   dependency order.

3. **Execution happens at a pass boundary.** `flux_plan_flush` emits
   `vkCmdExecuteCommands` for every required pass in registration order.
   Registration order is declaration order, so the command stream reproduces the
   sequence an eager recorder would have produced. A flush is legal only outside
   a render pass, which is the same constraint compute already had.

4. **Pruning is absence of readers.** A pass nothing required is never built and
   never executed. Its record is counted and dropped at submit. There is no
   heuristic and no policy flag: a pass with no consumer cannot affect a
   presented pixel, so skipping it is observationally identical to running it.

5. **Materialization is explicit across the compute/render seam.** A consumer
   that records its own compute (a Prism material filter) calls
   `flux_frame_materialize_image(frame, blurred)` at its boundary before
   recording its dispatches. That call requires the producer *and* flushes it
   immediately, so the blur precedes the material in the command stream.

6. **Sampling a deferred output inside a canvas pass is a contract violation and
   is rejected.** Compute cannot be inserted inside a render pass, so a canvas
   draw that reaches an unexecuted plan output fails with `FLUX_ERROR_INVALID_STATE`
   and a message naming the correct sequence. It is not silently rendered blank
   and it is not silently rendered stale.

Implementation: `libs/flux/src/core/plan.{h,c}` (the plan itself),
`libs/flux/src/core/frame.c` (`flux_frame_materialize_image`,
`flux_frame_plan_stats`, reset and finalize points),
`libs/flux/src/canvas/canvas.c` (boundary flush and the in-pass rejection),
`libs/flux/src/effect/effect.c` (the reusable blur *and* shadow filters register
instead of recording; `flux_blur_filter_current` refuses a pending slot),
`libs/prism/src/*.c` (materials materialize their blurred input).

## Alternatives Considered

- **Leave it to the host: document "only call the effect when you need it".**
  Rejected. It is an exhortation with a silent, per-host failure mode: the
  mistake costs GPU time and nothing else, which is precisely the class of bug
  that survives review. It also has to be re-derived correctly by every future
  host and every Rust binding.

- **A full `RenderPlan` graph compiler with topological sort and virtual
  resource aliasing.** Rejected *for now*, not in principle. ADR-0089 owns that
  destination. Shipping it here would mean a general DAG machinery, a virtual
  resource allocator, and a compilation cache to schedule the three to five
  passes a UI frame actually contains — cost and blast radius out of proportion
  to the defect. This ADR's pass array is the subset a plan compiler later
  subsumes: fixed capacity, declaration order, no allocation.

- **Record eagerly, then strip unwanted dispatches before sealing the command
  buffer.** Rejected. Stripping means rewriting already-recorded command
  streams, and a command buffer has no removal operation — the only sound
  implementation is recording into a scratch buffer and copying the survivors,
  which pays a second copy of every surviving dispatch.

- **Infer liveness from image handles or reference counts.** Rejected for the
  reason ADR-0137 recorded: pooled effect outputs hand back the same handle and
  the same refcount every frame, so neither distinguishes "nothing sampled it"
  from "something sampled it". Explicit require is the only honest signal.

- **Defer execution past the pass boundary and let the consumer's flush pick it
  up.** Rejected. A consumer at a boundary needs the pixels *now*; deferring
  again would require a queue of pending flushes and an ordering proof between
  them. Flushing at require is simpler and strictly ordered.

## Consequences

- **Unauthored GPU work disappears.** An effect nothing samples costs one array
  append and one count at submit. The measured case — the material showcase in
  `Raw Backdrop` — stops computing a full-resolution blur pyramid entirely.

- **The contract is now explicit and checked.** A host that wants a blur
  composed by canvas must materialize it before `flux_canvas_begin`. The failure
  is a named error at the offending call, not a blank texture or a Vulkan
  validation message.

- **`flux_frame_materialize_image` is public API.** It is the documented seam for
  any consumer that composes a deferred output into its own compute, including
  custom host effects. Called on a non-deferred image it is a documented no-op,
  so consumers need no special case.

- **Pruning is observable.** `flux_frame_plan_stats` reports registered,
  executed, and pruned counts per frame, which lets `tests/flux/integration/test_plan_prune.c`
  assert the mechanism instead of trusting it.

- **The plan is per frame slot, not per frame object.** Secondaries come from the
  slot's own command pool, which `begin_frame` has already reset after waiting
  that slot's fence, so their lifetime is exactly the slot's and recycling them
  costs nothing. `flux_plan_reset` is called by `begin_frame`; nothing is
  allocated on the hot path.

- **Effect parameters are copied into a per-frame arena** (`FLUX_PLAN_ARENA_BYTES`,
  48 KiB). A descriptor that does not fit falls back to eager recording, which
  is always correct: pruning is an optimisation, never a correctness dependency.

- **Passes are recorded outside a render pass, permanently.** This was already
  true of compute; it is now a stated contract rather than an incidental one.

### Known gap

`./tools/gate check bench` (the Tracy-driven performance SLA gate) fails in
this environment: `tracy-capture -s 2` never receives a connection and the
guardrail times out. Verified to fail identically on the **unmodified** tree
with the change stashed, so it is a toolchain/environment condition rather than
a regression from this ADR. Every other gate — including clang-tidy — passes.
Recorded here rather than left as a silent red mark: an unexplained failing gate
is how a project learns to ignore its gates.

## Negative Knowledge

- **Never claim a green tree from a gate set that silently omits gates.**
  `./tools/gate check` ran only `QUALITY_GUARDRAILS` while its own `--help`
  advertised `all` as including `tidy` and `bench`. Two gates that were never
  executed were reported as passing by omission. `all` now means all ten, and a
  `--fast` flag names the quick subset explicitly, so "green" cannot be
  ambiguous. `./tools/gate fix` forces the fast subset, because remediation is
  not verification and must not pay clang-tidy before touching a file.
- **Never accept a symbol index that only regenerates a hand-maintained list.**
  The `symbols` guardrail verified that `docs/reference/symbols.md` matched the
  headers *it had been told about*; a header absent from that list was invisible,
  so `flux/sdf.h` (7 symbols) and `flux/curve.h` (20) shipped exported and
  undocumented while the gate reported fresh. The guardrail now scans every
  installed header for the library's API macro and fails on any that is
  unlisted, which is what makes the index complete rather than merely current.
- **Never let a material own a second copy of the plan's dependency bookkeeping.**
  Each Prism material converts its eager recording body into "snapshot the plan,
  register, replay". The snapshot must capture every decision the replay depends
  on — resolved region lists, prepared push constants, barrier choices — because
  recomputing any of them at replay time can disagree with what planning decided,
  and the stages' barriers depend on those decisions. The same rule makes the
  snapshot copy *post*-resolution: an array populated after the `memcpy` is a
  null dereference at replay, which is how the first frosted conversion crashed
  on a null output image.
- **Never let `prism` see the plan's internals.** The arena and the pass array
  stay inside flux; a material hands over a parameter block and a size, and flux
  copies it. That keeps `flux_frame` opaque and stops a material from depending
  on the scheduler's data layout.

- **Never record compute inside a render pass, however convenient the call site
  looks.** Vulkan forbids it, and the rejection added here exists because the
  alternative — accepting the draw and painting an unwritten image — is a
  correctness failure that looks like a material bug.

- **Never let a flush happen *after* the consumer that needed it.** The whole
  ordering guarantee is "flush at the boundary that precedes the sample". A
  deferred flush would restore the original defect in a form that is far harder
  to see, because the pixels would be written *eventually*.

- **Never build a pass without marking its producers required.** Build order and
  *liveness* are two different things, and conflating them pruned a producer
  while executing the consumer that sampled it. Building a pass must propagate
  `required` backwards along `input_producers`. The first implementation walked
  producers for ordering only, which is invisible with a single pass and
  catastrophic with a chain: `test_plan_prune` case 4 fails with a one-line
  deletion if the propagation is removed.

- **Never reuse an arena offset by rounding down to a smaller alignment.** Two
  passes would alias one parameter block, and the corruption would surface as a
  later pass replaying the wrong sigma — a rendering bug with no crash to trace.

- **Never prune on a heuristic.** Identity, recency, and refcounts all fail to
  describe liveness for pooled outputs. A pass is pruned because no consumer
  required it, and for no other reason.

- **Never treat the arena or pass capacity as a correctness bound.** Exhaustion
  falls back to eager recording. A pruning mechanism that could fail to render
  would be worse than no pruning at all.

- **Never let a deferred output look "already computed" through a filter-slot
  flag.** `flux_blur_filter_current` reports a slot's output only when the slot
  is valid *and* its pass has executed; returning a pending image handed the
  caller last frame's pixels under this frame's handle. The flag describes
  ownership, not readiness, and conflating the two is a silent stale-read.

- **Never add a second submission path for deferred work.** Pruned and executed
  passes share one command stream and one submit; a parallel queue would need
  its own synchronisation and would break the ordering argument above.

## References

- [ADR-0089](0089-rendering-program-and-execution-plan.md) — the RenderPlan
  program this mechanism is a subset of.
- [ADR-0091](0091-drawing-group-and-effect-semantics.md) — effects name their
  inputs and outputs explicitly; this ADR gives that declaration a scheduling
  consequence.
- [ADR-0092](0092-resource-planning-and-retirement.md) — resource ownership and
  retirement, unchanged by deferral.
- [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md) — why handles and
  descriptor bytes cannot express liveness.
- [ADR-0144](0144-partial-paint-requires-complete-image.md) — the sibling case of
  making an implicit invariant mechanically enforced instead of documented.
