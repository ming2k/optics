# How to light a mesh

Draw a mesh with the Blinn-Phong lit material and one directional
light.

## Prerequisites

You have a `flux_device *d`, a mesh-drawing frame loop like the one in
[Record and present a frame](record-and-present-a-frame.md), and a
`flux_mesh` whose vertices carry correct normals (`flux_vertex.normal`).
The build must have `-Dscene=true` (default).

## Create the material

Set `kind` to `FLUX_MATERIAL_PHONG` and pick the surface properties:

    #include <flux/scene.h>

    flux_material_desc md = FLUX_MATERIAL_DESC_INIT;
    md.kind         = FLUX_MATERIAL_PHONG;
    md.base_color   = (flux_vec4){ 0.8f, 0.6f, 0.3f, 1.0f };
    md.color_format = flux_format_from_vk(flux_surface_vk_format(surface));
    md.depth_format = FLUX_FORMAT_D32_SFLOAT;
    md.shininess    = 48.0f;   /* specular exponent; <= 0 selects 32 */
    md.specular     = 0.6f;    /* highlight strength; 0 disables it  */

    flux_material *mat = nullptr;
    if (flux_material_create(d, &md, &mat) != FLUX_OK) {
        /* see flux_get_last_error */
        return;
    }

`shininess` and `specular` are ignored by `FLUX_MATERIAL_UNLIT`.

## Draw with a light

Inside the pass, draw with `flux_scene_draw_mesh_lit`:

    flux_scene_light light = FLUX_SCENE_LIGHT_DEFAULT;
    light.direction = (flux_vec3){ -0.6f, -1.0f, -0.4f };  /* travel direction */
    light.color     = (flux_vec3){ 1.0f, 1.0f, 1.0f };     /* linear RGB */
    light.ambient   = 0.12f;

    flux_scene_draw_mesh_lit(frame, &cam, world, mesh, mat, &light);

`direction` is the direction the light travels (a sunlight vector); it
is normalized internally. The light is consumed during the call — a
stack local is fine, and each draw may use a different light.

Pass `NULL` for the light, or keep calling `flux_scene_draw_mesh`, to
use `FLUX_SCENE_LIGHT_DEFAULT` (white light from above-left, ambient
0.08).

## Verify

Run the cube example with lighting enabled and confirm the faces shade
with the light direction instead of rendering flat:

```bash
./build/examples/flux/scene_cube --phong
```

## Pitfalls

- A black mesh usually means zeroed normals. Faceted meshes need
  per-face vertex normals; smooth meshes need per-vertex averaged ones.
- Lighting runs per draw, so a dropped highlight after heavy frames
  can indicate transient-ring exhaustion — check `flux_get_last_error`
  after the draw call.
- The light values are linear RGB. Convert `flux_color` values through
  `flux_color_to_linear` before reusing them as light colors.
