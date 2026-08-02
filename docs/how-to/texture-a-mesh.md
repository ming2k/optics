# How to Texture a Mesh

Create the image and sampler before the material, then chain a
`flux_material_surface_desc` through `flux_material_desc.next`. The material
retains both resources.

```c
flux_image_desc image_desc = FLUX_IMAGE_DESC_INIT;
image_desc.width = width;
image_desc.height = height;
image_desc.format = FLUX_FORMAT_RGBA8_SRGB;
image_desc.initial_data = rgba_pixels;

flux_image *image = nullptr;
if (flux_image_create(device, &image_desc, &image) != FLUX_OK)
    return false;

flux_sampler_desc sampler_desc = FLUX_SAMPLER_DESC_INIT;
sampler_desc.min_filter = FLUX_FILTER_LINEAR;
sampler_desc.mag_filter = FLUX_FILTER_LINEAR;
sampler_desc.address_u = FLUX_ADDRESS_REPEAT;
sampler_desc.address_v = FLUX_ADDRESS_REPEAT;

flux_sampler *sampler = nullptr;
if (flux_sampler_create(device, &sampler_desc, &sampler) != FLUX_OK)
    return false;
```

Set the surface state and create the material with the exact formats attached
to the render pass:

```c
flux_material_surface_desc surface = FLUX_MATERIAL_SURFACE_DESC_INIT;
surface.base_color_image = image;
surface.base_color_sampler = sampler;
surface.alpha_mode = FLUX_MATERIAL_ALPHA_MASK;
surface.alpha_cutoff = 0.5f;
surface.double_sided = true;

flux_material_desc material_desc = FLUX_MATERIAL_DESC_INIT;
material_desc.next = &surface;
material_desc.kind = FLUX_MATERIAL_UNLIT;
material_desc.base_color = (flux_vec4){1, 1, 1, 1};
material_desc.color_format = color_format;
material_desc.depth_format = depth_format;

flux_material *material = nullptr;
if (flux_material_create(device, &material_desc, &material) != FLUX_OK)
    return false;

flux_sampler_release(sampler);
flux_image_release(image);
```

Use `FLUX_FORMAT_RGBA8_SRGB` for color-authored textures. The shader samples
them into linear space before multiplying `base_color`. UV transforms apply
scale, counter-clockwise rotation, then offset. OPAQUE ignores sampled alpha;
MASK discards below `alpha_cutoff`; BLEND uses source-over blending and does
not write depth. Release the material only after every consuming frame has
finished.

For glTF or VRM content, prefer Rust
`Scene::from_glb_with_materials` and `Scene::draw_materials`; the content
layer maps embedded base-color images, samplers, UV transforms, unlit state,
alpha behavior, and primitive material indices automatically.

See [ADR-0048](../adr/0048-textured-surface-materials-and-core-images.md) for
the engine/content ownership boundary and [API Reference](../reference/api.md)
for lifetime rules.
