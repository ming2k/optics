---
id: ADR-0112
title: "Device-Resolution Icon Coverage Masks and Explicit Supersampling for the Software Rasteriser"
status: accepted
date: 2026-10-23
scope: libs/flux, libs/lens
superseded_by: null
negative_knowledge: true
---

# ADR-0112: Device-Resolution Icon Coverage Masks and Explicit Supersampling for the Software Rasteriser

- Status: Accepted
- Date: 2026-10-23
- Scope: `libs/flux` (`include/flux/canvas.h`, `include/flux/canvas_cpu.h`, `include/flux/core.h`, `src/canvas/backend.h`, `src/canvas/backend_cpu.c`, `src/canvas/canvas.c`, `src/canvas/internal.h`), `libs/lens` (`src/render/icon_mask.{h,c}`, `src/render/replay.c`, `src/internal.h`, `src/core/context.c`, `meson.build`).
- Amends: [ADR-0071](0071-pass-scoped-canvas-antialiasing.md) (AA policy gains a coverage-resolution axis).
- Extends: [ADR-0010](0010-glyph-blit-primitive.md), [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md), [ADR-0111](0111-lens-runtime-svg-mixed-paint-runs-and-orthogonal-icon-sizing.md).

## Context

Icons in `lens` are vector command streams in a 24×24 box. At the sizes real UI
uses them — 14 to 24 logical pixels — they were visibly stair-stepped, badly
enough that a downstream consumer reorganised its own artwork to compensate.

Measuring rather than guessing produced four findings.

**1. The dominant cause is the coverage budget, not the geometry.** The canvas
pins its attachment at 4× MSAA (`FLUX_CANVAS_SAMPLES`), and a resolved 4× pixel
carries coverage only in `{0, ¼, ½, ¾, 1}`. Rendered through the real pipeline,
a 14 px `chevron_right` (1.17 px stroke) produced exactly **two** distinct partial
alpha values on its edges; `check` at 14 px produced two; `alert_circle`
produced three. The same geometry rasterised at 64 samples per pixel yields 7–16
levels. The shape was never wrong — five coverage levels simply cannot describe a
thin diagonal.

Arbitrary paths cannot borrow the analytic anti-aliasing that rounded rects and
circles get from `canvas_sdf.frag`; that path is an SDF over a known primitive.
An icon is a general path, so it lives or dies by sample count.

**2. Two placement bugs made it worse at fractional scale.** Command replay
snapped every command rect to the device grid (`snap_rect`), then the icon branch
rounded the origin again in *logical* space (`roundf(r.x)`). At `scale = 1.25`
that turned a device-aligned origin of 1.50 px back into 1.25 px — off the
device grid, i.e. deliberately resampled. The two rounding rules contradicted
each other.

**3. Stroke width was never device-quantised.** Every skin derives it as
`2 * (icon_size / 24)`. At 14 px that is 1.167 device px: a sub-1.5 px line,
which is precisely the regime where 4× MSAA's three partial levels become visible
banding. Measured peak alpha for a 14 px chevron was **191/255** — the stroke
never reached full opacity.

**4. Rebuilding the coverage budget per frame is the wrong shape entirely.** An
icon's geometry is static, its device size is static for a given widget, and the
colour is the only per-frame variable. Re-rasterising triangles and
re-quantising coverage every frame spends per-frame work to arrive at an answer
that was already known, and still lands on five levels.

The lesson is the negative knowledge this ADR exists to record: **adding MSAA
samples cannot fix a bake-once artefact, and re-deriving static coverage every
frame is both slower and no better.**

## Decision

Give the icon pipeline a **device-resolution coverage mask** cache, and give the
software rasteriser an **explicit supersampling factor** so those masks can be
baked with enough grey levels to be worth caching.

### flux — the coverage primitive

