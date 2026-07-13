# How to apply a blur effect

Blur a `flux_image` with a separable Gaussian and either consume the
result for one frame or promote it to a long-lived image.

## Prerequisites

You have a `flux_device *d`, a `flux_image *input` in
`FLUX_FORMAT_RGBA8_UNORM` or `FLUX_FORMAT_BGRA8_UNORM`, and a
`VkCommandBuffer cmd` you are recording into (typically pulled from
`flux_frame_vk_command_buffer(frame)` inside a frame). The build
must have `-Deffect=true` (default).

## The minimum call

    #include <flux/effect.h>

    flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
    bd.input = input;
    bd.sigma = 6.0f;

    flux_image *blurred = nullptr;
    if (flux_effect_blur(cmd, &bd, &blurred) != FLUX_OK) {
        /* see flux_get_last_error */
        return;
    }

    /* Use `blurred` for the rest of this frame's recording: */
    flux_canvas_draw_image(canvas, blurred,
        (flux_rect){ 0, 0, (float)w, (float)h }, nullptr);

## Lifetime — pick one

`blurred` comes from the effect module's per-device transient pool.
It is exclusively leased and valid until `flux_effect_reset(d)` is called.

| Need                                                          | Do this                                                         |
|---------------------------------------------------------------|-----------------------------------------------------------------|
| Consume immediately and discard                               | Submit, wait for every referencing command buffer, then call `flux_effect_reset(d)`. |
| Keep across frames (cache a blurred background, prepared art) | Submit + wait, then `flux_effect_promote(blurred, &owned)`. Release with `flux_image_release(owned)`. |

The promote helper records its own one-shot graphics-queue submission
and blocks until the copy completes:

    /* After flux_frame_submit + the frame's fence wait */
    flux_image *owned = nullptr;
    if (flux_effect_promote(blurred, &owned) == FLUX_OK) {
        /* `owned` has its own bindless slot, refcount 1, regular flux_image lifecycle */
    }

## Sigma → kernel radius

| `sigma`                      | Behaviour                                                  |
|------------------------------|------------------------------------------------------------|
| `0.0`                        | Output is a bit-exact copy of the input (no kernel work).  |
| `> 0`                        | Kernel radius = `ceil(3 * sigma)`, capped at 64 texels.    |
| NaN, negative, or `> 64.0`   | Silently clamped to `[0, FLUX_EFFECT_BLUR_SIGMA_MAX]`.     |

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
compute reads. Images created via `flux_image_create` are in
`SHADER_READ_ONLY_OPTIMAL` once create returns, so the typical path
is fine.

## End-of-frame housekeeping

After all command buffers that reference this epoch's images have completed,
call `flux_effect_reset(d)` to return their leases to the pool. A CPU frame
boundary alone is not sufficient when multiple frames are in flight; waiting
the device idle is the simplest conservative choice. The pool grows only to
the high-water mark of simultaneously leased keyed images.

## See also

- [Effect module reference](../reference/effect.md) — full surface and error codes.
- [ADR-0008 — Image-effect pipeline](../adr/0008-image-effect-pipeline.md) — design rationale.
