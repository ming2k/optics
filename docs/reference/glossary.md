# Glossary

| Term                | Definition                                                                            |
|---------------------|---------------------------------------------------------------------------------------|
| Arena               | Bump allocator with O(1) reset; owns memory for short-lived value types in canvas.    |
| Attachment          | A Vulkan image bound as a render target for a pass (color, depth, stencil).           |
| Bézier curve        | Parametric curve defined by control points; flux supports quadratic (1 control point) and cubic (2 control points). |
| Bradford adaptation | The chromatic adaptation transform flux uses to bridge differing white points (e.g. D50 ↔ D65) in color-space conversions. |
| Canvas              | flux_canvas's immediate-mode recorder: one per surface, has a save/restore stack.     |
| Command buffer      | Vulkan structure recording GPU commands; flux owns one per frame slot.                |
| Composition graph   | Explicit DAG of image-producing passes above Flux operators; plans ROI, damage, and target lifetimes without owning GPU resources ([ADR-0080](../adr/0080-explicit-offscreen-composition-graph.md)). |
| Color space         | A `{ primaries, transfer function }` pair (`flux_color_space`) defining how encoded values map to linear light and which colors are representable. See [ADR-0069](../adr/0069-color-management.md). |
| Deferred upload     | Texture/buffer copy submitted with a fence without a host wait; resources recycle once the fence signals ([ADR-0022](../adr/0022-deferred-upload-submission.md)). |
| Descriptor set      | Vulkan binding for textures/samplers/buffers; flux uses one device-wide bindless set at slot 0 instead of per-frame descriptor pools. |
| Device              | Combined context + device object (`flux_device`). Created once per application.       |
| Diátaxis            | The documentation framework this project follows (tutorials / how-to / reference / explanation). |
| Dynamic rendering   | Vulkan 1.3 feature replacing render passes; required by flux-core.                    |
| Ear clipping        | Polygon triangulation algorithm; flux_canvas uses it for fill tessellation in v0.     |
| Frame               | One pass through `flux_surface_begin_frame` … `flux_frame_present`.                   |
| Glyph atlas         | Texture packing rendered glyphs; not in the library today.                            |
| Material            | flux_scene concept bundling a pipeline + descriptor data + colour for a draw.         |
| Mesh                | flux_scene's vertex + (optional) index buffer pair.                                   |
| Path                | flux_canvas's sequence of verbs + points describing a 2D shape.                       |
| Peer                | A consumer of flux-core that records into a `flux_frame` (canvas, scene, compute, your own). |
| Pipeline cache      | Vulkan cache of compiled pipelines; flux-core shares one across peers. Lifetime is the `flux_device`; cross-session persistence is consumer-owned (Skia `PersistentCache` model — load/save hooks on `flux_device_desc`, NULL by default). |
| Premultiplied alpha | Storage convention where R, G, B are multiplied by A before storage; flux's universal default. |
| Primaries           | The xy chromaticities of a color space's red, green, and blue primaries plus its white point; defines the gamut. |
| RHI                 | Render Hardware Interface; an abstraction layer flux deliberately does not provide. See [ADR-0006](../adr/0006-no-runtime-rhi.md) for the full reasoning and the conditions under which a parallel-build backend would be added. |
| Stencil             | An auxiliary buffer for masking; not used by flux_canvas today (would return for complex paths). |
| Stroker             | Algorithm expanding a centreline path into filled geometry following a stroke width.  |
| Surface             | A flux handle wrapping a `VkSurfaceKHR` plus its swapchain and per-frame state.       |
| Swapchain           | Vulkan ring of presentable images backing a `VkSurfaceKHR`.                           |
| Tessellator         | Algorithm converting a path (with curves) into triangles for the GPU.                 |
| Tone mapping        | Compressing scene-referred luminance into a display's range (e.g. HDR → SDR), applied in the output transform ([ADR-0069](../adr/0069-color-management.md)). |
| Transfer function   | The per-component curve (sRGB, gamma, PQ, HLG, linear) mapping encoded values to linear light and back. |
| Transient memory    | flux-core's per-frame GPU-visible ring buffer for vertex/index/uniform data.          |
| Vtable              | Function-pointer table; flux deliberately does not use one ([ADR-0001](../adr/0001-project-foundations.md)). |
| Working space       | The fixed space all flux rendering happens in: extended linear BT.709 (scRGB), 1.0 = 80 cd/m², on RGBA16F intermediates ([ADR-0069](../adr/0069-color-management.md)). |