1. `flux_canvas_supersample_desc` (pNext on `flux_canvas_desc`, tag
   `FLUX_TYPE_CANVAS_SUPERSAMPLE_DESC = 34`) sets the software backend's
   supersampling factor per axis. `0` keeps the default (2); the range is
   `1..FLUX_CANVAS_SUPERSAMPLE_MAX` (8). Out of range fails creation with
   `FLUX_ERROR_OUT_OF_RANGE` rather than clamping — a caller must never believe
   it received more fidelity than the buffer holds. `flux_canvas_create_cpu_ss`
   is the direct constructor. GPU backends ignore it: their coverage comes from a
   per-pass MSAA attachment, not from a buffer size.

   There is exactly **one** software-backend quality concept: the supersample
   factor. `flux_canvas_antialias_desc` (a pre-existing create-time tag) is kept
   only as the legacy spelling of "1 sample versus the default", and it maps onto
   the same factor rather than introducing a second, competing knob; the GPU-side
   `flux_canvas_pass_desc::antialias` remains per-pass MSAA policy and is a
   different thing entirely.
2. `flux_canvas_cpu_coverage` returns a single-channel coverage plane: one 8-bit
   value per output pixel, the box-filtered mean of the supersampled alpha (the
   same resolve `flux_canvas_cpu_pixels` performs). N samples give N+1 coverage
   levels, so the number of grey levels is a property of the requested factor,
   not of 8-bit quantisation. This is a new nullable backend vtable slot
   (`read_coverage`); the Vulkan backend leaves it NULL.
3. `flux_glyph_run_desc` no longer treats `atlas` and `host_coverage` as mutually
   exclusive. A run may carry both, and the canvas backend selects the one it can
   actually sample. The old contract rejected the dual payload with
   `FLUX_ERROR_INVALID_ARGUMENT` on a GPU canvas — which made "one compiled
   display list, either backend" impossible and, worse, failed *silently* for void
   draw entry points (see defect 2 below).

### lens — the mask cache

4. `libs/lens/src/render/icon_mask.c` bakes one mask per
   `(icon, device box size, device stroke width, outline width, paint run,
   layer)` into a device-free canvas at **8×8 supersampling** and keeps the
   resulting coverage plane. 65 levels per pixel is beyond what an 8-bit edge can
   distinguish.
5. Masks are keyed and stored at **device** resolution, and the blit is **1:1**:
   integer device-pixel origin and extent, NEAREST sampler. A mask is never
   resampled. This is the correctness condition of the whole design — filtering a
   mask reintroduces exactly the blur it exists to remove.
6. **Body and outline are separate layers** (`LENSI_ICON_MASK_BODY` /
   `LENSI_ICON_MASK_OUTLINE`) and are blitted outline-first, body-over. They are
   painted in different colours, so a single union mask would force both to one
   ink and silently drop the outline colour entirely.
7. Replay resolves device geometry *before* anything else: box edge
   `roundf(icon_dim * scale)` and origin `floorf(r.x * scale + 0.5)` — rounding in
   device space, which is the rule `snap_rect` already uses and the one the
   logical-space `roundf` violated.
8. The pool is bounded (96 entries) and LRU-evicted. Eviction is safe for
   published snapshots because every recorded glyph-run segment retains the image
   it draws (`flux_encoder_draw_glyph_run` → `flux_image_retain`).
9. A failed bake (allocation, oversized box, absent pool) falls back to the
   previous live-vector path as a whole. A partially masked icon would mix two
   rasterisations inside one glyph, so the fallback is all-or-nothing and is
   probed before any blit is emitted. **The fallback renders runs, not the whole
   stream**: it consumes the same `lensi_icon_run_spans` output as the bake, so it
   preserves per-run colour, per-run fill/stroke, and the outline underlay at
   lower coverage rather than at lower fidelity of content.

## Alternatives Considered

- **Raise the canvas MSAA to 8× or 16×.** Wrong axis, and measurably so. MSAA
  affects the *display* pass, which is re-resolved every frame, while an icon's
  coverage quantisation is the same every frame. It multiplies bandwidth and
  memory for every draw in the canvas (including text, image blits, and effects)
  to buy a handful of extra levels on a handful of small shapes. It also cannot
  reach the levels a baked mask carries: 4× gives 5 levels, 8× gives 9, and the
  bake gets 65 for free once per icon.
