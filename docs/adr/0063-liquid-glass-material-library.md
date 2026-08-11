# ADR-0063: Liquid glass moves to the prism material library

- Status: Accepted
- Date: 2026-08-11

## Context

The liquid-glass material (ADR-0046, focus field ADR-0050) shipped inside
the flux effect module under the ADR-0047 boundary: flux owns mechanism,
the caller owns policy, the shader owns material identity. That boundary
turned out to be discipline, not structure — the material still lived in
flux's tree, its public header, and its ABI, and three symptoms showed it
had outgrown that home:

- **The push-constant budget is exhausted.** ADR-0050 grew the push block
  from 156 to exactly 160 bytes, and the focus field fits only because it
  shares the smooth-union shape slot. There is no room for the next
  material iteration inside the current layout.
- **Every design iteration churns flux's public header and ABI.** A knob
  rename or a new descriptor field forces every flux consumer to rebuild,
  even though the material is one consumer's look, not a rendering
  primitive.
- **`glare` drifted semantically.** ADR-0046 narrowed it from a broad
  additive halo to a scale over the rim-lighting set; the name in the
  public header no longer says what the knob does.

Industry practice places materials above the rendering-primitive layer:
Apple's material lives in SwiftUI/UIKit, not Metal; WinUI's Acrylic sits
above the DWM, not inside it.

ADR-0047 explicitly rejected "splitting the glass effect out of flux into
a consumer", arguing that the operator needs bindless images, compute
pipelines, and frame-slot barriers, and that re-plumbing those above flux
is worse coupling than a parameter set. This ADR adopts that rejected
alternative, because the calculus changed: flux grew the public seams.
Compute pipelines were already public; the missing pieces were
compute-writable images, frame-slot queries, and bindless storage
accessors, and those are now promoted. The plumbing above flux is no
longer a re-plumbing — it is a handful of small public accessors.

## Decision

1. **New sibling library `libs/prism` — the material library of the
   optics stack.** A named material — its shader, its curve shapes, and
   its parameter contract — lives here, built purely on flux's public
   API. Public headers `<prism/prism.h>` (umbrella) and
   `<prism/liquid_glass.h>`; meson option `-Dprism` (default `true`,
   requires `-Dcompute=true`); pkg-config name `prism`, requiring `flux`.
2. **The liquid-glass material moves from flux to prism with a 1:1
   `prism_`-prefixed API:** `prism_liquid_glass_filter_create/retain/
   release/apply`, `prism_liquid_glass_shape/group/desc`,
   `PRISM_LIQUID_GLASS_GROUP_INIT`, `PRISM_LIQUID_GLASS_DESC_INIT`, and a
   prism-local `prism_struct_type` registry with
   `PRISM_TYPE_LIQUID_GLASS_DESC = 1`. The implementation lives in
   `libs/prism/src/liquid_glass.c` and `libs/prism/src/regions.h`, with
   the shaders at `libs/prism/src/shaders/liquid_glass.comp` and
   `libs/prism/src/shaders/storage_clear.comp` (moved from
   `libs/flux/src/effect/`).
3. **flux promotes the mechanism seams an external material library
   needs:** `flux_image_create_compute_writable` (core.h — a
   STORAGE|SAMPLED image in GENERAL layout, registered into both bindless
   slots, RGBA8/BGRA8 only, weak device reference),
   `flux_image_bindless_storage_handle` and
   `flux_device_default_sampler_handle` (vulkan.h),
   `flux_frame_get_state` / `flux_frame_has_active_pass` /
   `flux_frame_device` plus the now-public `flux_frame_state` enum
   (core.h), `flux_image_device` (core.h), and `FLUX_MAX_FRAMES_IN_FLIGHT`
   (core.h).
4. **The flux symbols are removed:** `flux_liquid_glass_shape/group/desc/
   filter`, `flux_liquid_glass_filter_create/retain/release/apply`,
   `FLUX_LIQUID_GLASS_GROUP_INIT`, and `FLUX_LIQUID_GLASS_DESC_INIT`.
   `FLUX_TYPE_LIQUID_GLASS_DESC = 20` is retired and never reused (see the
   comment in `libs/flux/include/flux/core.h`).
5. **The descriptor knob `glare` is renamed to `rim_light`** (same
   default, 0.55). It scales the entire rim-lighting set — key line,
   sheen, fresnel, shadow side, trough — one knob for overall rim energy.
   `light_direction` and every other parameter are unchanged.
6. **The boundary is now structural:** prism owns the material identity —
   the shader, the regions policy, and the dispatch machinery; flux owns
   the runtime; the caller owns policy. ADR-0047's caller-owned-policy
   principle survives, enforced by the split itself: flux cannot see the
   material, and prism cannot see product concepts. The material model is
   unchanged — ADR-0046 (convex-lens material) and ADR-0050 (focus field)
   remain in force; only the material's home moves.

## Alternatives Considered

- **Keep the material in flux under ADR-0047 discipline.** The previous
  stance, now abandoned. The discipline held — policy never leaked into
  the operator — but the structure did not: the push-constant budget is
  full, each look iteration churns flux's ABI, and parameter names drift
  inside a header that promises stability. The 0047 argument against
  splitting ("re-plumbing bindless images, compute pipelines, and
  frame-slot barriers above flux is worse coupling") no longer holds once
  those exact seams are small public accessors; the coupling the split
  was meant to avoid is now the coupling the split removes.
- **Everything-parameterizable shader.** Still rejected, for the same
  reason as ADR-0047: a shader reduced to a parameter interpreter loses
  the material's coherence, and every caller pays the cost of re-tuning
  identity curves. prism keeps the identity curves as constants — the
  move changes who owns them, not what they are.
- **Fold the material into lens.** Rejected: lens is a headless
  immediate-mode UI engine that owns no GPU passes, and the compositor
  consumer sits below lens. A material that requires a UI toolkit could
  not serve the layer that needs it most.

## Consequences

- **Breaking change.** `flux_liquid_glass_*` and its init macros are gone
  from `<flux/effect.h>`; consumers migrate to `<prism/liquid_glass.h>`
  and link `prism`. `glare` becomes `rim_light` in the same move.
  `FLUX_TYPE_LIQUID_GLASS_DESC` (20) is retired permanently.
- The Rust flux crate loses its glass wrapper; a new `prism-rs` workspace
  provides it.
- Material iteration no longer touches flux's public header or ABI;
  prism versions the material contract on its own.
- prism's push-constant budget is still 160 bytes, because flux devices
  guarantee that minimum — the focus/merge slot-sharing constraint from
  ADR-0050 carries over unchanged.
- Tests and examples move with the material: the regions unit test is now
  `tests/prism/unit/test_liquid_glass_regions.c`, and the showcase demos
  `liquid_glass` / `liquid_glass_study` link prism and remain the
  pixel-review gate for liquid-glass shader changes.
- flux's effect module keeps the transient operators, the reusable blur,
  and the capture seam; ADR-0008 and ADR-0017 are unaffected. ADR-0047 is
  superseded by this ADR.

## References

- [ADR-0008 — Image-effect pipeline as the home for blur and friends](0008-image-effect-pipeline.md)
- [ADR-0017 — Canvas render-target capture for real backdrop effects](0017-canvas-render-target-capture.md)
- [ADR-0046 — Liquid glass as a convex-lens material](0046-liquid-glass-convex-lens-model.md)
- [ADR-0047 — Caller-owned policy boundary for flux effects](0047-caller-owned-policy-boundary-for-flux-effects.md)
- [ADR-0050 — Single-body liquid-glass focus field](0050-single-body-liquid-glass-focus-field.md)
