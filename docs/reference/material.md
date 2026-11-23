# material Reference

material is the material library of the optics stack: named materials —
their shaders, curve shapes, and parameter contracts — built purely on
gfx's public effect runtime (compute pipelines, compute-writable images,
frame slots, the bindless heap). The boundary is structural: gfx owns
rendering mechanism, material owns material identity, and the caller owns
policy. See [ADR-0063](../adr/0063-liquid-glass-material-library.md) for
the split; the material model is
[ADR-0046](../adr/0046-liquid-glass-convex-lens-model.md) and the focus
field is [ADR-0050](../adr/0050-single-body-liquid-glass-focus-field.md).

Headers: `<optics/material.h>`
(umbrella), `<optics/material/liquid_glass.h>`, `<optics/material/frosted.h>`,
`<optics/material/acrylic.h>`, `<optics/material/mica.h>`, and `<optics/material/backdrop_layer.h>`.

## Build flag

| Flag       | Default | Effect                                                              |
|------------|---------|---------------------------------------------------------------------|
| `-Dprism`  | `true`  | Compile the material library.                                  |

The pkg-config file is `material` and declares a requirement on `gfx`, so
`pkg-config --cflags --libs material` pulls in both.

## Reusable liquid glass

`optics_material_liquid_glass_filter` composites analytic rounded-rectangle glass
bodies over a sharp capture and its matching blurred image. It owns one
transparent output per frame-in-flight slot. Apply it at a frame pass
boundary, then draw the borrowed output over the sharp scene:

```c
#include <optics/material/liquid_glass.h>

optics_material_liquid_glass_filter *filter = NULL;
optics_material_liquid_glass_filter_create(device, &filter);

optics_material_liquid_glass_group group = OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
group.shapes[0] = (optics_material_liquid_glass_shape){
    .bounds = {40.0f, 30.0f, 320.0f, 96.0f},
    .corner_radius = 18.0f,
};
group.focus = (optics_material_liquid_glass_shape){
    .bounds = {56.0f, 42.0f, 104.0f, 72.0f},
    .corner_radius = 12.0f,
};
group.focus_strength = 1.0f;

optics_material_liquid_glass_desc glass = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
glass.input = sharp_capture;
glass.blurred_input = blurred_capture;
glass.groups = &group;
glass.group_count = 1;

optics_gfx_image *output = NULL; /* borrowed; do not release */
optics_material_liquid_glass_filter_apply(filter, frame, &glass, &output);
```

All distances use capture-image pixels. `input`, `blurred_input`, and the
output have identical extents. For 8-bit SDR captures the sharp input may
be RGBA8 or BGRA8 while the blurred input and output are RGBA8; for 16F
working-space captures (ADR-0069) the sharp input is RGBA16_SFLOAT and the
blurred input and output are RGBA16_SFLOAT, so the material composites in
linear light. The output is transparent
outside the body SDF and its shadow. An empty group list clears
footprints retained by the current frame slot after all bodies disappear.

### `optics_material_liquid_glass_group`

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
| `curvature` | Continuous curvature blend factor in `[0, 1]`: `0` is Euclidean rounded rect, `1` is $G^2$ superellipse ($p=4$ squircle); `< 0` inherits descriptor |
| `contact_ao` | Sub-pixel contact ambient occlusion (CAO) in `[0, 1]`: razor-sharp $1.5\text{px}$ contact crease that grounds transparent glass against bright/white backdrops without muddying body tint; `< 0` inherits descriptor |
| `ambient_fresnel` | 360-degree isotropic environmental Fresnel sheen in `[0, 1]`: ensures unbroken silhouette brilliance around all corners and caps; `< 0` inherits descriptor |

Groups must be built from `OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT`: zero-init is
NOT inherit — a zeroed group pins the override fields to explicit zeros
instead of inheriting the descriptor values. The INIT macro sets all
override/physical fields to their `< 0` sentinel.

A positive `focus_strength` requires `shape_count == 1`, finite positive
focus bounds contained by `shapes[0].bounds`, and a finite corner radius.
Focus and smooth union intentionally share the secondary-shape
push-constant slot and are therefore mutually exclusive. Strength is
clamped to `[0, 1]`. A zero or negative strength disables focus and
ignores its geometry.

### Per-region backdrop statistics

