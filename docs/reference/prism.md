# Prism Reference

prism is the material library of the optics stack: named materials —
their shaders, curve shapes, and parameter contracts — built purely on
flux's public effect runtime (compute pipelines, compute-writable images,
frame slots, the bindless heap). The boundary is structural: flux owns
rendering mechanism, prism owns material identity, and the caller owns
policy. See [ADR-0063](../adr/0063-liquid-glass-material-library.md) for
the split; the material model is
[ADR-0046](../adr/0046-liquid-glass-convex-lens-model.md) and the focus
field is [ADR-0050](../adr/0050-single-body-liquid-glass-focus-field.md).

Requires flux built with `-Dcompute=true`. Headers: `<prism/prism.h>`
(umbrella) and `<prism/liquid_glass.h>`.

## Build flag

| Flag       | Default | Effect                                                              |
|------------|---------|---------------------------------------------------------------------|
| `-Dprism`  | `true`  | Compile the prism material library. Errors out if `-Dcompute=false`. |

The pkg-config file is `prism` and declares a requirement on `flux`, so
`pkg-config --cflags --libs prism` pulls in both.

## Reusable liquid glass

`prism_liquid_glass_filter` composites analytic rounded-rectangle glass
bodies over a sharp capture and its matching blurred image. It owns one
transparent output per frame-in-flight slot. Apply it at a frame pass
boundary, then draw the borrowed output over the sharp scene:

```c
#include <prism/liquid_glass.h>

prism_liquid_glass_filter *filter = NULL;
prism_liquid_glass_filter_create(device, &filter);

prism_liquid_glass_group group = PRISM_LIQUID_GLASS_GROUP_INIT;
group.shapes[0] = (prism_liquid_glass_shape){
    .bounds = {40.0f, 30.0f, 320.0f, 96.0f},
    .corner_radius = 18.0f,
};
group.focus = (prism_liquid_glass_shape){
    .bounds = {56.0f, 42.0f, 104.0f, 72.0f},
    .corner_radius = 12.0f,
};
group.focus_strength = 1.0f;

prism_liquid_glass_desc glass = PRISM_LIQUID_GLASS_DESC_INIT;
glass.input = sharp_capture;
glass.blurred_input = blurred_capture;
glass.groups = &group;
glass.group_count = 1;

flux_image *output = NULL; /* borrowed; do not release */
prism_liquid_glass_filter_apply(filter, frame, &glass, &output);
```

All distances use capture-image pixels. `input`, `blurred_input`, and the
output have identical extents. For 8-bit SDR captures the sharp input may
be RGBA8 or BGRA8 while the blurred input and output are RGBA8; for 16F
working-space captures (ADR-0069) the sharp input is RGBA16_SFLOAT and the
blurred input and output are RGBA16_SFLOAT, so the material composites in
linear light. The output is transparent
outside the body SDF and its shadow. An empty group list clears
footprints retained by the current frame slot after all bodies disappear.

### `prism_liquid_glass_group`

| Field | Meaning |
|-------|---------|
| `shapes[0]` | Primary rounded-rectangle body; positive width and height are required |
| `shapes[1]`, `shape_count` | Optional second body smoothly unioned with the primary when `shape_count == 2` |
| `blend_radius` | Smooth-union reach in pixels |
| `opacity` | Per-body opacity multiplied by descriptor opacity |
| `shadow_alpha`, `shadow_blur`, `shadow_offset_y` | Per-body drop-shadow policy |
| `tint_color` | `0xRRGGBB` adaptive-tint multiplier; `0xFFFFFF` is neutral |
| `focus`, `focus_strength` | Optional soft optical emphasis inside the primary body; changes clarity and directional light without adding coverage, an outline, or another body |
| `frost_strength`, `tint_strength`, `saturation` | Per-body overrides of the same descriptor knobs; `< 0` inherits the descriptor value, `>= 0` is used verbatim (an explicit `0` pins the knob to zero) |
| `plate_polarity` | Caller-chosen adaptive-tint polarity in `[0, 1]`: `0` pins the smoke plate, `1` the pearl plate, uniformly for the whole body instead of per-pixel polarity; `< 0` keeps the legacy per-pixel behaviour |
| `backdrop_energy` | Region high-frequency energy in `[0, 1]`; boosts the body tint over busy backdrops (up to 2×); `< 0` disables the boost |

