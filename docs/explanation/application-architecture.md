# Application Architecture

What goes where, and why? This document explains how an application, the
Optics libraries, and Vulkan fit together.

## The Stack

```text
application
  ├── iris ── window, event loop, desktop integration
  │     └── lens ── UI state, layout, input, draw lists
  │           ├── flux-text ── shaping and glyph runs
  │           └── flux canvas
  └── direct rendering
        ├── flux-scene-graph ── glTF content
        └── flux canvas / scene / compute / effect
                    └── flux-core ── device, frame, memory, sync
                                      └── Vulkan 1.3
```

Applications can stop at the `iris` surface for a complete UI host or use any
lower layer directly. `lens` remains useful without a window. The content
siblings are producers of `flux` primitives, not alternate rendering
backends.

`flux_compute` is a first-class peer: it does not need a surface or swapchain,
and it records dispatch commands into its own command buffers or a frame's
command buffer when graphics and compute share a submission.

## Boundary Rules

### The Host Owns the Window

In an `iris` application, `iris` owns the Wayland window, event loop, and OS
integration. In a direct `flux` application, the application creates the
`VkInstance` and `VkSurfaceKHR` through GLFW, SDL, Wayland, or another host.
`flux` borrows the surface handle; the host destroys it.

### Lens Is Headless

Input reaches `lens` as data. Windowing, portal calls, system theme watching,
and the accessibility bus remain in `iris`, which keeps `lens` deterministic
and embeddable.

### Flux-core Owns the Frame

Per frame, `flux-core` acquires the swapchain image, manages synchronisation,
and provides the command buffer. Modules or the application supply a
`flux_pass_desc` describing the colour attachments and any depth-stencil view
they want bound. See [ADR-0001](../adr/0001-project-foundations.md).

A scene consumer supplies a depth attachment, `flux_canvas` does not need one,
and a post-processing pipeline can supply its own colour attachments without
changing `flux-core`.

### Flux Modules Do Not See Each Other

`flux_canvas`, `flux_scene`, and `flux_compute` all depend on `flux-core` but
not on each other. They can be used together in a single frame because each
records into the same `VkCommandBuffer` returned by
`flux_frame_vk_command_buffer`. The application is the integration point.

### Content Libraries Feed Draw Primitives

`flux-text` shapes text and feeds `flux_canvas_draw_glyph_run`;
`flux-scene-graph` loads glTF content and feeds scene mesh draws. They link
through public `flux` APIs and remain separately consumable libraries.

## Worked Example: 3D Scene with a 2D HUD

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

flux_scene_draw_mesh(frame, &camera, world_matrix, mesh, material);

flux_frame_end_pass(frame);

/* 2D HUD on top of the rendered scene: load instead of clear. */
flux_canvas_begin(canvas, frame, nullptr);
flux_canvas_fill_rect_color(canvas, hud_rect, panel_color);
flux_canvas_end(canvas);

flux_frame_submit(frame);
flux_frame_present(frame);
```

`flux_canvas_begin` with a non-null `clear_color` clears; with `nullptr` it
loads the existing framebuffer content, making the overlay pattern work
naturally.

## The Deliberate Vulkan Seam

There is intentionally no abstraction over Vulkan at the public boundary.
`flux_frame_vk_command_buffer` returns a raw `VkCommandBuffer`, and
`flux_device_vk_device` returns a raw `VkDevice`. A module or application can
record Vulkan commands between `flux_frame_begin_pass` and
`flux_frame_end_pass`.

The modules are not privileged consumers; they use the same public API that an
application can use to build a custom renderer on top of `flux-core`. See
[ADR-0001](../adr/0001-project-foundations.md).

## See Also

- [Vulkan backend](vulkan-backend.md) — the per-frame lifecycle in detail.
- [ADR-0001 — project foundations](../adr/0001-project-foundations.md)
- [ADR-0002 — per-module device state](../adr/0002-per-module-device-state.md)
- [ADR-0023 — unified monorepo build](../adr/0023-unified-monorepo-build.md)
