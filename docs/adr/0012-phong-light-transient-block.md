# ADR-0012: Phong lighting parameters ride a transient buffer-device-address block

- Status: Accepted
- Date: Stage 9

## Context

`FLUX_MATERIAL_PHONG` was removed from the public enum at Stage 7.5 so
no unimplemented value would ship; the lighting pipeline now lands.
Blinn-Phong with one directional light needs, per draw: the world
matrix (fragment-stage world position), a normal matrix
(non-uniform-scale-safe normals), the base color, the light direction
and color, the ambient term, the eye position, and the specular
exponent and strength. That totals 176 bytes — over the 160-byte
`FLUX_DEVICE_REQUIRED_PUSH_BYTES` budget once the 64-byte MVP is
included.

Three ways to feed the data were evaluated:

| Approach | Push-constant cost | Fit |
|---|---|---|
| Everything in push constants | 240+ bytes | Over budget; packing tricks (view-space lighting, packed colors) still land at 176 |
| Per-material uniform buffer + descriptor | 64 bytes | New descriptor machinery; lights are per-draw, not per-material, so the buffer churns every draw anyway |
| Transient ring slice referenced by buffer device address | 72 bytes (MVP + 8-byte address) | Reuses `flux_frame_alloc_transient` and the canvas vertex-pulling precedent |

## Decision

**Per-draw parameter block in the frame's transient ring, referenced by
buffer device address from the push constants.** The phong push
constants are the MVP plus the 64-bit address of a 176-byte std430
block (`scene_phong_params` in `src/scene/scene.c`, mirrored by the
`PhongParams` `buffer_reference` block in `scene_phong.vert/.frag`).
Both shader stages read the same block through the same push range
(`VERTEX | FRAGMENT`).

Light data is per-draw, not per-material: `flux_scene_draw_mesh_lit`
takes an optional `flux_scene_light`; `flux_scene_draw_mesh` lights
PHONG materials with `FLUX_SCENE_LIGHT_DEFAULT`. The material carries
only the static surface properties (`base_color`, `shininess`,
`specular`).

The normal matrix (transpose of the inverted upper-left 3×3 of world)
and the eye position (translation column of the inverted view) are
computed on the CPU per draw — two `flux_mat4_invert` calls, cheap
next to the Vulkan record cost.

## Consequences

- The transient ring becomes a hard dependency of PHONG draws. When the
  ring is exhausted, the draw is dropped and the thread-local error is
  set by `flux_frame_alloc_transient` — the same degradation mode the
  canvas already has.
- Adding lighting features (point lights, shadow parameters) grows the
  transient block, not the push-constant budget. The block layout is
  internal; it can change in any release without ABI consequence.
- The unlit path is untouched: same push layout, same shaders, same
  single-stage push range as before.
- Specular is computed in world space, so the block carries the eye
  position; a view-space formulation would have saved 16 bytes but
  required transforming the light direction on the CPU per draw and
  made the shader depend on the modelview decomposition.

## When to revisit

- Multiple lights per draw: the block should become a header plus a
  variable-length light array in the same transient slice.
- PBR (`FLUX_MATERIAL_PBR`): texture-driven materials will pull
  bindless handles into the block; the addressing scheme is unchanged.

## See also

- ADR-0004 — paint kind drives pipeline selection (the same
  kind-selects-pipeline pattern, canvas-side).
- `docs/reference/symbols.md` — `flux_scene_draw_mesh_lit`,
  `flux_scene_light`.
