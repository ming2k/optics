# showcase examples

Composition effect demos built on flux. Unlike the per-library examples
(`examples/<library>/`), these are **not** API lessons: the effect itself is
the point — a specific visual or mathematical result achieved by composing
the stack. Each demo is still self-contained and builds with
`-Dexamples=true`:

```sh
meson setup build -Dexamples=true
meson compile -C build
./build/examples/showcase/particles_terrain
```

| Demo | Kind | Effect |
|------|------|--------|
| [`filament_plume`](#filament_plume) | visual | a flowing plume from 10,000 procedural points, one draw call |
| [`liquid_glass`](#liquid_glass) | visual | real-time backdrop blur + liquid-glass refraction |
| [`particles_terrain`](#particles_terrain) | visual | heightfield point cloud with additive glowing ridges |
| [`ripple_field`](#ripple_field) | visual, interactive | cursor-driven ripples over a 3D point field |
| [`julia_morph`](#julia_morph) | mathematical | a Julia set morphed per frame by a compute shader |
| [`liquid_glass_study`](#liquid_glass_study) | headless tool | A/B parameter harness for the liquid-glass material |

The windowed demos need a Vulkan-capable desktop and [GLFW][glfw], and
share the bootstrap + `pipeline_cache.h` helper documented in
[`../flux/README.md`](../flux/README.md). `liquid_glass` and
`liquid_glass_study` build only with `-Deffect=true`; `julia_morph` needs
`-Dcompute=true` (all default on).

[glfw]: https://www.glfw.org/

---

## filament_plume

A flowing, feather-like plume drawn from 10,000 translucent white points,
implemented as a direct shader translation of a compact p5.js formula.
`gl_VertexIndex` replaces the JavaScript loop variable, so the animation has
no vertex buffer and performs no per-point CPU work: one `vkCmdDraw` generates
the whole frame. The pipeline uses `FLUX_TOPOLOGY_POINT_LIST` and
`FLUX_BLEND_PRESET_PREMUL`; a small push-constant block supplies time,
framebuffer extent, and DPI-aware point size.

## liquid_glass

The flagship showcase, and the reference consumer of the effect module's
capture seam (ADR-0017):

1. **CAPTURE** — render a chaotic animated canvas scene into a `flux_image`
   via `flux_canvas_begin_target` / `end_target`;
2. **EFFECT** — `flux_blur_filter_apply` produces a fixed-cost frost, then
   `flux_liquid_glass_filter_apply` adds the glass: analytic SDF bodies,
   refraction, chromatic dispersion, Fresnel/glare, a spring-animated
   droplet;
3. **COMPOSITE** — the sharp capture and the transparent glass output are
   drawn back over the frame.

The blur is real backdrop blur: the glass shows whatever the canvas actually
rendered behind it, not a static stand-in texture.

## liquid_glass_study

The **headless** companion to `liquid_glass` — no window, no GLFW. It renders
a deliberately hostile backdrop (stripes, fake text rows, bright/dark zones),
composites several glass groups with argv-overridable parameters, reads the
frame back (`flux_frame_request_readback` / `flux_surface_read_pixels`), and
writes a PPM for eyeballing.

This is the designated pixel-review gate for liquid-glass shader changes
(ADR-0046, ADR-0050): run it after every edit of `effect_liquid_glass.comp`.
It also doubles as the reference for offscreen rendering + host readback.

## particles_terrain

An animated **heightfield rendered as a point cloud** — a flat plane of
particles that develops moving peaks and valleys, drawn with additive
blending so ridges accumulate into a glowing terrain with no triangle mesh.

A 256 × 256 grid (65 536 points) starts as a flat plane. Each point's `y` is
the sum of four travelling sinusoids:

```
y = 1.20·sin(0.45x + 0.80t)
  + 1.00·sin(0.50z − 0.65t)
  + 0.55·sin(0.33(x+z) + 1.10t)
  + 0.35·sin(0.70(x−z) − 1.40t)
```

Points are coloured by height — cool blue in the valleys, warm orange on the
slopes, near-white on the peaks — and the fragment shader turns each square
point into a soft disc, so overlapping points on a ridge sum toward white.
A perspective camera orbits the centre slowly. Everything is generated in the
vertex shader from `gl_VertexIndex`; the C side only supplies a
`FLUX_TOPOLOGY_POINT_LIST` + `FLUX_BLEND_PRESET_ADDITIVE` pipeline, a
caller-owned depth `flux_target`, and per-frame push constants.

## ripple_field

The interactive counterpart of `particles_terrain`: an additive point-cloud
surface where **moving the cursor emits ripples** at the corresponding point
of the field. The cursor ray is unprojected from screen space onto the
ground plane, and the ripple centres travel into the vertex shader as
uniforms.

## julia_morph

The mathematical showcase, and the compute counterpart of the visual demos:
every pixel iterates `z = z² + c` with `z` seeded at the pixel's complex
coordinate while the host slides `c` around the circle `|c| = 0.7885`.
Crossing the Mandelbrot boundary morphs the set between connected blobs,
dendrites and Cantor dust; smooth escape-time counts feed a slowly drifting
cosine palette.

It is the first example of the **compute → storage image → present** path
(`examples/flux/compute_fill` only reads an SSBO back to the CPU): a compute
pipeline writes escape-time colours into a bindless storage image, the result
is copied into a public `flux_image`, and `flux_canvas_draw_image` puts it on
the surface.
