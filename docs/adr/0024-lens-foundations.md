# ADR-0024: Lens project foundations

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Records the foundational decisions for the
  immediate-mode façade over a retained-mode core.

## Context

`lens` is the L2 layer of the flux/lens/iris stack: a headless UI
toolkit that turns per-frame immediate-mode build calls into a
retained-mode widget tree owning layout, interaction, animation, and a
draw list. Before any widget shipped, lens needed a small set of
structural decisions fixed in writing so every later ADR (identity,
store, layout, interaction, overlays, a11y) could build on them.

The forces that shaped the choices below:

1. **Authors think in immediate mode; the machine needs retained
   state.** A pure retained API forces explicit node handles and tree
   mutation calls; a pure immediate API cannot animate, hold focus, or
   keep per-widget state across frames. Lens exposes the immediate-mode
   ergonomics and reconciles it against a retained core.
2. **The toolkit is headless.** Lens must run in CI, offscreen renderers,
   and tests without a window system. Drawing is always routed through a
   canvas the caller supplies; lens never owns a surface.
3. **Per-frame arena + persistent store is the right split.** Build
   output (draw list, tab order, semantics strings) is disposable each
   frame; widget identity, animation curves, and per-node user state must
   survive.
4. **One lifecycle object matches observable lifetime.** A `context`
   separate from a `theme` or `device` is ceremony when they are always
   created and destroyed together.

## Decision

1. **One library, one umbrella header.** `liblens.so`, `lens.pc`,
   `<lens/lens.h>`. Per-concern subset headers
   (`<lens/icon.h>`) remain for compile-time narrowing.
2. **Immediate-mode façade over a retained-mode core.** The caller writes
   `lens_begin` → build (`lens_row`, `lens_button`, …) → `lens_end` →
   `lens_render(ui, canvas)` each frame. Lens reconciles the build calls
   against a retained tree keyed by stable ids
   ([ADR-0026](0026-lens-id-system.md), [ADR-0027](0027-lens-retained-store.md)).
3. **Headless by construction.** `flux_device *` in `lens_desc` is
   optional; when `NULL`, lens falls back to libc `malloc` and drawing is
   unavailable but the build/layout/interaction pipeline still runs.
   Drawing is routed exclusively through `<flux/canvas.h>`
   ([ADR-0025](0025-lens-draws-through-flux-canvas.md)).
4. **Per-frame arena + persistent store.** `flux_arena` is reset at the
   top of every `lens_begin`; the open-addressing id→node store
   ([ADR-0027](0027-lens-retained-store.md)) holds the retained tree and
   per-node state. Nodes are reaped after a grace window
   ([ADR-0038](0038-lens-node-state-gc.md)).
5. **`lens` is the only lifecycle object.** Theme, scale, device, arena,
   store, input, interaction state, and overlay open-set all live on
   `struct lens`. Created via `lens_create`, destroyed via `lens_destroy`.
6. **Implicit root container.** `lens_begin` opens a column container
   covering `input.display_size` so the first caller `lens_row`/`lens_label`
   need not seed a root.

References: `libs/lens/include/lens/lens.h`,
`libs/lens/src/core/context.c`, `libs/lens/src/internal.h`.

## Alternatives Considered

- **Pure retained API (node handles + mutate calls).** Familiar from
  DOM-style toolkits, but forces every caller to manage child ordering,
  identity, and diffing. Reject: the immediate-mode façade is the whole
  point of lens.
- **Pure immediate API (no retained state).** Cannot animate hover fades,
  hold keyboard focus, or persist per-widget state. Reject: the retained
  core exists precisely to carry that state.
- **Separate `lens_context` + `lens_device` + `lens_theme`.** Standard in
  some engines, but here the three are always created and destroyed
  together. Reject: ceremony without benefit.
- **Owning a surface.** Reject: headless/CI/offscreen is first-class; the
  caller (iris, or a test) owns the window system.

## Consequences

Positive:

- The public API matches the mental model: "describe the frame, lens
  remembers the rest."
- Headless operation is not bolted on; CI and tests reach the full
  build/layout/interaction path without a GPU.
- One lifecycle object keeps resource management obvious.

Negative:

- The immediate-mode/retained split is load-bearing: every later ADR must
  respect that build output is per-frame and state is id-keyed.
- The implicit root means a caller that forgets `lens_begin` builds into
  a dangling state; the contract is enforced by documentation, not by a
  crash.

## References

- [ADR-0025](0025-lens-draws-through-flux-canvas.md) — drawing seam.
- [ADR-0026](0026-lens-id-system.md) — identity.
- [ADR-0027](0027-lens-retained-store.md) — retained store.
- [ADR-0031](0031-lens-symbol-namespaces.md) — symbol namespaces.
- [ADR-0023](0023-unified-monorepo-build.md) — monorepo context.
