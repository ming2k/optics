---
id: ADR-0139
title: "Footprint-sized material slabs, batched body dispatch, and the prism_material_output contract"
status: accepted
date: 2026-11-02
superseded_by: null
scope: libs/prism, libs/flux, architecture
negative_knowledge: true
---

# ADR-0139: Footprint-sized material slabs, batched body dispatch, and the prism_material_output contract

- Status: Accepted
- Date: 2026-11-02
- Scope: `libs/prism/include/prism/types.h`, `libs/prism/include/prism/*.h`, `libs/prism/src/prism_material_frame.h`, `libs/prism/src/glass_dispatch.h`, `libs/prism/src/{liquid_glass,backdrop_layer,frosted,acrylic,mica}.c`, `libs/prism/src/shaders/{liquid_glass,backdrop_frost}.comp`, `libs/flux/src/core/{device,frame}.c`.
- Implementation: Landed. Breaking API (`feat(api)!`): every material's apply returns `prism_material_output` (image + capture-space placement) instead of a bare `flux_image *`, and the output image is slab-sized. Bodies travel in a host-visible table addressed by device address, and one dispatch covers one coalesced region. Verified by the prism integration suites (golden-referenced pixels unchanged for single-body scenes), `tests/prism/unit/test_liquid_glass_regions.c`, and measured in the numbers below.
- Depends on: [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0065](0065-per-group-overrides-and-backdrop-stats.md), [ADR-0079](0079-layered-backdrop-material.md), [ADR-0069](0069-color-management.md).
- Supersedes: the "output extent == input extent" clause of [ADR-0046](0046-liquid-glass-convex-lens-model.md) and [ADR-0079](0079-layered-backdrop-material.md); the one-dispatch-per-body recording in `glass_dispatch.h`; the per-body ordering barrier described in [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md).

## Context

Three problems shared one root cause: a prism material allocated and dispatched as if it were a full-screen effect.

**1. Resident GPU memory scaled with the capture, not with the material.** Every frame slot owned a capture-sized output image. At 4K that is 3840 x 2160 x 8 bytes = 64 MiB per slot, three slots per filter: 192 MiB for liquid glass, 384 MiB for the layered backdrop. A 48-px glass titlebar — the single most common use of the material — paid that for 0.7 MiB of actual coverage. A window with a dock, a sidebar and a panel measured a union bounding box of 100% of the screen, so "allocate the footprint bounding box" saves nothing for real chrome; only per-region allocation does.

**2. Extent churn re-allocated every slot.** Because the slot was keyed on the exact capture extent, a resize that moved a few pixels per frame released and re-created every slot image every frame. Measured: a 1-px-per-frame sweep ran 1.7x slower than steady state from the allocation churn alone.

**3. Cost scaled linearly with body count.** Every body was one compute dispatch, and every dispatch was preceded by a *whole-image* storage barrier because bodies composite source-over into one persistent image. Vulkan has no sub-rectangle image barrier, so ordering N bodies cost N-1 full-image barriers even when the bodies were mutually disjoint — a toolbar of sixteen chips paid fifteen barriers for dependencies that did not exist. The push-constant block was also full (exactly 160 bytes, the flux-guaranteed budget, exhausted since ADR-0050), so there was no room to describe a second body, let alone N.

## Decision

**A material's output is sized to its footprints, and the caller is told where it landed.**

1. Output images are slabs: the union of the submission's footprints, quantised up a `PRISM_SLAB_QUANTUM` (128 px) ladder with a `PRISM_SLAB_MIN_SIDE` (256 px) floor. The ladder is what makes a resize sweep reuse the same image instead of churning one per step.
2. `prism_material_output` carries `image`, `origin_x`, `origin_y`, `width`, `height`. Every material's apply returns it. The capture-space placement is the contract: capture = output + origin. `libs/prism/src/prism_material_frame.h` owns the geometry.
3. Legacy propagation inside the library is removed, not deprecated: `_ex` tiers and "also return the old thing" are forbidden by the API standards, and an output that claims capture extent while holding a fraction of it is exactly what caused the over-allocation. Callers draw at the placement; the slab is transparent outside the footprints, so drawing the whole slab is always correct.

**Per-body state leaves the push block and the barriers leave the dispatch loop.**

