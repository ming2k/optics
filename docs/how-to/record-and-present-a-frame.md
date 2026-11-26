# How to record and present a frame

Every frame in gfx follows the same five-call pattern. This guide
walks through it once. Once you can read this code, every example
in the tree (and every module) makes sense.

## Prerequisites

You have a `optics_gfx_device *d` and a `optics_gfx_surface *s`. The fastest path
to a working pair is to copy `examples/getting_started/canvas_window.c` and trim it;
[tutorial 01](../tutorials/01-getting-started.md) walks through the
build and run.

## The pattern

    optics_gfx_frame *frame;
    if (optics_gfx_surface_begin_frame(s, nullptr, &frame) != OPTICS_GFX_OK) {
        /* surface lost — skip this iteration */
        return;
    }

    optics_gfx_pass_attachment att = {
        .view        = VK_NULL_HANDLE,   /* use swapchain image */
        .load_op     = OPTICS_GFX_LOAD_CLEAR,
        .store_op    = OPTICS_GFX_STORE_STORE,
        .clear_color = { 0.1f, 0.1f, 0.12f, 1.0f },
    };
    optics_gfx_pass_desc pass = {
        .type                   = OPTICS_GFX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments      = &att,
    };
    optics_gfx_frame_begin_pass(frame, &pass);

    /* record draws — vkCmd* calls against optics_gfx_frame_vk_command_buffer(frame),
     * or draw with the canvas or scene modules */

    optics_gfx_frame_end_pass(frame);
    (void)optics_gfx_frame_submit (frame);
    (void)optics_gfx_frame_present(frame);

## What each call does

| Call                          | What happens                                                           |
|-------------------------------|------------------------------------------------------------------------|
| `optics_gfx_surface_begin_frame`    | Waits on the in-flight fence for this frame slot, acquires the next swapchain image, begins the command buffer. |
| `optics_gfx_frame_begin_pass`       | Transitions the swapchain image to `COLOR_ATTACHMENT_OPTIMAL`, calls `vkCmdBeginRendering` with the desc. |
| (your draws)                  | Run between `begin_pass` and `end_pass`.                               |
| `optics_gfx_frame_end_pass`         | Calls `vkCmdEndRendering`, transitions the image to `PRESENT_SRC_KHR`. |
| `optics_gfx_frame_submit`           | Ends the command buffer, calls `vkQueueSubmit2`.                       |
| `optics_gfx_frame_present`          | Calls `vkQueuePresentKHR` and rotates the frame slot.                  |

## Adding a depth attachment

Provide a caller-owned image view in the pass desc:

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

When you write a custom depth-buffered renderer, create an `optics_gfx_target`
depth image matching the surface extent and recreate it on surface resize.

## Recording with a module

For 2D content, do not call `optics_gfx_frame_begin_pass` yourself —
`optics_gfx_canvas_begin` does it internally. Custom `optics_gfx_graphics_pipeline`
draws expect to be recorded inside an active pass that you began with
`optics_gfx_frame_begin_pass`.

You can mix raw Vulkan recording with module drawing within the same
pass; both write to the same command buffer returned by
`optics_gfx_frame_vk_command_buffer`.

## Handling resize and surface loss

| Symptom                                            | Response                                |
|----------------------------------------------------|-----------------------------------------|
| Window resized                                     | Call `optics_gfx_surface_resize(s, w, h)` before the next frame. |
| `optics_gfx_surface_begin_frame` returns `OPTICS_GFX_ERROR_SURFACE_LOST` | The swapchain is stale (`OUT_OF_DATE`). Pull the new framebuffer size from your window system and call `optics_gfx_surface_resize(s, w, h)` before the next frame; skip this one. |
| `optics_gfx_frame_present` returns `OPTICS_GFX_ERROR_SURFACE_LOST` | Same — `OUT_OF_DATE` or `SUBOPTIMAL` was returned at present. Resize, then continue.   |
| `vkQueueWaitIdle` errors with `VK_ERROR_DEVICE_LOST` | Recreate the `optics_gfx_device`. Persistent state (images, meshes) must be recreated too. |

## See also

- [Vulkan backend explanation](../explanation/vulkan-backend.md) — the
  same lifecycle in prose, with diagrams.
- [examples/getting_started/canvas_window.c](../../examples/getting_started/canvas_window.c) — minimal
  working frame loop.
