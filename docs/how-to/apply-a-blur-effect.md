# How to apply a blur effect

Blur a `optics_gfx_image` with an exact separable Gaussian and either consume
the result for one frame or promote it to a long-lived image.

For a live backdrop that changes every frame, prefer a reusable
`optics_gfx_blur_filter`. It uses a fixed-cost two-level Dual-Kawase pyramid,
isolates storage per frame-in-flight slot, and avoids transient-pool
growth, device-wide waits, and sigma-dependent shader loops.

## Prerequisites

You have a `optics_gfx_device *d`, a `optics_gfx_image *input` in
`OPTICS_GFX_FORMAT_RGBA8_UNORM` or `OPTICS_GFX_FORMAT_BGRA8_UNORM`, and a
`VkCommandBuffer cmd` you are recording into (typically pulled from
`optics_gfx_frame_vk_command_buffer(frame)` inside a frame).

## The minimum call

    #include <optics/gfx/effect.h>

    optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
    bd.input = input;
    bd.sigma = 6.0f;

    optics_gfx_image *blurred = nullptr;
    if (optics_gfx_effect_blur(cmd, &bd, &blurred) != OPTICS_GFX_OK) {
        /* see optics_gfx_get_last_error */
        return;
    }

    /* Use `blurred` for the rest of this frame's recording: */
    optics_gfx_canvas_draw_image(canvas, blurred,
        (optics_gfx_rect){ 0, 0, (float)w, (float)h }, nullptr);

## Live compositor path

Create the filter once, then apply it with the recording `optics_gfx_frame`:

    optics_gfx_blur_filter *filter = NULL;
    optics_gfx_blur_filter_create(d, &filter);

    /* Per frame, outside a render pass: */
    optics_gfx_image *blurred = NULL; /* borrowed from filter */
    optics_gfx_blur_filter_apply(filter, frame, &bd, &blurred);
    optics_gfx_canvas_draw_image(canvas, blurred,
        (optics_gfx_rect){ 0, 0, (float)w, (float)h }, NULL);

Keep one filter per surface/frame stream. Release it after the last referencing
frame has completed. The output keeps the input dimensions and uses
`OPTICS_GFX_FORMAT_RGBA8_UNORM`. For
backdrop blur, render the scene into a quarter-resolution
`optics_gfx_image_create (OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET)` and scale `sigma` by the same factor.
The filter always records two downsample and two upsample passes; `sigma`
changes sample offsets without changing the sample count.

## Exact Gaussian output lifetime

The image returned by `optics_gfx_effect_blur` comes from the effect module's
per-device transient pool. It is exclusively leased and valid until
`optics_gfx_effect_reset(d)` is called. The live filter output instead follows the
frame-slot lifetime described above.

| Need                                                          | Do this                                                         |
|---------------------------------------------------------------|-----------------------------------------------------------------|
| Consume immediately and discard                               | Submit, wait for every referencing command buffer, then call `optics_gfx_effect_reset(d)`. |
| Keep across frames (cache a blurred background, prepared art) | Submit + wait, then `optics_gfx_effect_promote(blurred, &owned)`. Release with `optics_gfx_image_release(owned)`. |

The promote helper records its own one-shot graphics-queue submission
and blocks until the copy completes:

    /* After optics_gfx_frame_submit + the frame's fence wait */
    optics_gfx_image *owned = nullptr;
    if (optics_gfx_effect_promote(blurred, &owned) == OPTICS_GFX_OK) {
        /* `owned` has its own bindless slot, refcount 1, regular optics_gfx_image lifecycle */
    }

## Exact Gaussian sigma → kernel radius

This table applies to `optics_gfx_effect_blur`. The reusable live filter clamps the
same input range but maps positive sigma values to bounded pyramid offsets.

| `sigma`                      | Behaviour                                                  |
|------------------------------|------------------------------------------------------------|
| `0.0`                        | Output is a bit-exact copy of the input (no kernel work).  |
| `> 0`                        | Kernel radius = `ceil(3 * sigma)`, capped at 64 texels.    |
| NaN, negative, or `> 64.0`   | Silently clamped to `[0, OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX]`.     |

## What the call records into `cmd`

1. **Pass 1 (horizontal)** — compute dispatch reading `input`,
   writing an internal cached intermediate.
2. **Compute-write → compute-read barrier** on the intermediate.
3. **Pass 2 (vertical)** — compute dispatch reading the intermediate,
   writing `blurred`.
4. **Compute-write → shader-read barrier** on `blurred`. Any later
   shader stage (canvas draw, another effect, …) can sample
   `blurred` without additional synchronisation.

The input image is **not** transitioned — it must already be in
`SHADER_READ_ONLY_OPTIMAL` or `GENERAL` with prior writes visible to
compute reads. Images created via `optics_gfx_image_create` are in
`SHADER_READ_ONLY_OPTIMAL` once create returns, so the typical path
is fine.

## End-of-frame housekeeping

After all command buffers that reference this epoch's images have completed,
call `optics_gfx_effect_reset(d)` to return their leases to the pool. A CPU frame
boundary alone is not sufficient when multiple frames are in flight; waiting
the device idle is the simplest conservative choice. The pool grows only to
the high-water mark of simultaneously leased keyed images.

## See also

- [Effect module reference](../reference/effect.md) — full surface and error codes.
- [ADR-0008 — Image-effect pipeline](../adr/0008-image-effect-pipeline.md) — design rationale.
