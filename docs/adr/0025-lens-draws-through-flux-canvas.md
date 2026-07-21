# ADR-0025: Lens draws only through <flux/canvas.h>

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Establishes the single drawing seam.

## Context

Lens is headless by construction
([ADR-0024](0024-lens-foundations.md)). The retained core produces a
per-frame draw list of node-relative commands; those commands must be
realised as pixels by something. The question is what that "something"
is: a private renderer inside lens, a flux device/queue directly, or the
flux canvas primitive layer.

Forces:

1. **Headless parity.** Whatever renders lens must also work in CI and
   offscreen paths without a window system.
2. **No duplicated rendering engine.** flux already owns a Vulkan-backed
   (and CPU-backed) canvas with pipeline caching, clipping, and image
   drawing. Re-implementing any of that in lens is pure cost.
3. **Stable seam.** The retained core resolves node-relative geometry
  against `final_rect` only after layout. The drawing layer must accept
  already-absolute pixel rects and a clip stack.

## Decision

Lens draws only through `<flux/canvas.h>`. Concretely:

1. **`lens_render(ui, canvas)` is the only render entry point.** It walks
   the retained tree front-to-back, resolves each node's draw commands
   against its `final_rect`, maintains a logical clip stack, and emits
   `flux_canvas_draw_*` calls. Implemented in `libs/lens/src/render/replay.c`.
2. **Lens links no renderer.** There is no Vulkan, shader, or pipeline
   code in lens. `flux_device` in `lens_desc` is optional and, even when
   present, lens never calls device-level APIs directly for drawing.
3. **Node-relative commands.** Draw commands (`lens_draw_cmd` in
   `internal.h`) carry geometry relative to the node box; absolute pixels
   are resolved at replay time, after layout. Text commands copy their
   run into the per-frame arena so caller buffers may be short-lived.
4. **Canvas owns the surface.** The caller wraps a `flux_canvas` around
   whatever surface it owns (Wayland `VkSurfaceKHR`, offscreen image,
   CPU buffer) and hands it to `lens_render`.

References: `libs/lens/include/lens/lens.h` (`lens_render`),
`libs/lens/src/render/replay.c`, `libs/lens/src/render/drawlist.c`.

## Alternatives Considered

- **Private Vulkan renderer inside lens.** Reject: duplicates flux's
  pipeline cache, allocator, and CPU backend; breaks headless parity.
- **Direct `flux_device`/queue calls from lens.** Reject: bypasses the
  canvas's clip stack, batching, and pipeline selection; the canvas seam
  is exactly the abstraction lens needs.
- **Owning a surface.** Reject: same as ADR-0024 — headless is
  first-class, the caller owns the window system.

## Consequences

Positive:

- Lens stays renderer-agnostic: any backend flux's canvas supports is
  automatically a lens backend.
- Headless tests render to a CPU canvas without a GPU.
- The draw list is a pure data structure, which makes
  damage-tracking ([ADR-0030](0030-lens-damage-tracking.md)) and overlay
  emission composable.

Negative:

- Lens is coupled to the flux canvas API surface; changes to canvas
  semantics (clip behaviour, premultiplied-alpha contract) ripple into
  replay.
- There is no "render without a canvas" path; even offscreen needs a
  caller-supplied canvas.

## References

- [ADR-0024](0024-lens-foundations.md) — headless foundations.
- [ADR-0030](0030-lens-damage-tracking.md) — damage tracking (draw list
  hashing).
- [ADR-0033](0033-lens-text-seam.md) — text routed through the same seam.
