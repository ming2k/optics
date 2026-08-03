# ADR-0050: Single-body liquid-glass focus field

- Status: Accepted
- Date: 2026-08-03

## Context

Liquid-glass consumers need to emphasize one interactive region inside an
existing body. Rendering that region as a second glass body produces a second
silhouette, rim, refraction boundary, and shadow. A painted highlight avoids
the extra dispatch but cannot adapt its clarity to arbitrary transmitted
content and encourages opaque fills or accent outlines in product code.

ADR-0046 defines the material as one analytic convex-lens SDF. ADR-0047 keeps
product selection policy at the caller while Flux owns optical mechanism and
the shader owns the material curves. The liquid-glass push block was already
156 bytes against this project's 160-byte layout; an unbounded new parameter
set or another independent shape path would exceed that budget.

## Decision

Add one optional soft focus field to `flux_liquid_glass_group`. The caller
supplies a rounded-rectangle `focus` inside the primary body's bounds and a
finite `focus_strength`; strength is clamped to `[0, 1]`. The shader uses the
field only to reduce local frost and adaptive tint and add a broad directional
light pool. It does not change coverage, the body SDF, rim, refraction edge,
or shadow and therefore cannot create an inner glass outline.

When `shape_count == 1`, the dispatch packs focus geometry into the existing
secondary `shape1`/`radius1` push fields. One new `focus_strength` float grows
the push block from 156 to exactly 160 bytes. A positive focus and a
smooth-union secondary body are mutually exclusive, because they intentionally
share that storage. Descriptor validation rejects focus on a two-shape group,
invalid focus geometry, and focus bounds outside the primary bounds.

Flux defines the field falloff and relative optical response as material
identity. The caller owns focus geometry, strength, and when focus is active.
The implementation lives in `libs/flux/src/effect/effect.c` and
`libs/flux/src/effect/shaders/effect_liquid_glass.comp`; the public C and Rust
contracts live in `libs/flux/include/flux/effect.h` and
`bindings/flux-rs/crates/flux/src/lib.rs`.

## Alternatives Considered

- **A second analytic glass group.** Rejected because it creates the nested
  body, silhouette, rim, and shadow the focus mechanism must avoid.
- **Painted translucent highlight.** Rejected because it obscures content
  instead of changing transmission and cannot express the same adaptive
  optical hierarchy.
- **A larger independent focus push layout.** Rejected because it would
  exceed the established 160-byte push block and reduce portability.
- **Storage-buffer group records.** Rejected for one mutually exclusive
  shape: it adds allocation, binding, and synchronization machinery without
  increasing current expressive power.

## Consequences

- `flux_liquid_glass_group` grows by one shape and one float, so C and Rust
  consumers must rebuild against the matching header/bindings.
- One group can animate either a smooth union or an interaction focus in a
  frame, not both.
- Focus participates in the existing body dispatch and frame-slot lifecycle;
  no extra image, pass, or material body is allocated.
- Tests must preserve group validation and the 160-byte push-layout assertion.
  `examples/flux/liquid_glass_study` carries a focus case for pixel review.

## References

- [ADR-0046 — Liquid glass as a convex-lens material](0046-liquid-glass-convex-lens-model.md)
- [ADR-0047 — Caller-owned policy boundary for flux effects](0047-caller-owned-policy-boundary-for-flux-effects.md)