- **Analytic path coverage in the fragment shader (pathfinder-style).** Genuinely
  correct and resolution-independent, but it is a rasteriser — a new pipeline, a
  new per-draw GPU representation of every path, and a large blast radius across
  `canvas.c`, the tessellator, and the pipeline cache. It also spends per-frame
  GPU work to recompute what is static. Out of proportion for the symptom, and it
  would not compose with the existing stencil-then-cover fallback.
- **Render icons into a core glyph-atlas-style R8 atlas and blit glyphs.** This is
  the same 1:1 blit the chosen design uses, but routed into `glyph`'s atlas, which
  would make a text module own icon art, couple lens's icon cache to glyph's
  eviction policy, and drag text shaping into icon rendering. The mask pool is
  ~150 lines and owns exactly one concern.
- **Snap icon vertices and stroke widths to the pixel grid.** Tempting, cheap, and
  measured: it made fidelity *worse*. Snapping the stroke width destroyed the
  vector's intended proportion (mean absolute error against a 16× area reference
  rose from 6.68 to 29.29 on `alert_circle`), and snapping origin only helped
  where the origin was already off-grid — which fix 6 addresses directly, without
  touching the art. Grid-snapping trades a shape error for a coverage error; a
  baked mask avoids both.
- **Bake masks on the GPU with a render-to-texture pass.** Avoids the CPU bake,
  but adds an offscreen pass, a resolve, and a readback-or-bind lifecycle to the
  frame, and cannot run at all in the headless/device-less configuration the CPU
  backend exists for. The CPU bake is one-off per icon (3–4 ms cold, ~0.02 ms
  steady state per frame for three icons) and works identically on both backends.

## Consequences

Positive:

- Icon edges carry tens of distinct coverage levels instead of two to four.
  Measured through the public snapshot API on a CPU canvas, distinct partial
  levels went from 2/2/3/3 to 12/14/19/16 for `check`/`alert_circle`/`activity`
  at 14–24 px, and 20/19/28/28 for the filled star.
- Coverage is computed once per (icon, size) instead of every frame. Icons no
  longer allocate path scratch from the frame arena, which removes the dominant
  per-frame allocation for icon-heavy UIs.
- Placement is exact: measured coverage centroids sit within 0.05 px of the
  geometric centre at scale 1.0, 1.25, 1.5, and 2.0.
- The device-scale mismatch at fractional scale is fixed at its root (device-space
  rounding, device-quantised stroke) rather than compensated for.
- One compiled display list replays into either backend. Every mask keeps its
  host coverage bytes *and*, on a device-backed context, an R8 image — so the
  same `lens_scene_snapshot` renders into a GPU canvas and into a device-less CPU
  canvas (headless snapshot, offscreen export) without branching at the call site
  and without baking twice. This required relaxing the glyph-run primitive: a run
  may now carry both an `atlas` and a `host_coverage`, and the canvas backend
  picks its native source instead of rejecting the dual payload (see the negative
  knowledge below for why the old mutual-exclusion contract was a trap).

Negative, and accepted:

- Cold cost: the first frame that shows an icon pays a bake plus an upload
  (3–4 ms for three icons), and on the GPU a mid-pass `flux_image_update_region`
  is a one-shot submit-and-wait, i.e. a pipeline stall. It matches the pattern
  `glyph` already uses (`txt_atlas_flush` before a run draws), it is one-off per
  cache entry, and steady state measures ~0.02–0.07 ms per frame of recording.
- Memory: a 24 px mask at 8× supersampling uses a 192×192 float RGBA bake buffer
  transiently and keeps one byte per pixel (≈1 KiB) resident. The pool is capped
  at 96 entries, so the cache is bounded at a few hundred KiB per context.
