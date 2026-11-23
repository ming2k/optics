# Glossary

| Term                | Definition                                                                            |
|---------------------|---------------------------------------------------------------------------------------|
| Arena               | Bump allocator with O(1) reset; owns memory for short-lived value types in canvas.    |
| Attachment          | A Vulkan image bound as a render target for a pass (color, depth, stencil).           |
| Bézier curve        | Parametric curve defined by control points; gfx supports quadratic (1 control point) and cubic (2 control points). |
| Bradford adaptation | The chromatic adaptation transform gfx uses to bridge differing white points (e.g. D50 ↔ D65) in color-space conversions. |
| Canvas              | optics_gfx_canvas's immediate-mode recorder: one per surface, has a save/restore stack.     |
| Command buffer      | Vulkan structure recording GPU commands; gfx owns one per frame slot.                |
| Color space         | A `{ primaries, transfer function }` pair (`optics_gfx_color_space`) defining how encoded values map to linear light and which colors are representable. See [ADR-0069](../adr/0069-color-management.md). |
| Deferred upload     | Texture/buffer copy submitted with a fence without a host wait; resources recycle once the fence signals ([ADR-0022](../adr/0022-deferred-upload-submission.md)). |
| Descriptor set      | Vulkan binding for textures/samplers/buffers; gfx uses one device-wide bindless set at slot 0 instead of per-frame descriptor pools. |
| Device              | Combined context + device object (`optics_gfx_device`). Created once per application.       |
| Diátaxis            | The documentation framework this project follows (tutorials / how-to / reference / explanation). |
| Dynamic rendering   | Vulkan 1.3 feature replacing render passes; required by gfx-core.                    |
| Ear clipping        | Polygon triangulation algorithm; optics_gfx_canvas uses it for fill tessellation in v0.     |
| Frame               | One pass through `optics_gfx_surface_begin_frame` … `optics_gfx_frame_present`.                   |
| Glyph atlas         | Texture packing rasterized glyphs for glyph-run draws (`libs/text/src/atlas.c`).      |
| Material            | `<optics/gfx/scene.h>` object bundling a pipeline, descriptor data and colour for a draw.  |
| Mesh                | `<optics/gfx/scene.h>` vertex buffer plus optional index buffer.                           |
| Path                | optics_gfx_canvas's sequence of verbs + points describing a 2D shape.                       |
| Peer                | A consumer of gfx-core that records into a `optics_gfx_frame` (canvas, scene, compute, your own). |
| Pipeline cache      | Vulkan cache of compiled pipelines; gfx-core shares one across peers. Lifetime is the `optics_gfx_device`; cross-session persistence is consumer-owned (Skia `PersistentCache` model — load/save hooks on `optics_gfx_device_desc`, NULL by default). |
| Premultiplied alpha | Storage convention where R, G, B are multiplied by A before storage; gfx's universal default. |
| Primaries           | The xy chromaticities of a color space's red, green, and blue primaries plus its white point; defines the gamut. |
| RHI                 | Render Hardware Interface; an abstraction layer gfx deliberately does not provide. See [ADR-0006](../adr/0006-no-runtime-rhi.md) for the full reasoning and the conditions under which a parallel-build backend would be added. |
| Stencil             | An auxiliary buffer for masking; not used by optics_gfx_canvas today (would return for complex paths). |
| Stroker             | Algorithm expanding a centreline path into filled geometry following a stroke width.  |
| Surface             | A gfx handle wrapping a `VkSurfaceKHR` plus its swapchain and per-frame state.       |
| Swapchain           | Vulkan ring of presentable images backing a `VkSurfaceKHR`.                           |
| Tessellator         | Algorithm converting a path (with curves) into triangles for the GPU.                 |
| Tone mapping        | Compressing scene-referred luminance into a display's range (e.g. HDR → SDR), applied in the output transform ([ADR-0069](../adr/0069-color-management.md)). |
| Transfer function   | The per-component curve (sRGB, gamma, PQ, HLG, linear) mapping encoded values to linear light and back. |
| Transient memory    | gfx-core's per-frame GPU-visible ring buffer for vertex/index/uniform data.          |
| Vtable              | Function-pointer table; gfx deliberately does not use one ([ADR-0001](../adr/0001-project-foundations.md)). |
| Working space       | The fixed space all gfx rendering happens in: extended linear BT.709 (scRGB), 1.0 = 80 cd/m², on RGBA16F intermediates ([ADR-0069](../adr/0069-color-management.md)). |

## Rendering architecture terms (ADR-0089–0094)

These terms define the Clean-Break rendering architecture established by
ADR-0089 through ADR-0094.

| Term | Architecture contract |
|------|-------------------|
| DisplayList | Device-independent ordered drawing program owning its payloads and immutable resource versions; dynamic inputs are explicit ([ADR-0090](../adr/0090-display-list-resource-ownership.md)). |
| Dynamic input slot | Typed input bound to a resource version and access lease for each execution; participates in cache and lifetime validation ([ADR-0090](../adr/0090-display-list-resource-ownership.md)). |
| Resource version | Stable immutable content identity including a generation, independent of a device realization ([ADR-0090](../adr/0090-display-list-resource-ownership.md)). |
| DrawStyle | Fill or stroke parameters, opacity, and blend mode, separate from geometry and brush ([ADR-0091](../adr/0091-drawing-group-and-effect-semantics.md)). |
| Group | Ordered isolated content whose opacity is applied once before composition into its parent ([ADR-0091](../adr/0091-drawing-group-and-effect-semantics.md)). |
| RenderPlan | Target-specific validated execution structure containing passes, resource accesses, dependencies, and allocation requirements ([ADR-0089](../adr/0089-rendering-program-and-execution-plan.md)). |
| Submission | In-flight execution owner retaining dependencies until completion or terminal failure with safe retirement ([ADR-0092](../adr/0092-resource-planning-and-retirement.md)). |
| Execution session | Scoped exclusive recording access to execution state and a target; transfers in-flight obligations on submission ([ADR-0093](../adr/0093-c-state-contracts-and-rust-sessions.md)). |
| SceneSnapshot | Owned immutable UI publication containing visuals, placement, stable identities, hit-test data, accessibility semantics, and generation ([ADR-0094](../adr/0094-lens-scene-snapshots-and-invalidation.md)). |
