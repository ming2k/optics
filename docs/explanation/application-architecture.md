# Application Architecture

What goes where, and why? This document explains how an application, the
Optics libraries, and Vulkan fit together.

## The Stack

```text
application
  ├── app ── UI in a native window
  │     ├── window ── window, event loop, frame cadence, desktop integration
  │     │     └── gfx
  │     └── ui ── UI state, layout, input, snapshots
  │           ├── text ── text shaping and dynamic atlas
  │           └── gfx canvas
  └── direct rendering
        ├── motion ── motion vocabulary (header-only)
        └── gfx canvas / compute / effect / custom pipelines
                    └── device, frame, memory, sync
                                      └── Vulkan 1.3
```

There are three ways in. `optics_app_run` is a complete application host: a
window, a UI, and optional canvas content underneath it. `optics_window_run` on
its own is a window with gfx content and no UI toolkit, or with a different
UI layer plugged into `<optics/window/host.h>`. `ui` on its own is a headless UI
engine. The content siblings are producers of `gfx` primitives, not alternate
rendering backends.

The compute module (`<optics/gfx/compute.h>`) is a first-class peer: it does not need a surface or swapchain,
and it records dispatch commands into its own command buffers or a frame's
command buffer when graphics and compute share a submission.

## Boundary Rules

### The Host Owns the Window

In an `window` application, `window` owns the Wayland window, event loop, and OS
integration. In a direct `gfx` application, the application creates the
`VkInstance` and `VkSurfaceKHR` through GLFW, SDL, Wayland, or another host.
`gfx` borrows the surface handle; the host destroys it.

### ui Is Headless

Input reaches `ui` as data. Windowing, portal calls, system theme watching,
and the accessibility bus remain in `window`, which keeps `ui` deterministic
and embeddable.

### window Knows No UI Toolkit

`window` depends on `gfx` only. It folds input into an `optics_window_input`, calls the
UI host's `frame`, and presents the snapshot the host publishes; what the host
is made of is not its concern. `app` is the single place where ui and
window meet, so the binding exists once rather than once per platform backend.

### gfx-core Owns the Frame

Per frame, `gfx-core` acquires the swapchain image, manages synchronisation,
and provides the command buffer. Modules or the application supply a
`optics_gfx_pass_desc` describing the colour attachments and any depth-stencil view
they want bound. See [ADR-0001](../adr/0001-project-foundations.md).

A scene consumer supplies a depth attachment, `optics_gfx_canvas` does not need one,
and a post-processing pipeline can supply its own colour attachments without
changing `gfx-core`.

### gfx Modules Do Not See Each Other

The canvas, scene and compute modules all depend on the gfx core but
not on each other. They can be used together in a single frame because each
records into the same `VkCommandBuffer` returned by
`optics_gfx_frame_vk_command_buffer`. The application is the integration point.

### Content and Domain Libraries Feed Draw Primitives

`text` shapes text and feeds `optics_gfx_canvas_draw_glyph_run`;
`motion` provides closed-form non-divergent motion dynamics. They link
through public `gfx` APIs and remain separately consumable libraries.

### Events Are Reified, Intercepted, Then Folded

`window` reifies each native event — Wayland `wl_pointer`, Win32 `WM_*`,
Cocoa `NSEvent` — into one platform-neutral `optics_window_event`
(`<optics/window/event.h>`) in arrival order. Before folding the survivors into the
frame's `optics_window_input`, `window` runs the host's optional
`optics_window_opts.on_event` callback, which may drop or rewrite each event.
This is the only point at which a host may change what `ui` sees;
The UI host's `frame` receives the already-folded snapshot; `app` converts it to `optics_ui_input` for `build`. `window`
itself applies no policy — it reifies, orders, folds, and forwards. See
[ADR-0107](../adr/0107-iris-pre-lens-event-seam.md).

## Worked Example: Custom Pipeline Content with a 2D HUD

```c
optics_gfx_frame *frame;
optics_gfx_surface_begin_frame(surface, nullptr, &frame);

/* caller-defined pipeline content with a depth attachment */
optics_gfx_pass_attachment att = {
    .view        = VK_NULL_HANDLE,
    .load_op     = OPTICS_GFX_LOAD_CLEAR,
    .store_op    = OPTICS_GFX_STORE_STORE,
    .clear_color = { 0.04f, 0.04f, 0.06f, 1.0f },
};
optics_gfx_pass_depth_attachment depth = {
    .view        = my_depth_view,
    .format      = VK_FORMAT_D32_SFLOAT,
    .load_op     = OPTICS_GFX_LOAD_CLEAR,
    .store_op    = OPTICS_GFX_STORE_DONT_CARE,
    .clear_depth = 1.0f,
};
optics_gfx_pass_desc pass = {
    .type                   = OPTICS_GFX_TYPE_PASS_DESC,
    .color_attachment_count = 1,
    .color_attachments      = &att,
    .depth                  = &depth,
};
optics_gfx_frame_begin_pass(frame, &pass);

/* Bind a caller-defined optics_gfx_graphics_pipeline and record custom draws. */
optics_gfx_graphics_pipeline_bind(cmd, &(optics_gfx_graphics_pipeline_bind_opts){
    .pipeline = my_pipeline,
});
vkCmdDraw(cmd, 3, 1, 0, 0);

optics_gfx_frame_end_pass(frame);

/* 2D HUD on top of the rendered content: load instead of clear. */
optics_gfx_canvas_begin(canvas, frame, nullptr);
optics_gfx_canvas_fill_rect_color(canvas, hud_rect, panel_color);
optics_gfx_canvas_end(canvas);

optics_gfx_frame_submit(frame);
optics_gfx_frame_present(frame);
```

`optics_gfx_canvas_begin` with a non-null `clear_color` clears; with `nullptr` it
loads the existing framebuffer content, making the overlay pattern work
naturally.

## The Deliberate Vulkan Seam

There is intentionally no abstraction over Vulkan at the public boundary.
`optics_gfx_frame_vk_command_buffer` returns a raw `VkCommandBuffer`, and
`optics_gfx_device_vk_device` returns a raw `VkDevice`. A module or application can
record Vulkan commands between `optics_gfx_frame_begin_pass` and
`optics_gfx_frame_end_pass`.

The modules are not privileged consumers; they use the same public API that an
application can use to build a custom renderer on top of `gfx-core`. See
[ADR-0001](../adr/0001-project-foundations.md).

## See Also

- [Value-Semantic Rendering](rendering-model.md) — the rendering architecture, display lists, and GPU hardware mapping.
- [Vulkan backend](vulkan-backend.md) — the per-frame lifecycle in detail.
- [ADR-0001 — project foundations](../adr/0001-project-foundations.md)
- [ADR-0002 — per-module device state](../adr/0002-per-module-device-state.md)
- [ADR-0023 — unified monorepo build](../adr/0023-unified-monorepo-build.md)
- [ADR-0107 — the pre-ui event seam](../adr/0107-iris-pre-lens-event-seam.md)