- Masks are resolution- and stroke-specific. Changing `icon_size`, `scale`, the
  stroke width, or the outline width produces a new entry; a UI that animates icon
  size continuously would churn the pool. The LRU bound plus the quantised key
  keep that bounded and fall back gracefully rather than growing.
- Icons above `LENSI_ICON_MASK_MAX_PX` (256 device px) are not baked. At that size
  the vector path is both cheap and already high-resolution, and the 8× bake
  buffer would be large.

Invariants this ADR establishes:

- A mask blit MUST be 1:1: integer device origin/extent and a NEAREST sampler. A
  filtered or fractional mask blit is a bug, not a tuning choice.
- Device geometry is rounded in device space (`roundf(v * scale)`), never in
  logical space followed by scaling.
- Body and outline MUST remain separate mask layers; merging them loses the
  outline colour.
- `FLUX_CANVAS_SUPERSAMPLE_MAX` is a memory bound: raising it raises the bake
  buffer quadratically.
- An icon's paint runs are resolved by exactly one function
  (`lensi_icon_run_spans`). The mask path and the vector fallback both consume its
  spans, so they cannot disagree about run count, run boundaries, colour, or
  fill/stroke mode.

## Negative knowledge (defects this ADR's own implementation produced)

Recorded deliberately: each of these was live code that passed the test suite, and
each was found by auditing the implementation rather than by a failing test.

1. **The vector fallback collapsed the run table.** Replacing the live path with
   masks left the fallback iterating the *whole* command stream once and choosing
   fill-vs-stroke from `lensi_icon_mode()`. That is precisely the bug ADR-0111
   fixed: any runtime SVG with per-shape colours rendered every shape in the theme
   colour. Measured before the fix: three declared runs (red/green/blue) produced
   `red=0 green=0 blue=0` and ~39 600 theme-coloured pixels. The root cause was
   *duplication* — the bake had its own run resolution and the fallback had
   another. The fix is structural: one resolver, one emitter, both paths consume
   them.
2. **A device-backed context could not render into a device-less canvas.** Mask
   representation was chosen by `ui->device`, while the *consumer* is the canvas.
   `lens_create(device)` + `flux_canvas_create_cpu()` produced a completely blank
   icon — and because the glyph-run primitive treated `atlas` and `host_coverage`
   as mutually exclusive, it failed silently. Two lessons: pick representation by
   consumer (so masks keep both), and never let a draw primitive reject a payload
   it merely cannot use yet.
3. **Dead code contradicted the live code.** A `lensi_icon_mask_pad()` helper in
   the header computed a halo from `ceil(stroke/2)+1` while the bake used
   `ceil((stroke + 2·outline)/2)+1`; it was never called, so the two could drift
   silently and a future caller would misplace every blit. Deleted.

The meta-lesson: a cache that re-implements an existing rendering path needs the
old path to be *re-expressed in terms of* the new one's primitives, not left
alongside it. Leaving it alongside is how a fixed bug comes back.

Follow-up work not taken here: baking masks for the tab-strip/close affordances at
their non-integer stroke widths is covered by the same cache, but a UI that
animates icon size should quantise its steps. A GPU-side bake could remove the
cold-frame stall if icon churn ever shows up in a profile.

## References

- [ADR-0071](0071-pass-scoped-canvas-antialiasing.md) — pass-scoped AA policy this
  amends with a coverage-resolution axis.
- [ADR-0019](0019-canvas-backend-seam-and-cpu-backend.md) — the software backend
  and its host-coverage path.
- [ADR-0010](0010-glyph-blit-primitive.md) — the batched blit primitive masks reuse.
- [ADR-0111](0111-lens-runtime-svg-mixed-paint-runs-and-orthogonal-icon-sizing.md) —
  the icon paint-run model masks are keyed by, and the sizing change that first
  surfaced the aliasing.
- [ADR-0076](0076-lens-icon-paint-runs.md) — per-shape paint runs.
