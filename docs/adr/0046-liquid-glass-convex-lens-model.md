# ADR-0046: Liquid glass as a convex-lens material

- Status: Accepted
- Date: 2026-08-01

## Context

ADR-0008 introduced the flux effect module, and the analytic liquid-glass
compositor later shipped on it with a uniform-bevel rim model: a wide
additive highlight band across the whole rim, fresnel peaking exactly at
the silhouette, and a constant frost mix over the body. In practice
every glass body carried a milky halo, the center read muddy, and the
look diverged from the design target.

The design target is Apple's Liquid Glass (WWDC 2025): a thin convex
lens. Its interior is optically flat so content passes through
undisturbed; its rim is a curved band that bends light; its body tint
opposes the backdrop's luminance; its highlight is a thin directional
line at the silhouette, not a wash.

## Decision

Model the material as a convex lens in `effect_liquid_glass.comp`.

Geometry stays the shared rounded-rectangle SDF (with optional
smooth-union of two bodies). A rim parameter runs from 0 at the inner
edge of the `edge_width` band to 1 at the silhouette, and a clamped
spherical-cap slope turns it into a bend profile. The sharp backdrop
sample displaces inward along the SDF normal by `refraction` times that
profile — magnifying — with red and blue bent by
`refraction ± chromatic_aberration`. The interior sits at exactly zero
bend.

The body composes: a small frost mix growing through the rim band; a
vibrancy saturation step; a luminance-opposed tint (pearl over dark
content, smoke over bright) weakest in the rim band; and a lighting set
made of a ~2 px key line toward the light, a direction-weighted sheen, a
shadow-side dark line, and a faint trough inside the bottom rim. A
sub-LSB hash dither defeats banding in the `rgba8` output.

`light_direction` is defined as the direction toward the light source.
The default (-0.45, -0.89) puts the key light up-left, so highlights
read at the top of bodies. This corrects the previous effective
behavior, which lit the bottom-right.

The push-constant layout, public descriptor fields, and filter API are
unchanged; only the interpretation of the same parameters changes.

## Alternatives

- **Uniform bevel rim (previous model).** Rejected: additive light
  across a wide band is the halo; a uniform ring cannot read as
  directional environment light.
- **Normal-map / texture-based lens.** Rejected: a texture per shape
  regresses the analytic guarantee that coverage, refraction, and
  lighting agree at corners, and adds a rasterization seam for
  smooth-union merges.
- **Mip-chain refraction.** Rejected: sampling prefiltered mips for the
  lens would soften exactly the high-frequency content the lens is meant
  to magnify.

## Consequences

- Visual output changes under an unchanged ABI. Callers keep their
  parameter values; they read as physical pixels of the capture with
  full precision now.
- The old broad-`glare` look is gone; `glare` scales the key line,
  sheen, and shadow side together, so callers tune one knob for overall
  rim energy.
- `examples/flux/liquid_glass_study` renders the material headlessly
  over a hostile backdrop for pixel-level review; run it after every
  shader change.
- ccache does not track `#embed` inputs. After editing the shader,
  rebuild with `CCACHE_DISABLE=1` or a cleared cache, or the library
  keeps the previous SPIR-V.
