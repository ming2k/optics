# ADR-0008: Image-effect pipeline as the home for blur and friends

- Status: Accepted
- Date: 2026-05-24

## Context

Blur is the gateway feature for a family of effects callers will ask
for next: drop shadow, inner shadow, frosted-glass UI panels, bloom,
tone mapping, simple colour grading. Each of them is some
combination of "sample an image, run a kernel, write an image" —
optionally fed back into the canvas as a paint source or composited
into a later frame.

There are three places blur could plausibly live in flux today:

1. **As a new `flux_paint_kind`** (e.g. `FLUX_PAINT_BLURRED_IMAGE`).
   Reuses the canvas draw path. Breaks the ADR-0004 single-pass
   model: blur needs a ping-pong target and two passes
   (separable Gaussian) before the canvas's fragment shader
   ever runs. Carrying that intermediate state inside `flux_paint`
   smuggles a render-graph into a POD value type.
2. **As an inline canvas operator** (`flux_canvas_push_blur` /
   `flux_canvas_pop_blur`). Matches Skia's `saveLayer`/`restore`
   model. Forces the canvas to own offscreen target management,
   blend tracking, and pass scheduling — a meaningful new
   responsibility for a module that currently owns one render pass.
3. **As an image-domain operator** (`flux_image → flux_image`),
   external to the canvas, runnable inside or outside a frame.
   Compute-shader-native, fits the existing `flux_compute_pipeline`
   abstraction (ADR is implicit in `include/flux/compute.h`), and
   stays away from the canvas state machine.

The current paint/canvas model (ADR-0004) is small and stable
precisely because it doesn't carry multi-pass state. Adding a render
graph to satisfy the first effect would erode that property for
every later effect too. Conversely, an external image-effect module
is additive: the canvas keeps its contract, and effects compose by
producing `flux_image`s that the canvas can already consume via
`flux_canvas_draw_image` or a future `FLUX_PAINT_IMAGE`.

The compute module already gives us the right primitive: a
pipeline + dispatch surface bound to the device-wide bindless set,
recordable into any `VkCommandBuffer`. An effect is a compute
pipeline (or a short chain of them) plus a transient target policy.

## Decision

Introduce a separate **image-effect pipeline** as a new top-level
module (`src/effect/`, `include/flux/effect.h`). Effects are the
unit, not paint kinds and not canvas operators.

Contract:

- An effect reads one or more `flux_image` inputs, writes one
  `flux_image` output. Output may be caller-supplied (explicit
  lifetime) or transient (effect-owned, valid only until the next
  effect submission on the same device).
- Effects are compute-first. A separable Gaussian blur is two
  dispatches over a ping-pong pair; a tone-map is one dispatch. The
  effect module owns the transient target pool and the barriers
  between dispatches.
- Effects are recordable into any `VkCommandBuffer`, matching
  ADR-0006's "no runtime RHI" stance and the compute module's
  existing `flux_compute_dispatch` shape. Inside a frame: pull
  `flux_frame_vk_command_buffer()`. Outside a frame: caller's
  one-shot command buffer.
- The canvas does not learn about effects. It learns about
  `flux_image`. If the user wants a blurred image on the canvas,
  they run the blur effect, then draw the resulting `flux_image`.
  Drop shadow becomes: rasterize the path's alpha mask into a
  scratch image (a future canvas capability) → blur it → composite
  underneath the geometry. Each step is a separate, testable unit.

**Blur is the first concrete effect** and ships in the same change
that lands the module skeleton. Bringing up the module with no
operator would be premature abstraction; bringing up blur without
the module would lock us into option (1) or (2) by accident. The
two arrive together so the abstraction is validated by one real
consumer before it grows.

Initial public surface (illustrative, not final spelling):

```c
typedef struct flux_effect_blur_desc {
    flux_struct_type type;          /* FLUX_TYPE_EFFECT_BLUR_DESC */
    const void      *next;
    flux_image      *input;         /* retained for the dispatch */
    float            sigma;         /* Gaussian sigma in pixels */
} flux_effect_blur_desc;

FLUX_NODISCARD FLUX_API flux_result flux_effect_blur(
    VkCommandBuffer cmd,
    const flux_effect_blur_desc *desc,
    flux_image **out);
```

Subsequent operators (shadow, tone-map, bloom) take the same
shape: a desc struct + a command buffer + an out-image. No
effect-graph object yet; chaining is caller-side until a real
consumer demonstrates the need.

## Consequences

Positive:

- ADR-0004's paint model stays single-pass and POD-shaped. The
  paint enum doesn't grow effect-shaped members.
- The canvas keeps one render pass and one state machine. New
  effects don't reopen its design.
- Effects compose through `flux_image`, the type both the canvas
  and the scene module already consume. No new wiring at the
  boundary.
- Compute-shader-native means separable blurs, large kernels, and
  group-shared optimisations are cheap. Same path serves headless
  workloads (offline image processing) and frame workloads.
- New module is opt-in: linkers can dead-strip it for consumers
  who don't use any effect.

Negative:

- Transient target management is a new responsibility. The effect
  module needs a small pool keyed by `(format, w, h)` with a
  per-frame reset hook. This is meaningful new code, though
  bounded and testable in isolation.
- Two-pass effects pay an extra image read/write versus a fused
  single-pass fragment shader. For Gaussian this is the right
  trade — separable + compute beats fused fragment at any useful
  kernel radius.
- Drop shadow is now a three-step recipe rather than a single
  paint flag. We accept the verbosity in exchange for orthogonal
  primitives; a higher-level `flux_canvas_draw_shadowed_path`
  convenience can be layered on later once the primitive shape is
  proven.
- A new top-level module is a new public-API surface to maintain.
  Mitigated by keeping the initial surface to one operator and
  one desc struct.

## Alternatives considered

- **`flux_paint_kind` blur** (option 1 above). Rejected: smuggles
  multi-pass state into a POD value type and breaks ADR-0004's
  single-pass invariant.
- **Canvas `push_blur`/`pop_blur`** (option 2). Rejected: makes
  the canvas own offscreen targets and pass scheduling — a much
  larger responsibility expansion than the feature warrants, and
  one that grows with every later effect.
- **Effect graph object up front** (`flux_effect_graph` with nodes
  and edges). Rejected as premature; we don't have two concrete
  effects yet, let alone the chaining patterns that would justify
  a graph. Revisit when a real consumer chains three or more
  operators per frame.
- **Fragment-shader effects under the canvas pipeline cache**.
  Rejected: ties effect dispatch to canvas frame state, blocks
  headless use, and gives up the compute-shader optimisations
  (group-shared sampling, separable kernels) that make blur cheap.

## When to revisit

Promote to a graph object, or fold a subset of effects back into
the canvas, when *any* of:

- A real consumer chains three or more effects per frame and the
  caller-side wiring becomes the bottleneck rather than the
  shaders.
- Drop shadow proves to be the dominant use of the effect module
  and the three-step recipe creates measurable misuse (wrong
  formats, leaked transient targets, missed barriers).
- A platform constraint makes compute-first untenable for a
  required effect and we need a fragment fallback path.

## See also

- `include/flux/canvas.h` — `flux_image`, `flux_paint_kind`.
- `include/flux/compute.h` — `flux_compute_pipeline`,
  `flux_compute_dispatch` (the primitive effects build on).
- ADR-0004 — paint kind drives pipeline selection (the invariant
  this ADR preserves).
- ADR-0006 — no runtime RHI; effects inherit the same stance.
- ADR-0007 — slab allocator (transient targets allocate through
  it; no new `VkDeviceMemory` policy).