`optics_material_liquid_glass_filter_apply` also reduces each group's backdrop on the
GPU (one 16×16 workgroup per group over the primary body's clipped bounds)
into a per-slot statistics buffer:

```c
typedef struct optics_material_backdrop_stat {
    float mean_luminance;   /* mean Rec.709 luma of the blurred backdrop */
    float high_freq_energy; /* mean |sharp − blurred| luma */
} optics_material_backdrop_stat;
```

`optics_material_liquid_glass_filter_stats(filter, frame, out, max_groups,
out_count)` reads what the frame's slot last submitted — the apply from
`OPTICS_GFX_MAX_FRAMES_IN_FLIGHT` (3) frames ago. `begin_frame` has waited that
slot's fence, so the mapped buffer is stable while `frame` records. Call
it before this frame's apply on the same slot; it returns
`OPTICS_GFX_ERROR_INVALID_STATE` when the slot has never been applied. A group
whose body did not intersect the capture reads back as zeros.

These backdrop statistics provide caller-level diagnostic telemetry (such as
scene classification and high-frequency detection). In the decoupled physical
model (ADR-0102), the liquid glass shader relies on sub-pixel Contact AO
(`contact_ao`) and isotropic ambient Fresnel (`ambient_fresnel`) for silhouette
separation without requiring callers to drive binary polarity latches. For
applications that do monitor backdrop statistics across frames, `optics-motion`
(`<optics/motion.h>`, ADR-0077, ADR-0106) provides temporal de-jitter primitives (`optics_motion_hysteresis_step`
and `optics_motion_smoother_step`):

```c
optics_material_backdrop_stat stats[8];
uint32_t stat_count = 0;
if (optics_material_liquid_glass_filter_stats(filter, frame, stats, 8, &stat_count) == OPTICS_GFX_OK) {
    static optics_motion_smoother energy[8];    /* per-body filter state, host-owned */
    for (uint32_t i = 0; i < stat_count; ++i) {
        /* Continuous energy tracking: τ 60 ms at rest, ×8 (≈0.5 s) while the body
         * is crossing content — fast to settle, slow to flicker. */
        float smoothed_energy =
            optics_motion_smoother_step(&energy[i], stats[i].high_freq_energy,
                               0.060f, 8.0f, 0.05f, optics_ui_dt(ui), false);
        (void)smoothed_energy;
    }
}
```

### Descriptor policy

`refraction`, `chromatic_aberration`, `edge_width`, `rim_light`,
`light_direction`, saturation, brightness, total opacity, size scaling,
tint, and frost are dispatch-wide caller policy; saturation, tint, and
frost additionally accept per-group overrides (see the group table above).
`rim_light` scales the
entire rim-lighting set — key line, sheen, fresnel, diagonal secondary catchlight, trough —
one knob for overall rim energy (default 0.55; named `glare` before the
move to material). `OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT` provides the reference
neutral recipe. material owns the material curves; it does not infer
component policy. See
[ADR-0046](../adr/0046-liquid-glass-convex-lens-model.md) and
[ADR-0050](../adr/0050-single-body-liquid-glass-focus-field.md).

The filter, frame, and both inputs must belong to one device. Apply
requires a recording frame with no active canvas pass. Invalid
focus/merge combinations return `OPTICS_GFX_ERROR_INVALID_ARGUMENT`; mismatched
input extents or formats return `OPTICS_GFX_ERROR_UNSUPPORTED`.

### Filter lifetime

Each `optics_material_liquid_glass_filter` owns its compute pipelines. Release the
filter only after GPU work that references it has completed — its
frame-slot fence has signalled, or after `optics_gfx_device_wait_idle` —
because release destroys those pipelines inline. The borrowed output
image remains valid until the same slot is applied again, the filter is
released, or its input extent changes.

## Layered backdrop (frost + glass)

`optics_material_backdrop_layer_filter` composes the two-material chrome stack in
one ordered dispatch: frosted rectangles beneath analytic liquid-glass
bodies, all nested into one persistent transparent output per frame slot.
The glass lens samples the *frosted layer image*, so a glass body bends
and frosts the frost beneath it instead of looking past it into the sharp
capture — a relation the standalone liquid-glass filter cannot express,
because its inputs are the capture and its blur and it never sees another
material's output.

```c
#include <optics/material/backdrop_layer.h>

optics_material_backdrop_layer_filter *filter = nullptr;
if (optics_material_backdrop_layer_filter_create(device, &filter) != OPTICS_GFX_OK)
    return;

optics_material_base_material base = OPTICS_MATERIAL_BASE_MATERIAL_INIT;
base.bounds = (optics_gfx_rect){0.0f, 0.0f, (float)W, (float)H};

optics_material_backdrop_layer_desc layer = OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT;
layer.input = sharp_capture;          /* the desktop capture */
layer.blurred_input = blurred_capture; /* a blur of it, same extent */
layer.base_materials = &base;
layer.base_material_count = 1;
layer.groups = groups;                /* optics_material_liquid_glass_group[] */
layer.group_count = group_count;

optics_material_output out = {}; /* image borrowed from the filter; do not release */
if (optics_material_backdrop_layer_filter_apply(
        filter, &(optics_material_backdrop_layer_filter_apply_opts){
                    .frame = frame, .desc = &layer, .out = &out}) != OPTICS_GFX_OK)
    return;
/* Draw out.image at (out.origin_x, out.origin_y): it holds the slab, not the
 * whole capture. */
```

The layer order — every base material beneath every glass body — is the
material's identity, not caller policy; glass group order within the glass
layer remains the caller's paint order, matching the standalone filter.
Glass bodies are evaluated by the same reference recipe and the same
dispatch recorder as the standalone material, so a layered body renders
identically apart from its sampled backdrop. Descriptor fields carry the
same dispatch-wide glass policy as `optics_material_liquid_glass_desc`
(`OPTICS_MATERIAL_BACKDROP_LAYER_DESC_INIT` holds the reference values).

`optics_material_base_material` is one rounded-rect write of the blurred backdrop
resolved OVER the sharp capture — the layer image is a complete opaque
background, so a lens may sample anywhere its bend reaches (including
outside every frost rect) and always reads a true colour with alpha 1.
Per-rect `opacity` in `[0, 1]` blends frosted-vs-sharp, never
frosted-vs-transparent. `tint_color`/`tint_strength` blend a wash INTO the
frost so veils and scheme-adaptive scrims live beneath the glass instead
of being painted over it by chrome. A `corner_radius` of 0 is a plain
rectangle. Input/output contract, extents, and format rules match the
liquid-glass filter (RGBA8/BGRA8 sharp input + RGBA8 blurred input, or the
16F pair per ADR-0069). The output is transparent outside every frost
rect and glass silhouette (including shadow falloff); drawing it over the
sharp desktop completes the composite. Limits: 16 frost rects and 64 glass
groups per apply.

Per-group glass statistics are submitted exactly as the standalone filter
submits them and read back with `optics_material_backdrop_layer_filter_stats` on the
same frame-slot cadence. Filter lifetime follows the same rule as
`optics_material_liquid_glass_filter`.

See
[ADR-0079](../adr/0079-layered-backdrop-material.md) for the layer-relation
rationale.

## Mica foundation material

`optics_material_mica_filter` composites screen-anchored desktop wallpaper samples,
soft Gaussian/Dual-Kawase blur, luminosity plate balancing (Pearl for light
mode, Smoke for dark mode), theme color tinting, procedural dithering, and
inactive fallback states ([ADR-0096](../adr/0096-prism-mica-foundation-material.md)):

```c
#include <optics/material/mica.h>

optics_material_mica_filter *filter = NULL;
optics_material_mica_filter_create(device, &filter);

optics_material_mica_group group = OPTICS_MATERIAL_MICA_GROUP_INIT;
group.shape = (optics_material_mica_shape){
    .bounds = {0.0f, 0.0f, 800.0f, 600.0f},
    .corner_radius = 12.0f,
};

optics_material_mica_desc mica = OPTICS_MATERIAL_MICA_DESC_INIT;
mica.wallpaper = desktop_wallpaper;
mica.blurred_wallpaper = blurred_wallpaper; /* optional, falls back to wallpaper */
mica.groups = &group;
mica.group_count = 1;
mica.kind = OPTICS_MATERIAL_MICA_BASE; /* or OPTICS_MATERIAL_MICA_ALT for commanding bars */
mica.screen_origin_x = window_x;
mica.screen_origin_y = window_y;
mica.screen_width = display_width;
mica.screen_height = display_height;

optics_gfx_image *output = NULL; /* borrowed; do not release */
optics_material_mica_filter_apply(filter, frame, &mica, &output);
```

When `screen_width` and `screen_height` are nonzero, wallpaper UV coordinates
are anchored to absolute desktop screen coordinates so moving the window
dynamically reveals the portion of the wallpaper behind it. When zero, UV coordinates
default to the local surface.

## See also

- [Effect module reference](effect.md) — the gfx runtime material builds on: blur, capture, transient outputs.
- [ADR-0063 — Liquid glass moves to the material library](../adr/0063-liquid-glass-material-library.md) — why the material lives outside gfx.
- [ADR-0065 — Per-group material overrides and backdrop statistics](../adr/0065-per-group-overrides-and-backdrop-stats.md) — the override/adaptive group fields and the frame-lagged stats readback.
- [ADR-0046 — Liquid glass as a convex-lens material](../adr/0046-liquid-glass-convex-lens-model.md) — the material model.
- [ADR-0050 — Single-body liquid-glass focus field](../adr/0050-single-body-liquid-glass-focus-field.md) — the focus field.
- [ADR-0079 — Layered backdrop material](../adr/0079-layered-backdrop-material.md) — frost beneath glass in one dispatch.
- [ADR-0096 — material Mica foundation material](../adr/0096-prism-mica-foundation-material.md) — screen-anchored wallpaper composite.
