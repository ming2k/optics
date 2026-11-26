# Effect Module Reference

Image-domain effects: read one or more `optics_gfx_image` inputs, write an
`optics_gfx_image` output, record into any `VkCommandBuffer`. Header:
`<optics/gfx/effect.h>`, always part of `liboptics_gfx` and pulled in by `<optics/gfx.h>`. Design rationale: [ADR-0008](../adr/0008-image-effect-pipeline.md).

## Output ownership

| Aspect             | Detail                                                                                                         |
|--------------------|----------------------------------------------------------------------------------------------------------------|
| Allocator          | Effect-internal transient pool, per-device, keyed by `(format, width, height)`.                                |
| Lifetime           | Exclusively leased until `optics_gfx_effect_reset(device)` or device destruction.                                    |
| Bindless           | Output is registered into both SAMPLED and STORAGE bindless slots; sample with `optics_gfx_image_bindless_handle`.    |
| Layout             | `VK_IMAGE_LAYOUT_GENERAL` after the trailing barrier the effect emits.                                          |

To take a long-lived copy, use [`optics_gfx_effect_promote`](#optics_gfx_effect_promote)
below.

For an effect that updates every frame, use
[`optics_gfx_blur_filter_apply`](#reusable-frame-slot-blur) instead. It owns one
two-level pyramid and output per frame-in-flight slot and does not use
transient lease epochs.

## `optics_gfx_effect_blur`

Separable two-pass Gaussian blur.

```c
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_effect_blur(
    VkCommandBuffer cmd,
    const optics_gfx_effect_blur_desc *desc,
    optics_gfx_image **out);
```

### `optics_gfx_effect_blur_desc`

| Field    | Type             | Required | Notes                                                              |
|----------|------------------|----------|--------------------------------------------------------------------|
| `type`   | `optics_gfx_struct_type` | yes    | Must be `OPTICS_GFX_TYPE_EFFECT_BLUR_DESC`. `OPTICS_GFX_EFFECT_BLUR_DESC_INIT` sets it. |
| `next`   | `const void *`   | no       | Reserved for future chained extensions.                            |
| `input`  | `optics_gfx_image *`   | yes      | Must be RGBA8_UNORM, BGRA8_UNORM, or RGBA16_SFLOAT (ADR-0069 working space; requires rgba16f storage support). Sampled bindless handle required. |
| `sigma`  | `float`          | yes      | Gaussian sigma in pixels. Clamped silently to `[0, OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX]`. |

The output keeps the input dimensions. Its format follows the input:
`OPTICS_GFX_FORMAT_RGBA8_UNORM` for 8-bit SDR content (BGRA input remains valid
for sampling but is normalized before storage writes),
`OPTICS_GFX_FORMAT_RGBA16_SFLOAT` for 16F content, so blurring stays in linear
light.

### Returns

| Code                          | Meaning                                                                          |
|-------------------------------|----------------------------------------------------------------------------------|
| `OPTICS_GFX_OK`                     | Output image written to `*out`. Two dispatches recorded into `cmd`.              |
| `OPTICS_GFX_ERROR_INVALID_ARGUMENT` | Null `cmd` / `desc` / `out`, wrong `desc->type`, null `desc->input`, or input lacks a sampled bindless handle. |
| `OPTICS_GFX_ERROR_UNSUPPORTED`      | Input format is not RGBA8/BGRA8_UNORM or RGBA16_SFLOAT, or the device lacks rgba16f storage for a 16F input. |
| `OPTICS_GFX_ERROR_OUT_OF_MEMORY`    | Transient image allocation failed.                                               |
| `OPTICS_GFX_ERROR_BACKEND_FAILURE`  | Compute pipeline or image creation rejected by the driver. See `optics_gfx_get_last_error` for the VkResult. |

### Constants

| Symbol                          | Value | Meaning                                                  |
|---------------------------------|-------|----------------------------------------------------------|
| `OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX`    | 64.0f | Upper clamp on `desc->sigma`.                            |

### Behaviour

| `sigma` | Effect                                                                |
|---------|-----------------------------------------------------------------------|
| `0.0`   | Output is bit-exact copy of input (kernel collapses to a single sample). |
| `> 0`   | Two-pass separable Gaussian with kernel radius `ceil(3 * sigma)`, capped at 64 texels. |
| `NaN`, negative, or `> OPTICS_GFX_EFFECT_BLUR_SIGMA_MAX` | Silently clamped to the valid range. |

### Synchronisation

- The effect emits a `COMPUTE_SHADER STORAGE_WRITE → COMPUTE_SHADER | FRAGMENT_SHADER SAMPLED_READ | STORAGE_READ` image barrier on the output before returning. Callers can sample the output in any subsequent shader stage without additional synchronisation.
- The effect does **not** transition the input image. The input must already be in `SHADER_READ_ONLY_OPTIMAL` or `GENERAL` and its prior writes must be visible to compute reads.

## `optics_gfx_effect_shadow`

Drop shadow from a shape mask (ADR-0074): the input's alpha channel is the
shape coverage; the output is a premultiplied tinted shadow (`rgb = tint * a`)
of the same extent. Geometry-neutral — building the mask is the caller's job
(the canvas already draws rounded rects; material owns its own SDF path).

```c
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_effect_shadow(
    VkCommandBuffer cmd,
    const optics_gfx_effect_shadow_desc *desc,
    optics_gfx_image **out);
```

### `optics_gfx_effect_shadow_desc`

| Field       | Type             | Required | Notes                                                                 |
|-------------|------------------|----------|-----------------------------------------------------------------------|
| `type`      | `optics_gfx_struct_type` | yes    | Must be `OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC`. `OPTICS_GFX_EFFECT_SHADOW_DESC_INIT` sets it. |
| `next`      | `const void *`   | no       | Must be `NULL` (no chained extensions yet).                           |
| `input`     | `optics_gfx_image *`   | yes      | Mask image; same format rules as blur. Only the alpha channel is read. |
| `blur`      | `float`          | yes      | Gaussian sigma in pixels, clamped to `[0, OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX]`. |
| `offset_x`  | `float`          | yes      | Shadow offset in pixels; `+x` right. Sampled at `p - offset`.        |
| `offset_y`  | `float`          | yes      | Shadow offset in pixels; `+y` down.                                  |
| `tint_red` / `tint_green` / `tint_blue` | `float` | yes | Straight (unpremultiplied) colour in the input's colour space. |
| `alpha`     | `float`          | yes      | Shadow opacity, clamped to `[0, 1]`.                                  |

All float parameters must be finite; non-finite values are
`OPTICS_GFX_ERROR_INVALID_ARGUMENT`.

### Returns

| Code                          | Meaning                                                                          |
|-------------------------------|----------------------------------------------------------------------------------|
| `OPTICS_GFX_OK`                     | Output image written to `*out`. Three dispatches recorded into `cmd`.            |
| `OPTICS_GFX_ERROR_INVALID_ARGUMENT` | Null `cmd` / `desc` / `out`, wrong `desc->type`, non-null `next`, null `desc->input`, non-finite parameters, or input lacks a sampled bindless handle. |
| `OPTICS_GFX_ERROR_UNSUPPORTED`      | Input format is not RGBA8/BGRA8_UNORM or RGBA16_SFLOAT, or the device lacks rgba16f storage for a 16F input. |
| `OPTICS_GFX_ERROR_OUT_OF_MEMORY`    | Transient image allocation failed.                                               |
| `OPTICS_GFX_ERROR_BACKEND_FAILURE`  | Compute pipeline or image creation rejected by the driver.                        |

### Constants

| Symbol                          | Value | Meaning                                        |
|---------------------------------|-------|------------------------------------------------|
| `OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC`  | 40    | Descriptor tag; appended after the reserved paint block (35–39). |
| `OPTICS_GFX_EFFECT_SHADOW_BLUR_MAX`   | 64.0f | Upper clamp on `desc->blur`.                   |

### Behaviour

| `blur` | Effect                                                                 |
|--------|------------------------------------------------------------------------|
| `0.0`  | Hard offset copy of the mask alpha, tinted and premultiplied.           |
| `> 0`  | Separable two-pass Gaussian over the mask (radius `ceil(3 * blur)`), then a combine pass that samples the blurred mask at `p - offset`, tints, and premultiplies. |

Three passes of one shader (`mode` 0/1/2) over two leased intermediates and
one leased output. Dispatch shape is fixed per extent: sigma, offset, tint,
and alpha vary per frame without pipeline or allocation churn beyond the
transient pool's normal lease path (ADR-0074 animation safety).

### Synchronisation

- Same barrier contract as `optics_gfx_effect_blur`: the output carries a
  compute-write → read barrier; the input must already be sampleable.
- The leased intermediates are covered by the same barriers inside the
  recorded command buffer; callers never see them.

## Reusable frame-slot shadow

`optics_gfx_shadow_filter_apply` — the realtime counterpart of
`optics_gfx_effect_shadow`, mirroring `optics_gfx_blur_filter_apply`'s ownership model.

| Symbol | Role |
|--------|------|
| `optics_gfx_shadow_filter_create` | Create a filter (retains the device). One filter per surface/frame stream. |
| `optics_gfx_shadow_filter_retain` / `_release` | Refcounting; release frees per-slot images. |
| `optics_gfx_shadow_filter_apply` | Register the shadow as a deferred pass (ADR-0147); its dispatches are built only when something requires the output. |

The exact operator leases intermediates from the per-device transient pool
per call — correct for occasional use, but a per-frame compositor would
accumulate one lease per dispatch until `optics_gfx_effect_reset`. The filter
instead owns its two intermediates and output per frame-in-flight slot:
`begin_frame` has already waited for the selected slot before that slot is
reused, so driving it every frame grows nothing and never needs a
device-wide wait.

Accepts the same descriptor as `optics_gfx_effect_shadow` (`next` must be NULL).
Requires a recording frame at a pass boundary
(`OPTICS_GFX_ERROR_INVALID_STATE` otherwise). The returned image is borrowed from
the filter: valid until the same slot is applied again, the filter is
released, or its input extent/format changes. Do not release it.

Parameter validation (finite floats, descriptor type, input handle)
happens before the frame is dereferenced, so callers validating against a
not-yet-started frame fail cleanly with `OPTICS_GFX_ERROR_INVALID_ARGUMENT`
rather than undefined behaviour.

## Reusable frame-slot blur

`optics_gfx_blur_filter` is the fixed-cost animated compositor path. It records a
two-level Dual-Kawase pyramid instead of the exact Gaussian kernel. Create one
filter for one surface/frame stream, retain it across frames, and apply it only
at a frame pass boundary:

```c
optics_gfx_blur_filter *filter = NULL;
optics_gfx_blur_filter_create(device, &filter);

optics_gfx_effect_blur_desc blur = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
blur.input = captured_scene;
blur.sigma = 5.0f;

optics_gfx_image *output = NULL; /* borrowed; do not release */
optics_gfx_blur_filter_apply(filter, frame, &blur, &output);
```

The filter selects storage by `optics_gfx_frame_index(frame)`. Since
`optics_gfx_surface_begin_frame` has waited that slot's fence, it can overwrite the
slot without waiting for unrelated frames or growing the transient pool. The
output has the input dimensions and the input-following format (RGBA8_UNORM
for 8-bit inputs, RGBA16_SFLOAT for 16F inputs). It remains valid until the same
slot is applied again, the input extent/format changes, or the filter is
released. Positive `sigma` values control bounded sample offsets; they never
increase the four-dispatch sample count. Sigma 0 records a copy. Release the
filter only after GPU work that references it has completed.

Branded OS surface shaders (Liquid Glass, Frosted Glass, Acrylic, Mica) do not
live in Optics ([ADR-0174](../adr/0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md));
`gfx` exposes the orthogonal compute, SDF, blur, backdrop, and shadow
mechanisms so callers can compose custom effects outside the core stack.

## Deferred execution

`optics_gfx_blur_filter_apply` and `optics_gfx_shadow_filter_apply` **register** the work
rather than recording it ([ADR-0147](../adr/0147-deferred-effect-passes.md)).
The output image handle is valid the moment the call returns, but the
dispatches are only built and submitted when something requires the pixels, and
a pass nothing requires is pruned at submit without touching the GPU.

Two consequences a caller must know:

| Situation | What to do |
|-----------|------------|
| Another effect consumes the output (a material, a custom compute pass) | Nothing. The material materializes its input itself. |
| A canvas pass draws the output | Call `optics_gfx_frame_materialize_image(frame, image)` **before** `optics_gfx_canvas_begin`. Sampling a still-deferred output inside a pass is rejected with `OPTICS_GFX_ERROR_INVALID_STATE`. |

`optics_gfx_frame_materialize_image` requires a pass boundary (no canvas pass active)
and returns `OPTICS_GFX_OK` for an image that is not deferred, so callers need no
special case. `optics_gfx_frame_plan_stats` reports how many passes were registered,
executed, and pruned in the current frame.


## `optics_gfx_effect_promote`

Copy a transient effect output into a fresh caller-owned `optics_gfx_image`
with the same width, height, and format. Synchronous: records and
submits a one-shot graphics-queue command buffer and waits.

```c
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_effect_promote(
    optics_gfx_image *transient, optics_gfx_image **out);
```

### Parameters

| Parameter   | Type             | Notes                                                                |
|-------------|------------------|----------------------------------------------------------------------|
| `transient` | `optics_gfx_image *`   | Must be an image returned by a optics_gfx_effect_* operator (has a storage bindless handle). |
| `out`       | `optics_gfx_image **`  | Receives a fresh image at refcount 1, registered into the sampled bindless slot, in `SHADER_READ_ONLY_OPTIMAL`. |

### Returns

| Code                          | Meaning                                                                          |
|-------------------------------|----------------------------------------------------------------------------------|
| `OPTICS_GFX_OK`                     | Owned image written to `*out`. Release via `optics_gfx_image_release`.                 |
| `OPTICS_GFX_ERROR_INVALID_ARGUMENT` | Null `transient` / `out`, or `transient` is not an effect output (no storage handle). |
| `OPTICS_GFX_ERROR_OUT_OF_MEMORY`    | Destination image allocation failed.                                             |
| `OPTICS_GFX_ERROR_BACKEND_FAILURE`  | Vulkan command-buffer or queue submission failed.                                |

### Caller responsibilities

- Synchronise any prior submission that wrote `transient` (fence wait, `vkDeviceWaitIdle`, etc.) before calling promote. Promote does not re-synchronise.
- The transient remains valid after promote — it goes back into the effect pool in `VK_IMAGE_LAYOUT_GENERAL` and can be reused or released via `optics_gfx_effect_reset`. The promoted image is independent.

## `optics_gfx_effect_reset`

```c
OPTICS_GFX_API void optics_gfx_effect_reset(optics_gfx_device *device);
```

End the current lease epoch and return its output and intermediate slots to the
keyed pool. Old output pointers become invalid. Safe to call:

- On a device with no effect state allocated (no-op).
- Repeatedly with no calls between.
- After every command buffer referencing an effect image has completed.

Do **not** call merely because CPU recording moved to another frame. With
multiple frames in flight, wait every relevant fence or call
`optics_gfx_device_wait_idle(device)`. Recorded-but-not-submitted command buffers also
keep their referenced slots live.

## Threading

Effect calls are not thread-safe per device. Serialise per-device
calls as you would any other recording API. Concurrent calls against
different devices are independent.

## See also

- [ADR-0008 — Image-effect pipeline](../adr/0008-image-effect-pipeline.md) — design rationale.
- [`<optics/gfx/compute.h>` reference](api.md#headers) — the primitive effects build on.
- [`<optics/gfx/canvas.h>` reference](api.md#headers) — `optics_gfx_image` lifecycle.
