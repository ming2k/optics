# ADR-0045: Iris host resource lifecycle callbacks

- Status: Accepted
- Date: 2026-07-26
- Scope: iris application entry point

## Context

ADR-0043 established `build` and `paint` as Iris's per-frame host callbacks.
`paint` exposes Iris's borrowed `flux_device` so applications can create and
update device-backed document resources. A non-trivial host may therefore own
Flux images, text contexts, paths, or other objects that must be released
before Iris destroys the device.

Previously Iris returned from its frame loop directly into internal cleanup.
The host regained control only after `iris_app_run` returned, when the
borrowed device was already invalid. Requiring hosts to destroy all resources
during an arbitrary final frame is fragile and cannot cover startup failure
or window-close paths consistently.

## Decision

Add two optional callbacks to `iris_app_config`:

- `start(lens*, flux_device*, user) -> bool` runs after Iris creates its
  canvas, device, and Lens context, and before the first frame. Returning
  false aborts the run.
- `stop(lens*, flux_device*, user)` runs once after a successful or absent
  `start`, after the frame loop, and before Iris destroys any host-visible
  Flux/Lens object.

Both callbacks are thread-affine to `iris_app_run`, like `build` and `paint`.
Iris keeps its platform helper context active during `stop`. If `start`
returns false, `stop` is not called; the host is responsible for rolling back
any partial setup before returning false.

## Alternatives considered

- **Let hosts clean up after `iris_app_run`**: rejected because the borrowed
  device is already destroyed.
- **Expose ownership of the device to the host**: rejected because Iris owns
  the rendering and window lifecycle and must keep one authoritative teardown
  order.
- **Require lazy setup in the first `paint` and teardown in a final paint**:
  rejected because there is no reliable final-frame signal on every failure
  and close path.

## Consequences

- Hosts can safely own device-backed resources without reaching into the
  platform backend.
- Iris has an explicit lifecycle envelope around its per-frame callbacks.
- Adding fields changes the public configuration struct layout; language
  bindings must initialize the new fields, normally to null when unused.

## References

- [ADR-0043](0043-iris-foundations.md) — Iris application toolkit contract.
- `libs/iris/include/iris/app.h`
- `libs/iris/src/app_wayland.c`
