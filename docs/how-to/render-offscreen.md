# How to render offscreen (no window)

Render canvas or scene content into a flux-owned image and read the
pixels back — for thumbnailers, golden-image tests, compositors, or
any headless host. No `VkSurfaceKHR`, no windowing library.

## Prerequisites

A `flux_device *d` created with `desc.headless = true` (no
windowing-system instance extensions needed). Any module that draws
into a frame works on top; the examples below use the canvas
(`-Dcanvas=true`, default).

## Create the surface

Leave `vk_surface_khr` null and give an extent — that selects the
offscreen mode ([ADR-0013](../adr/0013-offscreen-surface.md)):

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width  = 512;
    sd.height = 512;

    flux_surface *surface = nullptr;
    if (flux_surface_create(d, &sd, &surface) != FLUX_OK) return;

Both dimensions are required; zero is rejected with
`FLUX_ERROR_INVALID_ARGUMENT` (there is no "minimised" state to defer
to). The color target is `FLUX_FORMAT_RGBA8_UNORM`, and `hdr_preferred`
/ `vsync` are ignored.

## Render — the frame loop is unchanged

The begin → draw → submit → present sequence is exactly the windowed
one, so rendering code moves between a window and an offscreen target
without edits. `flux_frame_present` simply completes the frame instead
of presenting.

    flux_frame *frame = nullptr;
    if (flux_surface_begin_frame(surface, nullptr, &frame) != FLUX_OK) return;

    flux_color clear = flux_color_rgba(20, 20, 28, 255);
    if (flux_canvas_begin_frame(canvas, frame, &clear) != FLUX_OK) return;
    flux_canvas_fill_rect_color(canvas,
        (flux_rect){ 128, 128, 256, 256 },
        flux_color_rgba(255, 255, 255, 255));
    flux_canvas_end_frame(canvas);

    if (flux_frame_submit(frame)  != FLUX_OK) return;
    if (flux_frame_present(frame) != FLUX_OK) return;

## Read the pixels back

`flux_surface_read_pixels` waits for the most recently submitted
frame's GPU work and copies it out as tightly packed RGBA8, row-major
from the top-left:

    size_t   bytes = (size_t)512 * 512 * 4;
    uint8_t *px    = malloc(bytes);
    if (flux_surface_read_pixels(surface, px, bytes) != FLUX_OK) return;
    /* px[(y * 512 + x) * 4 + 0] is the red byte of (x, y) */

Calling it before the first `flux_frame_submit` returns
`FLUX_ERROR_INVALID_STATE`; calling it on a windowed surface returns
`FLUX_ERROR_UNSUPPORTED`. The readback is synchronous — for a
per-frame capture loop, expect it to cost a GPU round trip each call.

## Notes

- `flux_surface_resize` works and drops the old contents; the next
  `flux_surface_read_pixels` before a new submit reports
  `FLUX_ERROR_INVALID_STATE`.
- Raw-Vulkan interop: `flux_surface_vk_swapchain` returns
  `VK_NULL_HANDLE`, `flux_frame_vk_image` / `flux_frame_vk_image_view`
  return the offscreen image being recorded, and
  `flux_frame_vk_command_buffer` works as usual.
- Colors land in memory as R, G, B, A bytes
  (`FLUX_FORMAT_RGBA8_UNORM`), independent of the BGRA ordering most
  swapchains negotiate.
