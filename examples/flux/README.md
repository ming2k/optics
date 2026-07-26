# flux examples

Self-contained C programs that each teach one part of the flux API. Every
example is a standalone executable built with `-Dexamples=true`:

```sh
meson setup build -Dexamples=true
meson compile -C build
```

The windowed examples (`hello_triangle`, `canvas_hello`, `scene_cube`,
`particles_terrain`) need a Vulkan-capable desktop and [GLFW][glfw]. The
headless compute example (`compute_fill`) has no such dependency and builds
unconditionally.

[glfw]: https://www.glfw.org/

## What's here

| Example | Builds | Teaches |
|---------|--------|---------|
| [`compute_fill`](#compute_fill) | always | a compute pipeline: dispatch, SSBO readback |
| [`hello_triangle`](#hello_triangle) | GLFW | the smallest render loop; public graphics pipeline |
| [`filament_plume`](#filament_plume) | GLFW | a flowing filament plume from 10,000 procedural points |
| [`canvas_hello`](#canvas_hello) | GLFW | the 2D canvas: paths, paint, text |
| [`scene_cube`](#scene_cube) | GLFW | 3D mesh + material, camera, a depth attachment |
| [`particles_terrain`](#particles_terrain) | GLFW | animated heightfield as a point cloud |

### Shared bootstrap

`hello_triangle`, `canvas_hello`, `scene_cube`, and `particles_terrain`
share an identical device + surface + frame-loop bootstrap (GLFW window,
`VkSurfaceKHR`, `flux_device`, `flux_surface`, the `begin_frame` /
`submit` / `present` loop, and resize handling). `hello_triangle`
introduces it and the header comment in each example marks where the
shared part ends and the example-specific part begins. `pipeline_cache.h`
is a file-backed pipeline-cache helper copied verbatim by all of them.

---

## compute_fill

The only **headless** example — no window, no GLFW. A compute shader fills
a storage buffer; the host reads it back and checks the result.

The first stop for anyone who wants to see a `flux_compute_pipeline` end
to end: descriptor binding, dispatch, and GPU→CPU readback.

## hello_triangle

A single hardcoded triangle via the public `flux_graphics_pipeline` API.
Shaders are compiled to SPIR-V by meson + glslangValidator at build time
and embedded with C23 `#embed`; the pipeline is described by a plain
descriptor struct (`flux_graphics_pipeline_desc`) — no raw Vulkan
pipeline-state boilerplate in user code.

This is the canonical entry point to flux: device, surface, one pipeline,
present loop, swapchain resize, and per-frame GPU timestamps.

## filament_plume

A flowing, feather-like plume drawn from 10,000 translucent white points,
implemented as a direct shader translation of a compact p5.js formula.
`gl_VertexIndex` replaces the JavaScript loop variable, so the animation has
no vertex buffer and performs no per-point CPU work: one `vkCmdDraw` generates
the whole frame. The public pipeline uses `FLUX_TOPOLOGY_POINT_LIST` and
`FLUX_BLEND_PRESET_PREMUL`; a small push-constant block supplies time,
framebuffer extent, and DPI-aware point size.

## canvas_hello

The 2D immediate-mode canvas: path tessellation, gradients, images, and
text through the FreeType + HarfBuzz backend. Drawing is recorded into
the same frame the other examples use; the canvas just feeds it.

## scene_cube

A spinning, depth-tested cube. It introduces the 3D scene module:

- `flux_mesh_create` + `flux_material_create` (unlit and `--phong`)
- a perspective camera (`flux_camera_perspective` / `_look_at`)
- a per-frame world matrix from a quaternion
- a caller-owned **depth attachment** via `flux_target`

`flux_target` owns the depth image, its backing GPU-allocator memory, and
its view; the example recreates it on resize and hands its view to
`flux_pass_desc.depth` each frame. (An earlier version of this example
hand-wrote the `VkImage` / `VkDeviceMemory` / `VkImageView` plumbing —
`flux_target` exists precisely so that no example, or downstream app,
has to.)

## particles_terrain

An animated **heightfield rendered as a point cloud** — a flat plane of
particles that develops moving peaks and valleys, drawn with additive
blending so ridges accumulate into a glowing terrain with no triangle
mesh.

### The effect

A 256 × 256 grid (65 536 points) starts as a flat plane. Each point's
`y` is the sum of four travelling sinusoids, so the surface ripples into
ridges and basins that drift diagonally over time:

```
y = 1.20·sin(0.45x + 0.80t)
  + 1.00·sin(0.50z − 0.65t)
  + 0.55·sin(0.33(x+z) + 1.10t)
  + 0.35·sin(0.70(x−z) − 1.40t)
```

Points are coloured by height — cool blue in the valleys, warm orange on
the slopes, near-white on the peaks — and sized larger the higher they
sit. The fragment shader turns each square point into a soft, round
disc; under additive blending, overlapping points on a ridge sum toward
white, giving the crests a glowing-ridge feel. A perspective camera
orbits the centre slowly.

### Implementation

Everything about the terrain is generated **inside the vertex shader**
from `gl_VertexIndex` — there is no vertex buffer. The C side only
supplies a pipeline and per-frame push constants.

The pipeline combines three things the built-in scene/material pipelines
do not expose, all through the public `flux_graphics_pipeline` API:

| Setting | Value | Why |
|---------|-------|-----|
| `topology` | `FLUX_TOPOLOGY_POINT_LIST` | render points, not triangles |
| `blend` | `FLUX_BLEND_PRESET_ADDITIVE` | overlapping points glow |
| `depth` | `FLUX_DEPTH_TEST_AND_WRITE` | valleys occlude behind ridges |

Per frame the example:

1. recreates the depth `flux_target` on resize and transitions it to
   `DEPTH_ATTACHMENT_OPTIMAL`;
2. builds a perspective + look-at camera that orbits the centre;
3. composes `mvp = projection · view · identity` with flux math;
4. packs `mvp`, elapsed time, and base point size into a `terrain_push`
   constant and binds it;
5. issues one `vkCmdDraw` of 65 536 vertices — the shader does the rest.

The vertex shader (`particles_terrain.vert`) maps the vertex index to a
grid cell, evaluates the height field, sets `gl_PointSize` from the
height, and emits a height-mapped colour. The fragment shader
(`particles_terrain.frag`) softens each point into a circular falloff so
the cloud reads as a continuous surface rather than a sheet of squares.

### Files

- `particles_terrain.c` — bootstrap + point-list pipeline + draw loop
- `shaders/particles_terrain.vert` — procedural grid, height field, colour
- `shaders/particles_terrain.frag` — soft additive particle splat
