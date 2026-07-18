# How to render on the CPU (no GPU)

Render 2D canvas content entirely on the host — no Vulkan device, no
`VkSurfaceKHR`, no ICD — and read the pixels back. For CI image diffing,
golden tests, thumbnailing, or any environment without a GPU.

This differs from [render offscreen](render-offscreen.md), which is still a
GPU path (it needs a headless `flux_device`). The CPU backend needs neither
a device nor a surface. See [ADR-0019](../adr/0019-canvas-backend-seam-and-cpu-backend.md).

## Create a CPU canvas

Include `<flux/canvas_cpu.h>` and give a framebuffer size (physical pixels)
and content scale:

    #include <flux/canvas.h>
    #include <flux/canvas_cpu.h>

    flux_canvas *c = nullptr;
    if (flux_canvas_create_cpu(256, 256, 1.0f, &c) != FLUX_OK) return;

Equivalently, via the unified factory (Skia-style — the backend field
selects the implementation):

    flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
    d.backend = FLUX_CANVAS_BACKEND_CPU;   /* AUTO also picks CPU when surface is null */
    d.width = 256; d.height = 256; d.scale = 1.0f;
    flux_canvas_create(&d, &c);

## Record

Bracket the drawing with `flux_canvas_cpu_begin` / `flux_canvas_cpu_end`
(no frame). The drawing verbs are the same as the GPU backend — the unified
`flux_canvas_begin_frame(c, NULL, clear)` works too:

    flux_color bg = flux_color_rgba_premul(20, 20, 30, 255);
    flux_canvas_cpu_begin(c, &bg);
    flux_canvas_fill_rrect(c, (flux_rect){16, 16, 224, 224}, 24.0f,
                           flux_color_rgba_premul(255, 90, 40, 255));
    flux_canvas_cpu_end(c);

Supported: solid fills, path fills/strokes, rounded rects, linear/radial
gradients, clipping, transforms, and glyph runs with host R8 coverage —
anti-aliased (4x supersampled), premultiplied SRC_OVER. `flux-text` supplies
that host coverage when it draws into a CPU canvas. Textured image draws and
offscreen `begin_target` remain unsupported because they require GPU images.

## Read the pixels

`flux_canvas_read_pixels` returns premultiplied RGBA8, row-major
(`stride == width * 4`). The buffer is owned by the canvas and refreshed on
each call:

    uint32_t w, h, stride;
    const uint8_t *rgba = flux_canvas_read_pixels(c, &w, &h, &stride);
    /* ... encode to PNG, hash, upload ... */

    flux_canvas_destroy(c);

`flux_canvas_read_pixels` is backend-polymorphic: it returns the framebuffer
on a CPU canvas and `NULL` on a GPU canvas (use the offscreen surface /
[render-offscreen](render-offscreen.md) path for GPU readback).

## Rust

The `flux` crate mirrors this:

    let c = flux::Canvas::new_cpu(256, 256, 1.0)?;
    c.begin_cpu(Some(flux::rgba(20, 20, 30, 255)))?;
    c.fill_rrect(16.0, 16.0, 224.0, 224.0, 24.0, flux::rgba(255, 90, 40, 255));
    c.end();
    let (w, h, stride, px) = c.read_pixels().expect("CPU pixels");
