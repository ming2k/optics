# Effect Module Reference

Image-domain effects: read one or more `flux_image` inputs, write a
`flux_image` output, record into any `VkCommandBuffer`. Requires
`-Dcanvas=true -Dcompute=true -Deffect=true`. Header:
`<flux/effect.h>`. Design rationale: [ADR-0008](../adr/0008-image-effect-pipeline.md).

## Build flag

| Flag         | Default | Effect                                                |
|--------------|---------|-------------------------------------------------------|
| `-Deffect`   | `true`  | Compile the effect module. Errors out if `-Dcanvas=false` or `-Dcompute=false`. |

When enabled, defines `FLUX_HAVE_EFFECT=1` on the public compile line
and installs `<flux/effect.h>`. The umbrella `<flux/flux.h>` then pulls
the effect header in automatically.

## Output ownership

| Aspect             | Detail                                                                                                         |
|--------------------|----------------------------------------------------------------------------------------------------------------|
| Allocator          | Effect-internal transient pool, per-device, keyed by `(format, width, height)`.                                |
| Lifetime           | Exclusively leased until `flux_effect_reset(device)` or device destruction.                                    |
| Bindless           | Output is registered into both SAMPLED and STORAGE bindless slots; sample with `flux_image_bindless_handle`.    |
| Layout             | `VK_IMAGE_LAYOUT_GENERAL` after the trailing barrier the effect emits.                                          |

To take a long-lived copy, use [`flux_effect_promote`](#flux_effect_promote)
below.

## `flux_effect_blur`

Separable two-pass Gaussian blur.

```c
FLUX_NODISCARD FLUX_API flux_result flux_effect_blur(
    VkCommandBuffer cmd,
    const flux_effect_blur_desc *desc,
    flux_image **out);
```

### `flux_effect_blur_desc`

| Field    | Type             | Required | Notes                                                              |
|----------|------------------|----------|--------------------------------------------------------------------|
| `type`   | `flux_struct_type` | yes    | Must be `FLUX_TYPE_EFFECT_BLUR_DESC`. `FLUX_EFFECT_BLUR_DESC_INIT` sets it. |
| `next`   | `const void *`   | no       | Reserved for future chained extensions.                            |
| `input`  | `flux_image *`   | yes      | Must be RGBA8_UNORM or BGRA8_UNORM. Sampled bindless handle required. |
| `sigma`  | `float`          | yes      | Gaussian sigma in pixels. Clamped silently to `[0, FLUX_EFFECT_BLUR_SIGMA_MAX]`. |

### Returns

| Code                          | Meaning                                                                          |
|-------------------------------|----------------------------------------------------------------------------------|
| `FLUX_OK`                     | Output image written to `*out`. Two dispatches recorded into `cmd`.              |
| `FLUX_ERROR_INVALID_ARGUMENT` | Null `cmd` / `desc` / `out`, wrong `desc->type`, null `desc->input`, or input lacks a sampled bindless handle. |
| `FLUX_ERROR_UNSUPPORTED`      | Input format is not RGBA8_UNORM or BGRA8_UNORM.                                  |
| `FLUX_ERROR_OUT_OF_MEMORY`    | Transient image allocation failed.                                               |
| `FLUX_ERROR_BACKEND_FAILURE`  | Compute pipeline or image creation rejected by the driver. See `flux_get_last_error` for the VkResult. |

### Constants

| Symbol                          | Value | Meaning                                                  |
|---------------------------------|-------|----------------------------------------------------------|
| `FLUX_EFFECT_BLUR_SIGMA_MAX`    | 64.0f | Upper clamp on `desc->sigma`.                            |

### Behaviour

| `sigma` | Effect                                                                |
|---------|-----------------------------------------------------------------------|
| `0.0`   | Output is bit-exact copy of input (kernel collapses to a single sample). |
| `> 0`   | Two-pass separable Gaussian with kernel radius `ceil(3 * sigma)`, capped at 64 texels. |
| `NaN`, negative, or `> FLUX_EFFECT_BLUR_SIGMA_MAX` | Silently clamped to the valid range. |

### Synchronisation

- The effect emits a `COMPUTE_SHADER STORAGE_WRITE → COMPUTE_SHADER | FRAGMENT_SHADER SAMPLED_READ | STORAGE_READ` image barrier on the output before returning. Callers can sample the output in any subsequent shader stage without additional synchronisation.
- The effect does **not** transition the input image. The input must already be in `SHADER_READ_ONLY_OPTIMAL` or `GENERAL` and its prior writes must be visible to compute reads.

## `flux_effect_promote`

Copy a transient effect output into a fresh caller-owned `flux_image`
with the same width, height, and format. Synchronous: records and
submits a one-shot graphics-queue command buffer and waits.

```c
FLUX_NODISCARD FLUX_API flux_result flux_effect_promote(
    flux_image *transient, flux_image **out);
```

### Parameters

| Parameter   | Type             | Notes                                                                |
|-------------|------------------|----------------------------------------------------------------------|
| `transient` | `flux_image *`   | Must be an image returned by a flux_effect_* operator (has a storage bindless handle). |
| `out`       | `flux_image **`  | Receives a fresh image at refcount 1, registered into the sampled bindless slot, in `SHADER_READ_ONLY_OPTIMAL`. |

### Returns

| Code                          | Meaning                                                                          |
|-------------------------------|----------------------------------------------------------------------------------|
| `FLUX_OK`                     | Owned image written to `*out`. Release via `flux_image_release`.                 |
| `FLUX_ERROR_INVALID_ARGUMENT` | Null `transient` / `out`, or `transient` is not an effect output (no storage handle). |
| `FLUX_ERROR_OUT_OF_MEMORY`    | Destination image allocation failed.                                             |
| `FLUX_ERROR_BACKEND_FAILURE`  | Vulkan command-buffer or queue submission failed.                                |

### Caller responsibilities

- Synchronise any prior submission that wrote `transient` (fence wait, `vkDeviceWaitIdle`, etc.) before calling promote. Promote does not re-synchronise.
- The transient remains valid after promote — it goes back into the effect pool in `VK_IMAGE_LAYOUT_GENERAL` and can be reused or released via `flux_effect_reset`. The promoted image is independent.

## `flux_effect_reset`

```c
FLUX_API void flux_effect_reset(flux_device *device);
```

End the current lease epoch and return its output and intermediate slots to the
keyed pool. Old output pointers become invalid. Safe to call:

- On a device with no effect state allocated (no-op).
- Repeatedly with no calls between.
- After every command buffer referencing an effect image has completed.

Do **not** call merely because CPU recording moved to another frame. With
multiple frames in flight, wait every relevant fence or call
`flux_device_wait_idle(device)`. Recorded-but-not-submitted command buffers also
keep their referenced slots live.

## Threading

Effect calls are not thread-safe per device. Serialise per-device
calls as you would any other recording API. Concurrent calls against
different devices are independent.

## See also

- [ADR-0008 — Image-effect pipeline](../adr/0008-image-effect-pipeline.md) — design rationale.
- [`<flux/compute.h>` reference](api.md#compute) — the primitive effects build on.
- [`<flux/canvas.h>` reference](api.md#canvas) — `flux_image` lifecycle.
