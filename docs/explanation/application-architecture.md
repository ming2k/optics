# Application Architecture

What goes where, and why? This document explains how an application,
the flux modules, and Vulkan fit together.

## The four-layer stack

```
┌──────────────────────────────────────────────────────────────────┐
│  Your application                                                │
│  Owns: VkInstance, VkSurfaceKHR (via GLFW/SDL/Wayland/X11)       │
│         the main loop, input, scene state                        │
└──────────────────────────────────┬───────────────────────────────┘
                                   │
                  uses canvas, scene, compute, plus flux-core directly
                                   │
┌──────────────────────────────────┴───────────────────────────────┐
│  Modules — canvas, scene, compute                                │
│  Own: drawing pipelines, shaders, attachment policy (depth       │
│        image for scene), tessellation, scene primitives          │
└──────────────────────────────────┬───────────────────────────────┘
                                   │
                  uses flux_device + flux_frame from flux-core
                                   │
┌──────────────────────────────────┴───────────────────────────────┐
│  flux-core                                                       │
│  Owns: device lifecycle, swapchain, per-frame sync,              │
│        transient memory, bindless heap, pipeline cache           │
└──────────────────────────────────┬───────────────────────────────┘
                                   │
                                Vulkan 1.3
```

`flux_compute` is a first-class peer: it does not need a surface or a
swapchain, and it records dispatch commands into its own command
buffers or into a frame's command buffer when mixing graphics and
compute in the same submission.

## Three rules

**1. Your application owns the window and the VkInstance.**

flux does not link against GLFW, SDL, or any windowing toolkit. You
create the instance (typically with `glfwGetRequiredInstanceExtensions`),
pass the required extensions to `flux_device_create`, create your
`VkSurfaceKHR` from the window, and lend it to `flux_surface_create`.
flux borrows; you destroy.

**2. flux-core owns the frame; modules own the attachments.**

Per frame, *flux-core* acquires the swapchain image, manages
synchronisation, and provides the command buffer. *Modules* (or your
application directly) supply a `flux_pass_desc` describing the colour
attachments and any depth-stencil view they want bound. See
[ADR-0001](../adr/0001-project-foundations.md).

This means `flux_scene` carries its own depth image, `flux_canvas` does not
need one, and a future post-processing pipeline could supply N colour
attachments without touching `flux-core`.

**3. Modules do not see each other.**

`flux_canvas`, `flux_scene`, and `flux_compute` all depend on
`flux-core` but not on each other. They can be used together in a single
frame (3D scene first, 2D HUD overlay second) because both record into
the same `VkCommandBuffer` returned by `flux_frame_vk_command_buffer`.
The application is the integration point.

## Worked example: 3D scene with a 2D HUD

```c
flux_frame *frame;
flux_surface_begin_frame(surface, nullptr, &frame);

  /* 3D scene with depth */
  flux_pass_attachment att = {
      .view        = VK_NULL_HANDLE,
      .load_op     = FLUX_LOAD_CLEAR,
      .store_op    = FLUX_STORE_STORE,
      .clear_color = { 0.04f, 0.04f, 0.06f, 1.0f },
  };
  flux_pass_depth_attachment depth = {
      .view        = my_depth_view,
      .format      = VK_FORMAT_D32_SFLOAT,
      .load_op     = FLUX_LOAD_CLEAR,
      .store_op    = FLUX_STORE_DONT_CARE,
      .clear_depth = 1.0f,
  };
  flux_pass_desc pass = {
      .type                   = FLUX_TYPE_PASS_DESC,
      .color_attachment_count = 1,
      .color_attachments      = &att,
      .depth                  = &depth,
  };
  flux_frame_begin_pass(frame, &pass);

  flux_scene_draw(frame, &camera, world_matrix, mesh, material);

  flux_frame_end_pass(frame);

  /* 2D HUD on top of the rendered scene — LOAD, not CLEAR */
  flux_canvas_begin(canvas, frame, nullptr);   /* nullptr = load existing */
  flux_canvas_fill_rect_color(canvas, hud_rect, panel_color);
  flux_canvas_end(canvas);

flux_frame_submit(frame);
flux_frame_present(frame);
```

`flux_canvas_begin` with a non-NULL `clear_color` clears; with `nullptr`
it loads the existing framebuffer content, making the overlay pattern above
work naturally.

## Where the boundaries fail (deliberately)

There is intentionally **no abstraction over Vulkan** at the public
boundary. `flux_frame_vk_command_buffer` returns a raw `VkCommandBuffer`.
`flux_device_vk_device` returns a raw `VkDevice`. A module or
application can record any Vulkan command it likes between
`flux_frame_begin_pass` and `flux_frame_end_pass`.

This is on purpose. The modules themselves demonstrate the pattern:
they are not privileged consumers; they use the same public API any
application can use to build a custom renderer on top of `flux-core`.
See [ADR-0001](../adr/0001-project-foundations.md).

## See also

- [Vulkan backend](vulkan-backend.md) — the per-frame lifecycle in detail.
- [ADR-0001 — project foundations](../adr/0001-project-foundations.md)
- [ADR-0002 — per-module device state](../adr/0002-per-module-device-state.md)
