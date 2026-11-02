---
id: ADR-0137
title: "Caller-declared content revisions for prism backdrop-layer output caching"
status: accepted
date: 2026-11-02
superseded_by: null
scope: libs/prism, architecture
negative_knowledge: true
---

# ADR-0137: Caller-declared content revisions for prism backdrop-layer output caching

- Status: Accepted
- Date: 2026-11-02
- Scope: `libs/prism/include/prism/backdrop_layer.h`, `libs/prism/include/prism/types.h`, `libs/prism/src/backdrop_layer.c`, `libs/prism/src/regions.h`, `libs/prism/src/shaders/backdrop_frost.comp`.
- Implementation: Landed. `prism_backdrop_layer_revision` (`PRISM_TYPE_BACKDROP_LAYER_REVISION`) is a `next`-chain extension carrying a `uint64_t` content id; `layer_resolve_revision` rejects an unknown chain tag rather than ignoring it; `backdrop_layer_slot::cached_revision` replaces the former byte-hash/handle key. Verified by `tests/prism/integration/test_backdrop_layer.c` (cache hit, revision bump publishes new content, unknown tag rejected).
- Depends on: [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md).
- Supersedes: the caching decision described in ADR-0123's `[INV-COMP-04]` implementation note (the invariant stands; its mechanism changes).

## Context

ADR-0123 established `[INV-COMP-04]`: optical materials must sample from an explicit, isolated offscreen layer cache, and an un-invalidated backdrop must never be re-filtered. `prism_backdrop_layer_filter` implements that by keeping one persistent output image per frame slot and skipping every compute pass when the submission looks unchanged.

The original gate was `(FNV-1a hash of the descriptor bytes, input bindless handle, blurred bindless handle)`. Both halves of that key are unsound:

1. **Handles do not track content.** `input` is normally one long-lived capture render target, so its handle is constant for the life of the window. `blurred_input` is a `flux_blur_filter` pool output, and the pool hands the *same* image back every frame. A repainted backdrop therefore hashes and compares identically to the previous frame. Measured on Intel ARL and on llvmpipe: flipping the captured backdrop from black to white with an identical descriptor and identical handles left the glass body frozen at the first frame's colour, indefinitely.
2. **Byte-hashing a struct with pointers is neither stable nor meaningful.** `prism_liquid_glass_group` carries `const prism_liquid_glass_shape *shapes`. Hashing its bytes hashes the pointer. Mutating geometry in place (stable pointer) never invalidated, so an animating body froze; rebuilding the array each frame changed the pointer, so the cache never hit and the hash was pure overhead. `liquid_glass.h` documents the stack-array construction idiom, which lands in the freeze case.

The cache was thus wrong in one direction and useless in the other, with no configuration where it worked. Because the failure mode is "plausible but stale pixels" rather than a crash, no existing test caught it: the only cache assertion in the suite pinned that a hit returns the same image, i.e. it encoded the bug.

A cache cannot be made correct by inspecting `prism`'s own inputs. Whether a backdrop changed is knowledge that exists only in the caller: the apparatus that repainted the capture, re-ran the blur, moved a plate, or changed a material parameter.

## Decision

The output cache is keyed on a **caller-declared content revision**, not on image handles or descriptor bytes.

1. `prism_backdrop_layer_revision` is a descriptor extension chained through `prism_backdrop_layer_desc::next`, carrying a single `uint64_t revision`. The caller bumps it whenever anything the material samples or draws has changed.
2. `revision == 0` (and the absence of the extension) disables caching. Every call re-renders. This is the default, so a caller that has not reasoned about invalidation cannot get stale pixels.
3. An unknown extension tag is `FLUX_ERROR_INVALID_ARGUMENT`, not silently ignored. A revision prism cannot read would otherwise downgrade a cache-enabled call to a cache-disabled one without telling anyone.
4. Byte-hashing of descriptor structs is removed entirely. Nothing in prism infers content identity from pointers.

Two defects found in the same audit are fixed alongside, because they share the layer's composition contract:

- **Base-material plates now compose instead of overwriting.** `backdrop_frost.comp` previously wrote an opaque pixel for every dispatched pixel regardless of coverage, so the last plate in the array painted the sharp backdrop over every earlier plate inside the shared dispatch region: two overlapping plates could not coexist. The pass is split into a `BASE` stage (resolve the opaque background once per region) and a `PLATE` stage (source-over one plate inside its own coverage). Plate order in the caller's array is now the composite order, per pixel.
- **Clear-region capacity no longer rejects legal descriptors.** `liquid_glass_build_clear_regions` treated the caller's dispatch capacity as a limit on how many footprints a material may carry, returning `FLUX_ERROR_INVALID_STATE` for a 16-plate + 49-body layer — a descriptor well inside the documented maxima — and then failing on every subsequent frame, because `slot->previous` kept the state that triggered it. Capacity now bounds only the number of rectangles that get dispatched: an uncoalescable footprint set degrades to one full-image clear, which is a superset of every footprint and therefore still correct. Malformed geometry (a footprint outside the image) remains a hard error.

