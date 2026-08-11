# ADR-0047: Caller-owned policy boundary for flux effects

- Status: Superseded by ADR-0063
- Date: 2026-08-01

## Context

ADR-0008 chartered the effect module as the home for image-domain
operators (blur, shadows, glass panels, grading). The liquid-glass
effect (ADR-0046) grew quickly, and several rounds of review surfaced
the same failure mode: policy leaking into the operator. Concrete
instances: a fixed 72 px body-size reference and a fixed scaling floor
baked into the shader, a single dispatch-wide shadow configuration that
could not express "a dock bar casts a deeper shadow than a status
chip", and a hard-coded neutral tint pair that prevented accent-tinted
glass.

A generic foundation must not embed decisions that belong to one
product surface, or every consumer inherits them. It must also not
explode into hundreds of knobs that force every consumer to re-derive
the same material.

## Decision

The boundary for flux effects is: **flux owns mechanism, the caller
owns policy, the shader owns the material identity.**

- **Mechanism (flux):** the image-domain operator itself — SDF
  geometry, lensing, dispersion, scattering, adaptive tint response,
  rim lighting, shadows, smooth-union merges, dispatch and frame-slot
  plumbing. None of it may reference a product concept (no components,
  no shells, no protocols).
- **Policy (caller):** every knob a caller can reasonably want to
  differ by use case — geometry strengths, lighting strengths, tone,
  shadow, size-scaling policy, tint color. Descriptor-level parameters
  cover the dispatch-wide look; group-level parameters cover per-body
  optical character (shadow, tint color, opacity, merge behavior).
  Values are used verbatim; the effect never second-guesses them.
- **Material identity (shader):** the curve shapes and relative layer
  weights that make the material read as *this* material — the lens
  profile, the luminance response curve, the highlight falloffs. These
  are constants on purpose: exposing them would not generalize the
  library, it would force every caller to re-derive the same look.

Review rule: a change that makes the effect aware of *where* or *why*
it is used is a boundary violation; a change that lets the caller
decide *how strong* an existing layer is belongs in the descriptor or
the group.

## Alternatives

- **Everything parameterizable.** Rejected: a shader reduced to a
  parameter interpreter loses the material's coherence, and every
  caller pays the cost of re-tuning identity curves.
- **Product presets inside flux (dock preset, chip preset).** Rejected:
  presets are where product knowledge enters the library; per-body
  parameters already express them at the call site.
- **Splitting the glass effect out of flux into a consumer.** Rejected:
  the operator needs bindless images, compute pipelines, and frame-slot
  barriers; re-plumbing those above flux is worse coupling than a
  parameter set.

## Consequences

- Shadow configuration moved from descriptor to group; tint color is a
  new group-level knob. Existing neutral behavior is the zero/white
  default for both, so uninterested callers see no change.
- Push-constant budget stands at 156 of 160 bytes; further per-group
  floats require packing or a second constant path.
- The aegis compositor configures per-component values (dock vs chip
  vs collapsed handle) at the call site; flux stays product-neutral.
