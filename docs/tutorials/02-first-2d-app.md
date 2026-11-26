# Your first 2D canvas application

You will write `hello_canvas.c` from scratch: a window that fills its
client area with a coloured rectangle. By the end you will understand
the three objects every gfx-canvas app needs (device, surface,
canvas) and the per-frame call sequence.

## Step 1 — Start from a Known-good Skeleton

Open `examples/getting_started/canvas_window.c` on a disposable branch. We will trim the
known-good example instead of starting from a blank file. The full version
records animated 2D vector geometry through the `window` layer; you will strip
it down, then add drawing back.

## Step 2 — minimum viable frame

Delete everything between `optics_gfx_canvas_begin` and `optics_gfx_canvas_end`
except this:

    optics_gfx_canvas_fill_rect_color(canvas,
        (optics_gfx_rect){ 100, 100, 400, 300 },
        optics_gfx_color_rgba(60, 140, 220, 255));

Build:

    meson compile -C build && ./build/examples/getting_started/canvas_window

A blue rectangle appears on a dark background. That is the whole shape
of a gfx-canvas frame.

## Step 3 — add a path

Before the main loop:

    optics_gfx_arena path_arena;
    if (optics_gfx_arena_init(&path_arena, 16 * 1024, nullptr) != OPTICS_GFX_OK) {
        /* see optics_gfx_get_last_error for detail */
        return 1;
    }

Inside the loop, between `optics_gfx_canvas_begin` and `optics_gfx_canvas_end`:

    optics_gfx_arena_reset(&path_arena);

    optics_gfx_path *circle = nullptr;
    if (optics_gfx_path_create(&circle, &path_arena) != OPTICS_GFX_OK) continue;
    optics_gfx_path_add_circle(circle, 400, 300, 80);

    optics_gfx_brush paint = optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 200, 80, 255));
    optics_gfx_canvas_fill_path(canvas, circle, OPTICS_GFX_FILL_NON_ZERO, &paint);

`optics_gfx_path_create` is `[[nodiscard]]` — it returns `OPTICS_GFX_OK` on success,
`OPTICS_GFX_ERROR_OUT_OF_MEMORY` if the arena is exhausted, or
`OPTICS_GFX_ERROR_INVALID_ARGUMENT` for a NULL `out` or arena. Check or
explicitly cast to `void` if you genuinely want to ignore the failure.

Rebuild and run. A yellow circle now sits on top of the blue rectangle.

Notice that `optics_gfx_path` is a *value type* owned by the arena, not a
refcounted handle. You reset the arena every frame and the path memory
is reused. The light/heavy split is one of the foundations in
[ADR-0001](../adr/0001-project-foundations.md).

## Step 4 — animate with the transform stack

Before drawing the circle:

    double t = monotonic_seconds();  /* host clock */
    optics_gfx_canvas_save(canvas);
    optics_gfx_canvas_translate(canvas, 400, 300);
    optics_gfx_canvas_rotate(canvas, (float)t);
    optics_gfx_canvas_translate(canvas, -400, -300);

After drawing the circle:

    optics_gfx_canvas_restore(canvas);

The circle now spins around its own centre.

## Step 5 — handle surface loss on resize

The frame loop in `examples/getting_started/canvas_window.c` already shows the pattern:

    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(surface, nullptr, &frame);
    if (r == OPTICS_GFX_ERROR_SURFACE_LOST) {
        int32_t w, h;
        optics_window_get_geometry(&w, &h);
        if (w > 0 && h > 0)
            (void)optics_gfx_surface_resize(surface, (uint32_t)w, (uint32_t)h);
        continue;
    }

`OPTICS_GFX_ERROR_SURFACE_LOST` means the swapchain is out of date; resize it, skip
this frame, and try again on the next iteration.

## Step 6 — clean up

`optics_gfx_device_wait_idle`, then release in reverse creation order:

    optics_gfx_device_wait_idle(device);
    optics_gfx_canvas_release(canvas);
    optics_gfx_arena_deinit(&path_arena);
    optics_gfx_surface_release(surface);
    vkDestroySurfaceKHR(optics_gfx_device_vk_instance(device), vk_surface, nullptr);
    optics_gfx_device_release(device);

## What you now know

- The three objects: `optics_gfx_device`, `optics_gfx_surface`, `optics_gfx_canvas`.
- The per-frame envelope: `optics_gfx_surface_begin_frame` →
  `optics_gfx_canvas_begin` → draws → `optics_gfx_canvas_end` →
  `optics_gfx_frame_submit` → `optics_gfx_frame_present`.
- The arena pattern for short-lived value types.
- The transform stack (`save` / `restore` / `translate` / `rotate`).

## What's next

| Want to...                              | Read                                               |
|-----------------------------------------|----------------------------------------------------|
| Understand the frame lifecycle          | [How to record and present a frame](../how-to/record-and-present-a-frame.md) |
| See every canvas method                 | [`libs/gfx/include/optics/gfx/canvas.h`](../../libs/gfx/include/optics/gfx/canvas.h) |