## Alternatives Considered

- **Keep the handle comparison and document it.** Rejected: it is wrong for the documented usage. Nothing in the API forces the caller to re-create the capture target per frame, and the recommended pattern uses exactly one long-lived target plus a pooled blur output.
- **Hash the descriptor field by field, dereferencing `shapes`.** Fixes the pointer instability but not the handle problem. Content identity of a sampled image is still unknowable from the descriptor, and the per-frame cost is a walk over up to 64 groups plus their shape arrays — measurable overhead to answer a question the caller can answer for free.
- **Sample the content to detect change (checksum a few texels).** Rejected: reads the GPU back on the render thread, needs a stall or a frame of latency, and can silently miss change in unsampled regions. It converts a cheap unambiguous answer into an expensive probabilistic one.
- **Invalidate whenever the capture pass ran.** Closer, but still guesswork: a pass may run and paint identical content, and the layer also depends on the blur and on caller parameters. It would also require prism to observe flux's pass stream, which cuts across the module boundary ADR-0063 draws.
- **Drop the cache entirely.** Defensible, but `[INV-COMP-04]` is a real win for static desktop chrome, which is the common case for a dock, sidebar, or window frame. The revision extension keeps that win and makes the contract explicit rather than implicit.
- **Use `flux_frame_index` or a global frame counter as the revision automatically.** Rejected: that is "never cache", since the counter advances every frame regardless of content.

## Consequences

- **The caller owns invalidation.** A constant revision across a repaint freezes stale content, and prism cannot detect it. This is the deliberate trade: an interface that is honest about what it cannot know, over one that guesses and is silently wrong. The header states the obligation at the type, at the descriptor, and at the entry point.
- **The default is the safe one.** No extension means no cache. Adopting the cache is an explicit, reviewable act at the call site.
- **`prism_struct_type` gains `PRISM_TYPE_BACKDROP_LAYER_REVISION = 6`** (append-only, per `libs/prism/include/prism/types.h`).
- **Plate composition semantics are now per-pixel array order.** Any caller that relied on the old "last plate wins over the whole dispatch region" behaviour was relying on a defect; the single-plate and non-overlapping cases are pixel-identical to before.
- **Barriers are elided only where proven unnecessary.** `liquid_glass_region_overlaps_any` is the proof obligation for dropping a whole-image ordering barrier between two material dispatches. It is unit-tested exactly (`tests/prism/unit/test_liquid_glass_regions.c`) rather than through pixels, because a missing barrier is undefined behaviour that current GPUs happen to serialize away — a pixel test cannot fail on it.
- **Pipeline construction is a one-time cost, not a resident cost.** Each filter builds its own compute pipelines on first apply, but `flux_compute_pipeline_create` passes `flux_device_vk_pipeline_cache`, so the second and every later filter with the same shader is a cache hit. Measured on Intel ARL: filter 0 costs 26.3 ms (SPIR-V to ISA compile), filters 1–7 cost 0.54–1.19 ms. Only llvmpipe stays at ~12 ms per filter, which is that driver's per-module shader JIT, not a property of this design. Per-device pipeline hoisting was evaluated and rejected: it would buy a sub-millisecond saving on one library while requiring a shared-ownership seam for device-bound state that prism does not have, and it cannot help the software backend it was proposed for.
- **Resident GPU memory is full-extent per slot, which is the next architectural problem.** Each slot owns a capture-sized output image (3 slots at 4K RGBA16F = 192 MiB for liquid glass, 384 MiB for the layer filter), even though the material is fully transparent outside its footprints. The same over-allocation is why any extent change reallocates every slot and measurably slows a resize sweep. Both symptoms share one root cause and are fixed together by allocating region-sized, bucket-quantised slot outputs; that is tracked as its own decision because it changes the output contract (the returned image stops being capture-sized, so the caller needs its placement).

## References

- [ADR-0123](0123-layered-compositing-subsurfaces-and-damage-tracking.md) — layered compositing and `[INV-COMP-04]`.
- [ADR-0063](0063-liquid-glass-material-library.md) — flux owns mechanism, prism owns material identity, the caller owns policy.
- [ADR-0118](0118-coverage-strategy-placement-distance-fields-and-curve-kernel.md) — why `flux_sdf.glsl` is the single field definition the plate pass adapts rather than restates.
- Vulkan 1.3 specification, "Memory Dependencies" — non-conflicting accesses between dispatches; image barriers cover the whole subresource.
- Prior art: Chromium's `cc::DamageTracker` and Apple's `CALayer.contentsScale` invalidation both require the producer to declare change rather than infer it.
