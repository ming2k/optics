# How to render on the CPU (no GPU)

Render 2D canvas content entirely on the host — no Vulkan device, no
`VkSurfaceKHR`, no ICD — and read the pixels back. For CI image diffing,
golden tests, thumbnailing, or any environment without a GPU.

This differs from [render offscreen](render-offscreen.md), which is still a
GPU path (it needs a headless `optics_gfx_device`). The CPU backend needs neither
a device nor a surface. See [ADR-0019](../adr/0019-canvas-backend-seam-and-cpu-backend.md).

## Create a CPU canvas

Include `<optics/gfx/canvas_cpu.h>` and give a framebuffer size (physical pixels)
and content scale:

    #include <optics/gfx/canvas.h>
    #include <optics/gfx/canvas_cpu.h>

    optics_gfx_canvas *c = nullptr;
    optics_gfx_canvas_desc d = OPTICS_GFX_CANVAS_DESC_INIT;
    d.backend = OPTICS_GFX_CANVAS_BACKEND_CPU;   /* AUTO also picks CPU when surface is null */
    d.width = 256; d.height = 256; d.scale = 1.0f;
    optics_gfx_canvas_create(&d, &c);

## Record

Bracket the drawing with `optics_gfx_canvas_begin` / `optics_gfx_canvas_end`
(no frame). The drawing verbs are the same as the GPU backend — the unified
`optics_gfx_canvas_begin(c, NULL, clear)` works too:

    optics_gfx_color bg = optics_gfx_color_rgba_premul(20, 20, 30, 255);
    optics_gfx_canvas_begin(c, &bg);
    optics_gfx_canvas_fill_rrect(c, (optics_gfx_rect){16, 16, 224, 224}, 24.0f,
                           optics_gfx_color_rgba_premul(255, 90, 40, 255));
    optics_gfx_canvas_end(c);

Supported: solid fills, path fills/strokes (all blend modes), rounded rects/circles/squircles
(SDF), linear/radial gradients, clipping, transforms, and glyph runs with host R8 coverage —
rendered at 1x resolution with continuous analytic edge anti-aliasing. `text` supplies
that host coverage when it draws into a CPU canvas. Textured image draws and
offscreen `begin_target` remain unsupported because they require GPU images.

## Read the pixels

`optics_gfx_canvas_read_pixels` returns premultiplied RGBA8, row-major
(`stride == width * 4`). The buffer is owned by the canvas and refreshed on
each call:

    uint32_t w, h, stride;
    const uint8_t *rgba = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    /* ... encode to PNG, hash, upload ... */

    optics_gfx_canvas_release(c);

`optics_gfx_canvas_read_pixels` is backend-polymorphic: it returns the framebuffer
on a CPU canvas and `NULL` on a GPU canvas (use the offscreen surface /
[render-offscreen](render-offscreen.md) path for GPU readback).

## Bake a coverage mask

`optics_gfx_canvas_cpu_coverage` returns a single-channel 1x coverage plane instead of
RGBA: one 8-bit value per pixel matching the output alpha channel, `stride == width`.
It is what a resolution-specific mask is made of — clear to transparent, draw
the geometry once, read the plane:

    optics_gfx_canvas *c;   /* 1x continuous analytic coverage (up to 256 levels) */
    optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                           .width = 64, .height = 64}, &c);
    uint32_t w, h, stride;
    const uint8_t *coverage = optics_gfx_canvas_cpu_coverage(c, &w, &h, &stride);

Blit a finished mask 1:1 with a NEAREST sampler — filtering it reintroduces the
blur the mask exists to remove. `ui` builds its icon cache on this pair
(ADR-0112).

## Rust

The `gfx` crate mirrors this:

    let c = gfx::Canvas::new_cpu(256, 256, 1.0)?;
    c.begin_cpu(Some(gfx::rgba(20, 20, 30, 255)))?;
    c.fill_rrect(16.0, 16.0, 224.0, 224.0, 24.0, gfx::rgba(255, 90, 40, 255));
    c.end();
    let (w, h, stride, px) = c.read_pixels().expect("CPU pixels");
