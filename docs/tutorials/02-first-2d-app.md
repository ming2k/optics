# Your first 2D canvas application

You will write `hello_canvas.c` from scratch: a window that fills its
client area with a coloured rectangle. By the end you will understand
the three objects every flux-canvas app needs (device, surface,
canvas) and the per-frame call sequence.

## Step 1 — Start from a Known-good Skeleton

Open `examples/flux/canvas_hello.c` on a disposable branch. We will trim the
known-good example instead of starting from a blank file. The full version
draws gradients, paths, strokes, and images; you will strip those out, then
add them back.

## Step 2 — minimum viable frame

Delete everything between `flux_canvas_begin` and `flux_canvas_end`
except this:

    flux_canvas_fill_rect_color(canvas,
        (flux_rect){ 100, 100, 400, 300 },
        flux_color_rgba(60, 140, 220, 255));

Build:

    meson compile -C build && ./build/examples/flux/canvas_hello

A blue rectangle appears on a dark background. That is the whole shape
of a flux-canvas frame.

## Step 3 — add a path

Before the main loop:

    flux_arena path_arena;
    if (flux_arena_init(&path_arena, 16 * 1024, nullptr) != FLUX_OK) {
        /* see flux_get_last_error for detail */
        return 1;
    }

Inside the loop, between `flux_canvas_begin` and `flux_canvas_end`:

    flux_arena_reset(&path_arena);

    flux_path *circle = nullptr;
    if (flux_path_create(&circle, &path_arena) != FLUX_OK) continue;
    flux_path_add_circle(circle, 400, 300, 80);

    flux_paint paint = flux_paint_default();
    paint.color = flux_color_rgba_premul(255, 200, 80, 255);
    flux_canvas_fill_path(canvas, circle, &paint);

`flux_path_create` is `[[nodiscard]]` — it returns `FLUX_OK` on success,
`FLUX_ERROR_OUT_OF_MEMORY` if the arena is exhausted, or
`FLUX_ERROR_INVALID_ARGUMENT` for a NULL `out` or arena. Check or
explicitly cast to `void` if you genuinely want to ignore the failure.

Rebuild and run. A yellow circle now sits on top of the blue rectangle.

Notice that `flux_path` is a *value type* owned by the arena, not a
refcounted handle. You reset the arena every frame and the path memory
is reused. The light/heavy split is one of the foundations in
[ADR-0001](../adr/0001-project-foundations.md).

## Step 4 — animate with the transform stack

Before drawing the circle:

    double t = glfwGetTime();
    flux_canvas_save(canvas);
    flux_canvas_translate(canvas, 400, 300);
    flux_canvas_rotate(canvas, (float)t);
    flux_canvas_translate(canvas, -400, -300);

After drawing the circle:

    flux_canvas_restore(canvas);

The circle now spins around its own centre.

## Step 5 — handle surface loss on resize

The frame loop in `examples/flux/canvas_hello.c` already shows the pattern:

    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(surface, nullptr, &frame);
    if (r == FLUX_ERROR_SURFACE_LOST) {
        int w, h;
        glfwGetFramebufferSize(win, &w, &h);
        if (w > 0 && h > 0)
            (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
        continue;
    }

`FLUX_ERROR_SURFACE_LOST` means the swapchain is out of date; resize it, skip
this frame, and try again on the next iteration.

## Step 6 — clean up

`flux_device_wait_idle`, then release in reverse creation order:

    flux_device_wait_idle(device);
    flux_canvas_destroy(canvas);
    flux_arena_destroy(&path_arena);
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);

## What you now know

- The three objects: `flux_device`, `flux_surface`, `flux_canvas`.
- The per-frame envelope: `flux_surface_begin_frame` →
  `flux_canvas_begin` → draws → `flux_canvas_end` →
  `flux_frame_submit` → `flux_frame_present`.
- The arena pattern for short-lived value types.
- The transform stack (`save` / `restore` / `translate` / `rotate`).

## What's next

| Want to...                              | Read                                               |
|-----------------------------------------|----------------------------------------------------|
| Build a 3D app                          | [03 — Your first 3D scene application](03-first-3d-app.md) |
| Understand the frame lifecycle          | [How to record and present a frame](../how-to/record-and-present-a-frame.md) |
| See every canvas method                 | [`libs/flux/include/flux/canvas.h`](../../libs/flux/include/flux/canvas.h) |