Groups must be built from `PRISM_LIQUID_GLASS_GROUP_INIT`: zero-init is
NOT inherit — a zeroed group pins the override fields to explicit zeros
instead of inheriting the descriptor values. The INIT macro sets all five
override/adaptive fields to their `< 0` sentinel.

A positive `focus_strength` requires `shape_count == 1`, finite positive
focus bounds contained by `shapes[0].bounds`, and a finite corner radius.
Focus and smooth union intentionally share the secondary-shape
push-constant slot and are therefore mutually exclusive. Strength is
clamped to `[0, 1]`. A zero or negative strength disables focus and
ignores its geometry.

### Per-region backdrop statistics

`prism_liquid_glass_filter_apply` also reduces each group's backdrop on the
GPU (one 16×16 workgroup per group over the primary body's clipped bounds)
into a per-slot statistics buffer:

```c
typedef struct prism_backdrop_stat {
    float mean_luminance;   /* mean Rec.709 luma of the blurred backdrop */
    float high_freq_energy; /* mean |sharp − blurred| luma */
} prism_backdrop_stat;
```

`prism_liquid_glass_filter_stats(filter, frame, out, max_groups,
out_count)` reads what the frame's slot last submitted — the apply from
`FLUX_MAX_FRAMES_IN_FLIGHT` (3) frames ago. `begin_frame` has waited that
slot's fence, so the mapped buffer is stable while `frame` records. Call
it before this frame's apply on the same slot; it returns
`FLUX_ERROR_INVALID_STATE` when the slot has never been applied. A group
whose body did not intersect the capture reads back as zeros.

These are the inputs the `plate_polarity` and `backdrop_energy` group
fields consume. prism only measures: mapping statistics onto group fields,
clamping, and temporal smoothing are caller policy (ADR-0065), which keeps
the material free of frame-to-frame state.

```c
prism_backdrop_stat stats[8];
uint32_t stat_count = 0;
if (prism_liquid_glass_filter_stats(filter, frame, stats, 8, &stat_count) == FLUX_OK) {
    for (uint32_t i = 0; i < stat_count; ++i) {
        /* Caller policy: pin the plate against the measured backdrop tone;
         * surfaces with a fixed text tone pin a constant polarity instead. */
        groups[i].plate_polarity = stats[i].mean_luminance < 0.4f ? 1.0f : 0.0f;
        groups[i].backdrop_energy = stats[i].high_freq_energy;
    }
}
```

### Descriptor policy

`refraction`, `chromatic_aberration`, `edge_width`, `rim_light`,
`light_direction`, saturation, brightness, total opacity, size scaling,
tint, and frost are dispatch-wide caller policy; saturation, tint, and
frost additionally accept per-group overrides (see the group table above).
`rim_light` scales the
entire rim-lighting set — key line, sheen, fresnel, shadow side, trough —
one knob for overall rim energy (default 0.55; named `glare` before the
move to prism). `PRISM_LIQUID_GLASS_DESC_INIT` provides the reference
neutral recipe. Prism owns the material curves; it does not infer
component policy. See
[ADR-0046](../adr/0046-liquid-glass-convex-lens-model.md) and
[ADR-0050](../adr/0050-single-body-liquid-glass-focus-field.md).

The filter, frame, and both inputs must belong to one device. Apply
requires a recording frame with no active canvas pass. Invalid
focus/merge combinations return `FLUX_ERROR_INVALID_ARGUMENT`; mismatched
input extents or formats return `FLUX_ERROR_UNSUPPORTED`.

### Filter lifetime

Each `prism_liquid_glass_filter` owns its compute pipelines. Release the
filter only after GPU work that references it has completed — its
frame-slot fence has signalled, or after `flux_device_wait_idle` —
because release destroys those pipelines inline. The borrowed output
image remains valid until the same slot is applied again, the filter is
released, or its input extent changes.

## See also

- [Effect module reference](effect.md) — the flux runtime prism builds on: blur, capture, transient outputs.
- [ADR-0063 — Liquid glass moves to the prism material library](../adr/0063-liquid-glass-material-library.md) — why the material lives outside flux.
- [ADR-0065 — Per-group material overrides and backdrop statistics](../adr/0065-per-group-overrides-and-backdrop-stats.md) — the override/adaptive group fields and the frame-lagged stats readback.
- [ADR-0046 — Liquid glass as a convex-lens material](../adr/0046-liquid-glass-convex-lens-model.md) — the material model.
- [ADR-0050 — Single-body liquid-glass focus field](../adr/0050-single-body-liquid-glass-focus-field.md) — the focus field.