4. A material's per-body parameters travel in a host-visible, device-addressed table with a fixed `PRISM_BODY_STRIDE` (256 bytes). A fixed stride means adding a field never moves an existing row, and both sides static-assert the stride so the shader's array stride and the host's row stride cannot drift. Inheritance and clamping are resolved on the host, so the shader carries no per-pixel override logic.
5. Footprints are coalesced into disjoint regions (the transitive merge the clear pass already used), and each region's bodies are recorded once, in caller order, into an id list. One dispatch evaluates one region against its own id list. Bodies are **batched, not broadcast**: a region's shader work is proportional to the bodies that touch it, so there is no N-way scan.
6. The per-body ordering barrier is deleted, not tuned. Coalescing makes it unnecessary: two bodies that share a pixel necessarily land in the same region, where their source-over order is the shader's loop order; disjoint regions never write each other's pixels, so the Vulkan memory model imposes no hazard. The proof obligation is `liquid_glass_region_overlaps_any`, unit-tested exactly rather than through pixels, because a missing barrier is undefined behaviour that current GPUs happen to serialize away.
7. `flux_device_wait_idle` becomes a reliable reclamation point. A retire tag no longer unconditionally adds one to the submission serial: the `+1` only exists to cover a frame that is *still recording*, so it is applied only when the device has one (`recording_frames`, maintained at the record/submit transitions). Before this, a host that released its last material and waited for idle kept the memory until it submitted an otherwise pointless frame, which quietly made leak assertions on live counters depend on an extra submission.

## Alternatives Considered

- **Allocate the footprint bounding box, keep the extent contract.** Measured against real chrome: a dock + sidebar + panel union to 100% of a 4K screen (0% saved), four corner HUD panels to 94% (6% saved). It only helps single-centre-dialog layouts. Rejected as the primary mechanism; the ladder subsumes it.
- **Keep the capture-sized output and cache aggressively.** The cache (ADR-0137) removes the *dispatch* cost but not the *residency*: a cached 64 MiB image is still 64 MiB. The two mechanisms solve different halves and both are needed.
- **Atlas many materials into one shared slab.** Rejected: it couples materials that currently have no relationship, and a shared image cannot express "this material's slab changed" without invalidating the others. Per-material slabs share the ladder and the geometry, not an allocation.
- **Deprecate the old `flux_image **` signature with a `_place` companion.** Rejected by the API standards (`docs/dev/standards/api-design.md` forbids dual tiers and floating modifiers), and it would preserve the broken mental model — that the output is the size of the input — which is what the change exists to correct.
- **Keep per-body dispatch but elide disjoint barriers** (the state before this ADR). Helped disjoint layouts only, left the push block full, and still paid one dispatch per body. Batching removes the dispatch as well as the barrier and gives the next material iteration somewhere to put its parameters.
- **Broadcast: one dispatch over all footprints, shader loops every body.** One dispatch total, but every pixel scans all N bodies (~64 bounds tests per pixel for a chrome strip). Chosen against: batching keeps the dispatch count equal to the *coalesced region* count, which is 1 for a stacked card stack and 1-3 for typical chrome.

## Consequences

- **Measured, 3 slots, 4K capture, titlebar-shaped footprint:** liquid glass 96 → 11 MiB (RGBA8), 192 → 22 MiB (RGBA16F), i.e. 8.7x at 16F. Layered backdrop 192 → 107 MiB, 384 → 214 MiB (1.8x; its base stage must resolve a true background for the whole slab because the lens taps outside its own silhouette — that requirement is documented at the stage).
- **Measured, 1920x1080 capture:** 16 disjoint chips 0.34 ms/frame, 64 disjoint chips 0.62 ms, 16 fully stacked bodies 0.21 ms, 64 fully stacked 0.46 ms. Cost now tracks covered area and coalesced regions, not body count, and the stacked case — where every body overlaps and the barrier is mandatory — is the cheapest because it is one dispatch.
- **The output contract changed for every caller.** Drawing the returned image edge-to-edge is now wrong; the placement must be honoured. Library consumers (examples, Rust bindings, tests) are migrated in the same change; there is no compatibility path.
- **Pipeline count is still per filter**, but that was re-evaluated and is not a defect: `flux_compute_pipeline_create` uses the device pipeline cache, so only the first filter per shader pays compilation (Intel ARL: 26.3 ms first, 0.54-1.19 ms after). Recorded in ADR-0137's consequences.
- **`prism_material_frame.h` is now the single place slab geometry, the body table, and the region batch are defined.** A material that hand-rolls its own extent logic is the drift this header exists to prevent.
- **Remaining asymmetry:** frosted, acrylic and mica still allocate capture-sized slots and return a zero origin. Their dispatch is already per-body-parameter-batched where it matters, but their slab migration is outstanding; the uniform `prism_material_output` return is what keeps that a per-material change rather than an API change.

## References

- [ADR-0063](0063-liquid-glass-material-library.md) — why the material, not flux, owns its layout decisions, including the 160-byte push exhaustion this ADR works around.
- [ADR-0065](0065-per-group-overrides-and-backdrop-stats.md) — the per-group override model the body table now expresses.
- [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md) — the output cache whose key this ADR does not change; the slab extent participates through `cached_revision`.
- Vulkan 1.3 specification, "Memory Dependencies" — non-conflicting accesses between dispatches, and the fact that a pipeline barrier covers a whole subresource.
- Prior art: Core Animation's `contentsScale`/layer bounds invalidation and Chromium's `cc::TileManager` tile ladder, both of which size GPU surfaces to content and quantise to a ladder.
