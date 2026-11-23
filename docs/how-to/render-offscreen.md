# How to render offscreen (no window)

Render canvas or scene content into a gfx-owned image and read the
pixels back — for thumbnailers, golden-image tests, compositors, or
any headless host. No `VkSurfaceKHR`, no windowing library.

## Prerequisites

A `optics_gfx_device *d` created with `desc.headless = true` (no
windowing-system instance extensions needed). Any module that draws
into a frame works on top; the examples below use the canvas.

## Create the surface

Leave `vk_surface_khr` null and give an extent — that selects the
offscreen mode ([ADR-0013](../adr/0013-offscreen-surface.md)):

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width  = 512;
    sd.height = 512;

    optics_gfx_surface *surface = nullptr;
    if (optics_gfx_surface_create(d, &sd, &surface) != OPTICS_GFX_OK) return;

Both dimensions are required; zero is rejected with
`OPTICS_GFX_ERROR_INVALID_ARGUMENT` (there is no "minimised" state to defer
to). The color target is `OPTICS_GFX_FORMAT_RGBA8_UNORM`, and `hdr_preferred`
/ `vsync` are ignored.

## Render — the frame loop is unchanged

The begin → draw → submit → present sequence is exactly the windowed
one, so rendering code moves between a window and an offscreen target
without edits. `optics_gfx_frame_present` simply completes the frame instead
of presenting.

    optics_gfx_frame *frame = nullptr;
    if (optics_gfx_surface_begin_frame(surface, nullptr, &frame) != OPTICS_GFX_OK) return;

    optics_gfx_color clear = optics_gfx_color_rgba(20, 20, 28, 255);
    if (optics_gfx_canvas_begin(canvas, frame, &clear) != OPTICS_GFX_OK) return;
    optics_gfx_canvas_fill_rect_color(canvas,
        (optics_gfx_rect){ 128, 128, 256, 256 },
        optics_gfx_color_rgba(255, 255, 255, 255));
    optics_gfx_canvas_end(canvas);

    if (optics_gfx_frame_submit(frame)  != OPTICS_GFX_OK) return;
    if (optics_gfx_frame_present(frame) != OPTICS_GFX_OK) return;

## Read the pixels back

`optics_gfx_surface_read_pixels` waits for the most recently submitted
frame's GPU work and copies it out as tightly packed RGBA8, row-major
from the top-left:

    size_t   bytes = (size_t)512 * 512 * 4;
    uint8_t *px    = malloc(bytes);
    if (optics_gfx_surface_read_pixels(surface, px, bytes) != OPTICS_GFX_OK) return;
    /* px[(y * 512 + x) * 4 + 0] is the red byte of (x, y) */

Calling it before the first `optics_gfx_frame_submit` returns
`OPTICS_GFX_ERROR_INVALID_STATE`; calling it on a windowed surface returns
`OPTICS_GFX_ERROR_UNSUPPORTED`. The readback is synchronous — for a
per-frame capture loop, expect it to cost a GPU round trip each call.

## Notes

- `optics_gfx_surface_resize` works and drops the old contents; the next
  `optics_gfx_surface_read_pixels` before a new submit reports
  `OPTICS_GFX_ERROR_INVALID_STATE`.
- Raw-Vulkan interop: `optics_gfx_surface_vk_swapchain` returns
  `VK_NULL_HANDLE`, `optics_gfx_frame_vk_image` / `optics_gfx_frame_vk_image_view`
  return the offscreen image being recorded, and
  `optics_gfx_frame_vk_command_buffer` works as usual.
- Colors land in memory as R, G, B, A bytes
  (`OPTICS_GFX_FORMAT_RGBA8_UNORM`), independent of the BGRA ordering most
  swapchains negotiate.
