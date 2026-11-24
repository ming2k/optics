# How to light a mesh

Draw a mesh with the Blinn-Phong lit material and one directional
light.

## Prerequisites

You have a `optics_gfx_device *d`, a mesh-drawing frame loop like the one in
[Record and present a frame](record-and-present-a-frame.md), and a
`optics_gfx_mesh` whose vertices carry correct normals (`optics_gfx_vertex.normal`).

## Create the material

Set `kind` to `OPTICS_GFX_MATERIAL_PHONG` and pick the surface properties:

    #include <optics/gfx/scene.h>

    optics_gfx_material_desc md = OPTICS_GFX_MATERIAL_DESC_INIT;
    md.kind         = OPTICS_GFX_MATERIAL_PHONG;
    md.base_color   = (optics_gfx_vec4){ 0.8f, 0.6f, 0.3f, 1.0f };
    md.color_format = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(surface));
    md.depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT;
    md.shininess    = 48.0f;   /* specular exponent; <= 0 selects 32 */
    md.specular     = 0.6f;    /* highlight strength; 0 disables it  */

    optics_gfx_material *mat = nullptr;
    if (optics_gfx_material_create(d, &md, &mat) != OPTICS_GFX_OK) {
        /* see optics_gfx_get_last_error */
        return;
    }

`shininess` and `specular` are ignored by `OPTICS_GFX_MATERIAL_UNLIT`.

## Draw with a light

Inside the pass, pass the light in the draw descriptor:

    optics_gfx_scene_light light = OPTICS_GFX_SCENE_LIGHT_DEFAULT;
    light.direction = (optics_gfx_vec3){ -0.6f, -1.0f, -0.4f };  /* travel direction */
    light.color     = (optics_gfx_vec3){ 1.0f, 1.0f, 1.0f };     /* linear RGB */
    light.ambient   = 0.12f;

    optics_gfx_scene_draw_mesh(frame, &(optics_gfx_scene_draw_mesh_opts){
                                          .camera = &cam,
                                          .world = world,
                                          .mesh = mesh,
                                          .material = mat,
                                          .light = &light,
                                      });

`direction` is the direction the light travels (a sunlight vector); it
is normalized internally. The light is consumed during the call — a
stack local is fine, and each draw may use a different light.

Pass `NULL` for the light, or keep calling `optics_gfx_scene_draw_mesh`, to
use `OPTICS_GFX_SCENE_LIGHT_DEFAULT` (white light from above-left, ambient
0.08).

## Verify

Run the cube example with lighting enabled and confirm the faces shade
with the light direction instead of rendering flat:

```bash
./build/examples/showcase/scene_studio
```

Use the **Ambient** slider in the scene inspector to vary the light level and
confirm the faces shade with the directional light instead of rendering flat.

## Pitfalls

- A black mesh usually means zeroed normals. Faceted meshes need
  per-face vertex normals; smooth meshes need per-vertex averaged ones.
- Lighting runs per draw, so a dropped highlight after heavy frames
  can indicate transient-ring exhaustion — check `optics_gfx_get_last_error`
  after the draw call.
- The light values are linear RGB. Convert `optics_gfx_color` values through
  `optics_gfx_color_to_linear` before reusing them as light colors.
