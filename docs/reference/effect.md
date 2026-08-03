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

For an effect that updates every frame, use
[`flux_blur_filter_apply`](#reusable-frame-slot-blur) instead. It owns one
two-level pyramid and output per frame-in-flight slot and does not use
transient lease epochs.

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

The output keeps the input dimensions and uses `FLUX_FORMAT_RGBA8_UNORM`.
BGRA input remains valid for sampling but is normalized before storage writes.

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

## Reusable frame-slot blur

`flux_blur_filter` is the fixed-cost animated compositor path. It records a
two-level Dual-Kawase pyramid instead of the exact Gaussian kernel. Create one
filter for one surface/frame stream, retain it across frames, and apply it only
at a frame pass boundary:

```c
flux_blur_filter *filter = NULL;
flux_blur_filter_create(device, &filter);

flux_effect_blur_desc blur = FLUX_EFFECT_BLUR_DESC_INIT;
blur.input = captured_scene;
blur.sigma = 5.0f;

flux_image *output = NULL; /* borrowed; do not release */
flux_blur_filter_apply(filter, frame, &blur, &output);
```

The filter selects storage by `flux_frame_index(frame)`. Since
`flux_surface_begin_frame` has waited that slot's fence, it can overwrite the
slot without waiting for unrelated frames or growing the transient pool. The
output has the input dimensions and `FLUX_FORMAT_RGBA8_UNORM`. It remains valid until the same
slot is applied again, the input extent/format changes, or the filter is
released. Positive `sigma` values control bounded sample offsets; they never
increase the four-dispatch sample count. Sigma 0 records a copy. Release the
filter only after GPU work that references it has completed.

## Reusable liquid glass

`flux_liquid_glass_filter` composites analytic rounded-rectangle glass bodies
over a sharp capture and its matching blurred image. It owns one transparent
output per frame-in-flight slot. Apply it at a frame pass boundary, then draw
the borrowed output over the sharp scene:

```c
flux_liquid_glass_filter *filter = NULL;
flux_liquid_glass_filter_create(device, &filter);

flux_liquid_glass_group group = FLUX_LIQUID_GLASS_GROUP_INIT;
group.shapes[0] = (flux_liquid_glass_shape){
    .bounds = {40.0f, 30.0f, 320.0f, 96.0f},
    .corner_radius = 18.0f,
};
group.focus = (flux_liquid_glass_shape){
    .bounds = {56.0f, 42.0f, 104.0f, 72.0f},
    .corner_radius = 12.0f,
};
group.focus_strength = 1.0f;

flux_liquid_glass_desc glass = FLUX_LIQUID_GLASS_DESC_INIT;
glass.input = sharp_capture;
glass.blurred_input = blurred_capture;
glass.groups = &group;
glass.group_count = 1;

flux_image *output = NULL; /* borrowed; do not release */
flux_liquid_glass_filter_apply(filter, frame, &glass, &output);
```

All distances use capture-image pixels. `input`, `blurred_input`, and the
output have identical extents; the sharp input may be RGBA8 or BGRA8, while
the blurred input and output are RGBA8. The output is transparent outside the
body SDF and its shadow. An empty group list clears footprints retained by the
current frame slot after all bodies disappear.

### `flux_liquid_glass_group`

| Field | Meaning |
|-------|---------|
| `shapes[0]` | Primary rounded-rectangle body; positive width and height are required |
| `shapes[1]`, `shape_count` | Optional second body smoothly unioned with the primary when `shape_count == 2` |
| `blend_radius` | Smooth-union reach in pixels |
| `opacity` | Per-body opacity multiplied by descriptor opacity |
| `shadow_alpha`, `shadow_blur`, `shadow_offset_y` | Per-body drop-shadow policy |
| `tint_color` | `0xRRGGBB` adaptive-tint multiplier; `0xFFFFFF` is neutral |
| `focus`, `focus_strength` | Optional soft optical emphasis inside the primary body; changes clarity and directional light without adding coverage, an outline, or another body |

A positive `focus_strength` requires `shape_count == 1`, finite positive focus
bounds contained by `shapes[0].bounds`, and a finite corner radius. Focus and
smooth union intentionally share the secondary-shape push-constant slot and
are therefore mutually exclusive. Strength is clamped to `[0, 1]`. A zero or
negative strength disables focus and ignores its geometry.

### Descriptor policy

`refraction`, `chromatic_aberration`, `edge_width`, `glare`,
`light_direction`, saturation, brightness, total opacity, size scaling, tint,
and frost are dispatch-wide caller policy. `FLUX_LIQUID_GLASS_DESC_INIT`
provides the reference neutral recipe. Flux owns the effect mechanism and
material curves; it does not infer component policy. See
[ADR-0047](../adr/0047-caller-owned-policy-boundary-for-flux-effects.md) and
[ADR-0050](../adr/0050-single-body-liquid-glass-focus-field.md).

The filter, frame, and both inputs must belong to one device. Apply requires a
recording frame with no active canvas pass. Invalid focus/merge combinations
return `FLUX_ERROR_INVALID_ARGUMENT`; mismatched input extents or formats
return `FLUX_ERROR_UNSUPPORTED`.

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
- [`<flux/compute.h>` reference](api.md#headers) — the primitive effects build on.
- [`<flux/canvas.h>` reference](api.md#headers) — `flux_image` lifecycle.
