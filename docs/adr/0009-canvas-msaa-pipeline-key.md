# ADR-0009: Canvas sample count joins the pipeline-cache key

- Status: Superseded by ADR-0071
- Date: 2026-05-24

## Context

Every canvas pipeline is currently created with
`rasterizationSamples = VK_SAMPLE_COUNT_1_BIT` (`src/canvas/renderer.c`,
in the multisample-state block of `get_canvas_pipeline`). One-sample
rasterisation is fast and matches a no-AA contract, but it produces
visible staircasing on every non-axis-aligned edge the canvas
emits — stroked diagonals, rounded-rect corners, circle outlines,
the boundary of any rotated fill. The smoke tests and `canvas_hello`
do not catch this because they render axis-aligned rectangles
through `flux_canvas_fill_rect`, which is pixel-snapped.

The pain is real and downstream: consumers building UI on top of
flux (the originating motivation for the canvas in the first place)
expect their rounded-corner shadows and thin strokes to render
without jaggies. They have three options today, none good:

1. Accept the staircase.
2. Render into a 2× backing surface and downsample — pushes the
   responsibility outside flux, doubles backing-store bandwidth.
3. Take the canvas's `VkRenderPass`-equivalent objects via the
   raw-Vulkan accessor seams (ADR-0006 keeps these open) and build
   their own multi-sample pipelines around the same shaders. This
   leaks every pipeline-cache invariant flux currently guards.

Two alternative AA strategies were proposed for review:

- **SDF fast-path for known primitives.** Solves rect / round-rect /
  circle, but the bulk of jaggies in real UI render through
  `stroke_path` and `fill_path` on arbitrary user-supplied curves
  — those paths don't get an SDF representation without a
  separate rasteriser, and at that point we have written
  pathfinder.
- **`VK_EXT_conservative_rasterization`.** Wrong tool: it produces
  over-coverage, not anti-aliasing, and it is an optional
  extension that consumers wanting AA cannot rely on.

That leaves hardware MSAA. The cost is well-understood: a 4×
colour attachment doubles bandwidth on desktop discrete GPUs and is
absorbed nearly free on tilers (the multi-sample buffer lives in
on-chip tile memory and never round-trips to main memory). Per-pixel
ops cost the same because shading is per-pixel — only depth/colour
storage scale with sample count.

Crucially, MSAA's contract intersects ADR-0004's pipeline cache. The
cache is keyed by `(color_format, paint_kind)`, but a pipeline's
multisample state is part of its compatibility with the framebuffer
attachment. A canvas configured for 4× MSAA cannot bind a 1×
pipeline. So this ADR amends the cache key.

## Decision

Add `sample_count` to `flux_canvas_desc`, default `1`, accepted
values: `1`, `2`, `4`, `8` (clamped at runtime to what
`VkPhysicalDeviceLimits.framebufferColorSampleCounts` reports for
the surface's format).

```c
typedef struct flux_canvas_desc {
    flux_struct_type type;         /* FLUX_TYPE_CANVAS_DESC */
    const void      *next;
    flux_surface    *surface;
    uint32_t         sample_count; /* 1 | 2 | 4 | 8; 1 = no MSAA */
} flux_canvas_desc;
```

Pipeline-cache key becomes `(color_format, paint_kind, sample_count)`.
The cache stays per-device — `get_canvas_pipeline` grows the extra
discriminator. ADR-0004's "paint kind drives pipeline selection"
holds; this is an amendment to its cache key, not a contradiction.

The canvas allocates a single multi-sample colour image at
`flux_canvas_create` time when `sample_count > 1`, sized to the
surface, and configures the pass to resolve into the swapchain
image. The MSAA image lives in the device's slab allocator (ADR-0007)
under the existing image pool; no new allocator policy.

Surface resize already triggers a swapchain rebuild; we hook the
same path to resize the MSAA image. The image is per-canvas (not
per-frame): one allocation, replaced only on resize. Frames-in-flight
do not multiply storage because the resolve happens at end-of-pass —
the multi-sample image is never read by the application or held
across frames.

Validation:

- `sample_count = 0` and out-of-set values (3, 5, 7…) return
  `FLUX_ERROR_INVALID_ARGUMENT`.
- A value the device cannot support is clamped down to the next
  legal value and the clamp is reported through
  `flux_get_last_error` with code `FLUX_OK` and a message — i.e.
  not an error, but observable.

Headless devices and `lavapipe` default to 1× (their AA path is
software-emulated; the test suite stays cheap).

## Consequences

Positive:

- Anti-aliasing without consumer-side gymnastics. A UI consumer
  bumps `sample_count` to 4 and stops shipping a custom render
  pass.
- The canvas API surface grows by one field. ADR-0004 holds; the
  cache key is a strict extension.
- Compute, scene, and effect modules are untouched. MSAA is a
  property of the canvas pipeline, not a global device setting.
- Tilers absorb 4× nearly free; desktop pays the bandwidth
  consciously through an explicit knob.

Negative:

- 4× backing image (typical sized 1080p @ RGBA8 ≈ 8 MB at 1×,
  ≈ 33 MB at 4×). Acceptable on every shipping desktop GPU; users
  who don't opt in pay nothing.
- The pipeline cache grows by ≤ 4× in the worst case (every
  paint × every format × every sample count). In practice the
  worst case is bounded — a single application picks one
  sample_count at canvas-create time and sticks with it.
- One more dynamic state to consider when a future ADR proposes
  layered canvases, sub-passes, or sample-rate shading. We
  inherit those threads when they appear; not now.

## Alternatives considered

- **SDF fast-path.** Rejected: covers only analytic primitives;
  the staircasing on arbitrary curves (the actual pain) is not
  addressed.
- **Conservative rasterisation.** Rejected: over-coverage is not
  AA, and the extension is optional in Vulkan 1.3.
- **Render into a 2× backing surface and downsample.** Rejected:
  doubles bandwidth, doubles cache footprint, and the resolve is a
  separate pipeline the consumer maintains. MSAA's hardware
  resolve is strictly cheaper.
- **Sample-rate shading** (run the fragment shader per sample).
  Rejected as the default: it defeats the point of MSAA's cost
  story. Could be added later as an opt-in per pipeline.
- **Sample count negotiated per-frame instead of per-canvas.**
  Rejected: requires pipeline-cache lookups on every draw call;
  the per-canvas knob is a stable property the cache can rely on.

## When to revisit

Promote to a richer canvas configuration (per-pass sample count,
sample-rate shading, layered canvases) when *any* of:

- A consumer demonstrably needs to switch sample counts within a
  single frame.
- HDR + MSAA interactions surface specific compositing bugs that
  this ADR's flat "one knob" model can't express.
- Tile-based desktop GPUs become the dominant target and the
  bandwidth-conscious default of 1× becomes pessimistic.

## See also

- ADR-0004 — paint kind drives pipeline selection (the cache key
  this ADR extends).
- ADR-0006 — no runtime RHI (MSAA is per-canvas at create time;
  no dynamic backend switch).
- ADR-0007 — slab allocator (MSAA image allocates through it; no
  new memory policy).
- `src/canvas/renderer.c` — `get_canvas_pipeline`,
  multisample-state block.
