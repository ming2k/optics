# How to record and present a frame

Every frame in flux follows the same five-call pattern. This guide
walks through it once. Once you can read this code, every example
in the tree (and every module) makes sense.

## Prerequisites

You have a `flux_device *d` and a `flux_surface *s`. The fastest path
to a working pair is to copy `examples/flux/hello_triangle.c` and trim it;
[tutorial 01](../tutorials/01-getting-started.md) walks through the
build and run.

## The pattern

    flux_frame *frame;
    if (flux_surface_begin_frame(s, nullptr, &frame) != FLUX_OK) {
        /* surface lost — skip this iteration */
        return;
    }

    flux_pass_attachment att = {
        .view        = VK_NULL_HANDLE,   /* use swapchain image */
        .load_op     = FLUX_LOAD_CLEAR,
        .store_op    = FLUX_STORE_STORE,
        .clear_color = { 0.1f, 0.1f, 0.12f, 1.0f },
    };
    flux_pass_desc pass = {
        .type                   = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments      = &att,
    };
    flux_frame_begin_pass(frame, &pass);

    /* record draws — vkCmd* calls against flux_frame_vk_command_buffer(frame),
     * or use a module like flux_canvas / flux_scene */

    flux_frame_end_pass(frame);
    (void)flux_frame_submit (frame);
    (void)flux_frame_present(frame);

## What each call does

| Call                          | What happens                                                           |
|-------------------------------|------------------------------------------------------------------------|
| `flux_surface_begin_frame`    | Waits on the in-flight fence for this frame slot, acquires the next swapchain image, begins the command buffer. |
| `flux_frame_begin_pass`       | Transitions the swapchain image to `COLOR_ATTACHMENT_OPTIMAL`, calls `vkCmdBeginRendering` with the desc. |
| (your draws)                  | Run between `begin_pass` and `end_pass`.                               |
| `flux_frame_end_pass`         | Calls `vkCmdEndRendering`, transitions the image to `PRESENT_SRC_KHR`. |
| `flux_frame_submit`           | Ends the command buffer, calls `vkQueueSubmit2`.                       |
| `flux_frame_present`          | Calls `vkQueuePresentKHR` and rotates the frame slot.                  |

## Adding a depth attachment

Provide a caller-owned image view in the pass desc:

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

`examples/flux/scene_cube.c` does exactly this; it owns its own depth image
and recreates it on surface resize. Copy that pattern when you write a
3D renderer.

## Recording with a module

For 2D content, do not call `flux_frame_begin_pass` yourself —
`flux_canvas_begin_frame` does it internally. Likewise, `flux_scene_draw_mesh`
expects to be called inside an active pass that you began manually
(or inside a pass that another module opened).

You can mix raw Vulkan recording with module drawing within the same
pass; both write to the same command buffer returned by
`flux_frame_vk_command_buffer`.

## Handling resize and surface loss

| Symptom                                            | Response                                |
|----------------------------------------------------|-----------------------------------------|
| Window resized                                     | Call `flux_surface_resize(s, w, h)` before the next frame. |
| `flux_surface_begin_frame` returns `FLUX_ERROR_SURFACE_LOST` | The swapchain is stale (`OUT_OF_DATE`). Pull the new framebuffer size from your window system and call `flux_surface_resize(s, w, h)` before the next frame; skip this one. |
| `flux_frame_present` returns `FLUX_ERROR_SURFACE_LOST` | Same — `OUT_OF_DATE` or `SUBOPTIMAL` was returned at present. Resize, then continue.   |
| `vkQueueWaitIdle` errors with `VK_ERROR_DEVICE_LOST` | Recreate the `flux_device`. Persistent state (images, meshes) must be recreated too. |

## See also

- [Vulkan backend explanation](../explanation/vulkan-backend.md) — the
  same lifecycle in prose, with diagrams.
- [examples/flux/hello_triangle.c](../../examples/flux/hello_triangle.c) — minimal
  working frame loop.
