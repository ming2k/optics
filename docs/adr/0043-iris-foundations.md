# ADR-0043: Iris project foundations — L3 application toolkit

- Status: Accepted
- Date: 2026-07-21
- Scope: iris (L3 application toolkit). Records the foundational
  decisions for the platform-integration layer above lens.

## Context

iris is the L3 layer of the flux/lens/iris stack: the application
toolkit that owns windows, the event loop, system integration (theme,
file dialog, cursor), and the accessibility transport. It sits above
lens ([ADR-0024](0024-lens-foundations.md)), which owns the widget tree
and semantic model, and above flux, which owns rendering. Before any
platform backend shipped, iris needed a small set of structural
decisions fixed in writing so the cross-platform surface and the
backend seam stay coherent as Win32 and Cocoa backends land.

The forces that shaped the choices below:

1. **lens is headless; iris is not.** Lens deliberately owns no window
   system ([ADR-0025](0025-lens-draws-through-flux-canvas.md)). iris is
   the layer that creates the surface, drives the event loop, and feeds
   lens an input snapshot every frame
   ([ADR-0029](0029-lens-interaction-model.md), [ADR-0036](0036-lens-input-clipboard-ime.md)).
2. **Public API is platform-neutral.** A consumer includes `<iris/iris.h>`
   and calls `iris_app_run`; the active backend is chosen at build time
   ([ADR-0044](0044-iris-backend-selection.md)), not at call time. The
   same C signatures are the contract every backend must satisfy.
3. **Per-frame callbacks, not a retained scene.** `iris_app_run` drives
   two host callbacks per frame: `build(ui, in)` (inside
   `lens_begin`/`lens_end`) and `paint(canvas)` (inside
   `flux_canvas_begin`/`_end`, before `lens_render`). The host owns the
   chrome; iris owns the plumbing.
4. **System integration is optional-capability, not required.** Live
   theme watching, AT-SPI, and portal file dialogs depend on libsystemd
   / xdg-desktop-portal; when absent, iris degrades (startup-only theme
   query, no-op a11y stub) rather than failing.
5. **One lifecycle on the calling thread.** `iris_app_run` blocks until
   the window closes; both callbacks run on the same thread.

## Decision

1. **One library, one umbrella header.** `libiris.so`, `iris.pc`,
   `<iris/iris.h>`. Subset headers (`<iris/app.h>`, `<iris/window.h>`,
   `<iris/theme.h>`, `<iris/cursor.h>`, `<iris/file_dialog.h>`,
   `<iris/a11y.h>`) cover individual concerns.
2. **iris owns windows + event loop + system integration; lens owns the
   widget tree and semantic model.** iris creates the surface, the flux
   device/canvas, and the lens context; it folds pointer/keyboard events
   into one `lens_input` per frame and drives the build/paint callbacks.
3. **`iris_app_run` is the cross-platform entry point.** It opens a
   window, runs the platform event loop, and dispatches `build` + `paint`
   per frame. Implemented in `libs/iris/src/app.c` as a thin dispatcher;
   today it forwards to the Wayland backend
   ([ADR-0044](0044-iris-backend-selection.md)).
4. **Per-frame callback contract.** `build` runs inside an open
   `lens_begin`/`lens_end` and receives `(lens*, lens_input*)`; `paint`
   runs inside an open `flux_canvas_begin`/`_end`, *before*
   `lens_render`, so host drawing lands under lens's chrome. Both
   callbacks are optional (a pure-lens app leaves `paint` NULL; a
   pure-canvas demo leaves `build` NULL).
5. **Platform-neutral public surface.** Every public iris function is
   meaningful on every backend; backend-specific capability that is not
   universal degrades to a documented no-op (e.g. window state requests
   honour compositor policy; on backends without the underlying
   capability the calls are no-ops — see the seam rationale in
   `window.h`).
6. **Optional system integration.** Theme query reads the GNOME /
   freedesktop colour scheme at startup; live watching via
   `org.freedesktop.portal.Settings` is enabled when libsystemd is
   available. File dialogs shell out to xdg-desktop-portal over sd-bus.
   The a11y seam is a real AT-SPI bridge when libsystemd is available, a
   contract-only stub otherwise
   ([ADR-0035](0035-lens-accessibility-tree.md)).
7. **Thread-affine, blocking.** `iris_app_run` and the window-state API
   are thread-affine to the calling thread; the call blocks until the
   window closes.

References: `libs/iris/include/iris/iris.h`, `libs/iris/include/iris/app.h`,
`libs/iris/src/app.c`, `libs/iris/meson.build`.

## Alternatives Considered

- **Runtime backend selection (dlopen).** Reject: doubles the API surface
  (init/factory per backend), adds a loader, and no realistic iris
  consumer links two backends in one binary. Compile-time selection
  ([ADR-0044](0044-iris-backend-selection.md)) is simpler.
- **Retained scene graph owned by iris.** Reject: lens already owns the
  widget tree; a second scene graph in iris would duplicate layout and
  state.
- **Mandatory libsystemd.** Reject: breaks platform-less CI / bindgen
  builds and any minimal Linux target; optional capability keeps the
  library linkable everywhere.
- **Per-platform public headers.** Reject: the whole point is that
  `<iris/iris.h>` is the same on every platform; backend differences live
  behind the seam.

## Consequences

Positive:

- The public API matches the mental model: "call `iris_app_run`, build
  chrome per frame, iris handles the rest."
- Lens stays headless; every window-system concern lives in iris.
- Optional capabilities degrade gracefully, so the same source compiles
  from full-featured desktops down to header-only bindgen builds.

Negative:

- The cross-platform surface grows conservatively; a capability that
  cannot be expressed neutrally (or cannot degrade to a no-op) cannot
  land in the public API without breaking the contract.
- Backend correctness is enforced by review, not by the type system — a
  Win32/Cocoa backend must satisfy the same per-frame callback contract
  the Wayland backend does.

## References

- [ADR-0024](0024-lens-foundations.md) — the lens layer iris builds on.
- [ADR-0025](0025-lens-draws-through-flux-canvas.md) — the drawing seam
  iris supplies a canvas into.
- [ADR-0029](0029-lens-interaction-model.md) — the interaction model iris
  feeds.
- [ADR-0035](0035-lens-accessibility-tree.md) — the a11y walk iris's
  AT-SPI bridge consumes.
- [ADR-0036](0036-lens-input-clipboard-ime.md) — the input/clipboard/IME
  contract iris fills.
- [ADR-0044](0044-iris-backend-selection.md) — backend selection.
- [ADR-0023](0023-unified-monorepo-build.md) — monorepo context.
