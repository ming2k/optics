# flux examples

Self-contained C programs that each teach one part of the flux API. Every
example is a standalone executable built with `-Dexamples=true`:

```sh
meson setup build -Dexamples=true
meson compile -C build
```

The windowed examples (`hello_triangle`, `canvas_hello`, `image_animation`,
`scene_cube`) need a Vulkan-capable desktop and [GLFW][glfw]. The headless
compute example (`compute_fill`) has no such dependency and builds
unconditionally.

Effect showcases — demos where a visual or mathematical effect is the point
rather than the API — live in [`../showcase`](../showcase/).

[glfw]: https://www.glfw.org/

## What's here

| Example | Builds | Teaches |
|---------|--------|---------|
| [`compute_fill`](#compute_fill) | always | a compute pipeline: dispatch, SSBO readback |
| [`hello_triangle`](#hello_triangle) | GLFW | the smallest render loop; public graphics pipeline |
| [`canvas_hello`](#canvas_hello) | GLFW | the 2D canvas: paths, paint, images |
| [`image_animation`](#image_animation) | GLFW | canvas image draws + app-owned animation |
| [`scene_cube`](#scene_cube) | GLFW | 3D mesh + material, camera, a depth attachment |

### Shared bootstrap

`hello_triangle`, `canvas_hello`, `image_animation`, and `scene_cube` share
an identical device + surface + frame-loop bootstrap (GLFW window,
`VkSurfaceKHR`, `flux_device`, `flux_surface`, the `begin_frame` /
`submit` / `present` loop, and resize handling). `hello_triangle`
introduces it and the header comment in each example marks where the
shared part ends and the example-specific part begins. `pipeline_cache.h`
is a file-backed pipeline-cache helper copied verbatim by all of them —
and by the windowed showcases in `../showcase/`.

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

## canvas_hello

The 2D immediate-mode canvas: path tessellation, gradients, images, and
the save/translate/rotate state stack. Drawing is recorded into the same
frame the other examples use; the canvas just feeds it.

Text shaping is not part of flux proper — it lives in the flux-text
sibling; see `examples/flux-text/text_hello`.

## image_animation

Five canvas image-animation building blocks in four panels: eased bounce,
squash/stretch, spin+pulse, cross-fade via paint alpha, and sprite-sheet
playback via `flux_canvas_draw_image_sub`. All assets are procedural.

flux owns no animation timeline — the app drives time and the canvas just
draws. This example is the reference for that seam.

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
