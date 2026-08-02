# ADR-0048: Textured surface materials and core images

- Status: Accepted
- Date: 2026-08-02

## Context

`flux-scene-graph` uploaded glTF vertices, normals, UVs, skins, and animation,
but discarded each primitive's material index. Applications supplied one
`flux_material` for the whole scene. A VRM whose white base-color factors were
modulated by embedded PNG textures therefore rendered as a white model.

The missing behavior crosses two established boundaries. Texture sampling,
alpha testing, blending, culling, and shaders are draw primitives owned by
`libflux`. Parsing glTF material JSON and decoding PNG or JPEG content are
asset-production responsibilities owned above the RHI by
`flux-scene-graph`, as established by [ADR-0016](0016-pure-rhi-and-draw-primitives.md).
`flux_image` was nevertheless declared and compiled as part of Canvas even
though scene materials and effects consume the same GPU resource.

## Decision

Make `flux_image` a core GPU resource and add an optional
`flux_material_surface_desc` extension to `flux_material_desc`. The extension
defines a retained base-color image and sampler, UV scale/rotation/offset,
OPAQUE/MASK/BLEND alpha semantics, alpha cutoff, and double-sided culling.
UNLIT and PHONG shaders sample the device bindless heap. BLEND materials use
straight source-alpha blending and disable depth writes; MASK materials
discard below the cutoff.

Keep asset decoding in the safe Rust `flux-scene-graph` crate. It validates
the GLB, decodes only referenced embedded PNG/JPEG images under explicit
dimension and allocation limits, uploads base-color images as sRGB, maps glTF
samplers and `KHR_texture_transform`, and builds one immutable Flux material
per glTF material. Non-zero texture-coordinate sets and external image URIs
remain explicit load errors for the in-memory GLB API.

The C scene graph preserves every primitive's glTF material index and retains
a host-installed material table plus fallback. A non-NULL draw-option
material remains a whole-scene compatibility override. Scene-owned draws
record OPAQUE/MASK primitives before BLEND primitives while preserving source
order inside each phase.

## Alternatives Considered

- **Decode images in the C scene-graph parser.** Rejected because it adds
  codec dependencies to the Vulkan-only C stack and violates ADR-0016's
  producer boundary.
- **Keep one application-created material per scene.** Rejected because it
  cannot represent per-primitive textures, alpha modes, or culling and is the
  direct cause of untextured VRM output.
- **Put sampled images behind Canvas.** Rejected because a GPU image is shared
  RHI state, not a 2D drawing policy. Scene must not depend on Canvas.
- **Append fields to `flux_material_desc`.** Rejected in favor of the existing
  tagged `next` extension convention, which preserves the base descriptor's
  ABI and source compatibility.

## Consequences

- `flux_image` creation, lifetime, update, and render-target declarations are
  always available from `<flux/core.h>`; Canvas continues to consume them.
- A material retains its optional image and sampler, so callers may release
  their references after `flux_material_create` succeeds.
- `flux-scene-graph` gains Rust dependencies on `gltf` and bounded PNG/JPEG
  decoding through `image`; `image` is pinned to `0.25.8` for Rust 1.85.
- The material layer implements the base-color subset required by unlit VRM
  assets. Metallic-roughness PBR, normal/emissive textures, morph targets,
  external resources, and additional UV sets remain separate additive work.

## References

- [ADR-0016: Pure RHI and draw primitives](0016-pure-rhi-and-draw-primitives.md)
- [KHR_texture_transform](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_texture_transform)
- [KHR_materials_unlit](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_materials_unlit)
