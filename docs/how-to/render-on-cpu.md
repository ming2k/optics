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
anti-aliased (4x supersampled), premultiplied SRC_OVER. `glyph` supplies
that host coverage when it draws into a CPU canvas. Textured image draws and
offscreen `begin_target` remain unsupported because they require GPU images.

## Read the pixels

`flux_canvas_read_pixels` returns premultiplied RGBA8, row-major
(`stride == width * 4`). The buffer is owned by the canvas and refreshed on
each call:

    uint32_t w, h, stride;
    const uint8_t *rgba = flux_canvas_read_pixels(c, &w, &h, &stride);
    /* ... encode to PNG, hash, upload ... */

    flux_canvas_release(c);

`flux_canvas_read_pixels` is backend-polymorphic: it returns the framebuffer
on a CPU canvas and `NULL` on a GPU canvas (use the offscreen surface /
[render-offscreen](render-offscreen.md) path for GPU readback).

## Bake a coverage mask

`flux_canvas_cpu_coverage` returns a single-channel coverage plane instead of
RGBA: one 8-bit value per pixel, the box-filtered mean alpha, `stride == width`.
It is what a resolution-specific mask is made of — clear to transparent, draw
the geometry once, read the plane:

    uint32_t w, h, stride;
    const uint8_t *coverage = flux_canvas_cpu_coverage(c, &w, &h, &stride);

The number of distinct coverage levels a pixel can carry is set by the
supersampling factor, not by 8-bit quantisation: N samples resolve N+1 levels.
The default (2 → 4 samples) gives 5, which is why a baked mask that will be
sampled 1:1 on screen wants more. Ask for it explicitly with
`flux_canvas_create_cpu_ss`, or through `flux_canvas_supersample_desc` on
`flux_canvas_create`:

    flux_canvas *c;   /* 8x8 = 64 samples/px -> 65 coverage levels */
    flux_canvas_create_cpu_ss(64, 64, 1.0f, FLUX_CANVAS_ANTIALIAS_AUTO, 8, &c);

`flux_canvas_create_cpu_ss` is a bake-time tool: the sample buffer costs
`ss*ss` float RGBA per pixel, so a display canvas should keep the default. Blit
a finished mask 1:1 with a NEAREST sampler — filtering it reintroduces the blur
the mask exists to remove. `lens` builds its icon cache on exactly this pair
(ADR-0112).

## Rust

The `flux` crate mirrors this:

    let c = flux::Canvas::new_cpu(256, 256, 1.0)?;
    c.begin_cpu(Some(flux::rgba(20, 20, 30, 255)))?;
    c.fill_rrect(16.0, 16.0, 224.0, 224.0, 24.0, flux::rgba(255, 90, 40, 255));
    c.end();
    let (w, h, stride, px) = c.read_pixels().expect("CPU pixels");
