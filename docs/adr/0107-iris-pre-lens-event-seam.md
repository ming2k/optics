---
id: ADR-0107
title: "Iris pre-lens event seam — ordered iris_event interception before snapshot folding"
status: accepted
date: 2026-10-06
scope: libs/iris, bindings/iris-rs, tests/iris, tools/gen_symbols.py, docs
superseded_by: null
negative_knowledge: true
---

# ADR-0107: Iris pre-lens event seam — ordered `iris_event` interception before snapshot folding

- Status: Accepted
- Date: 2026-10-06
- Scope: `libs/iris` (public `<iris/event.h>` and `<iris/app.h>`, the Wayland / Win32 / Cocoa backends, `src/platform_input.h`, `src/platform_internal.h`, `src/app.c`), `bindings/iris-rs`, `tests/iris`, `tools/gen_symbols.py`, `docs/`.
- Amends: [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md) — materializes the `iris_event` stream it declares.
- Extends: [ADR-0043](0043-iris-foundations.md), [ADR-0044](0044-iris-backend-selection.md), [ADR-0055](0055-watch-apis-and-wakeup-seam.md).

## Context

`iris` owns the platform event loop and is the sole event pump of the stack
([ADR-0043](0043-iris-foundations.md), [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md);
`docs/governance/backends.md`). Each backend translates native events —
Wayland `wl_pointer` / `wl_keyboard` / text-input-v3, Win32 `WM_*`, Cocoa
`NSEvent` — and folds them directly into one `lens_input` per frame
(`libs/iris/src/app_wayland.c` `wp_accum` → `drain_input`).

Three structural facts force a decision now:

1. **There is no host interception point.** `iris_build_fn` receives `in`, a
   read-only copy of the snapshot `lens` has already consumed
   (`libs/iris/include/iris/app.h`). The only documented way for a host to
   filter input is "own the event source before `iris_app_run`" — which
   contradicts the same document's premise that iris owns the loop. A host
   that wants `Cmd+W` for itself, a scroll-sensitivity multiplier, pen
   calibration, or a synthetic click for automation must fork a backend.

2. **The governance already names a type that does not exist.** Both
   `docs/governance/backends.md` and [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md)
   describe iris as translating OS events into a uniform `iris_event`
   snapshot stream. No `iris_event` exists anywhere in the tree; the real
   fold target is `lens_input`.

3. **`lens` must stay headless and immediate-mode.** [ADR-0029](0029-lens-interaction-model.md)
   and [ADR-0036](0036-lens-input-clipboard-ime.md) deliberately rejected an
   event queue in favour of a per-frame snapshot, because widgets return a
   synchronous `lens_response`. Any interception seam must not reintroduce a
   second, deferred state machine.

The monorepo is pre-1.0 and under global lockstep versioning
([ADR-0106](0106-monorepo-identity-unification-glyph-vista-transit.md),
`tools/check-version-lockstep.sh`), so this is the window for a clean addition
with no compatibility forwarders.

## Decision

Insert a single, ordered, synchronous **pre-fold interception seam** between
iris's reification of a native event and the folding of surviving events into
the frame's `lens_input`.

```text
native OS event
      │
      ▼
[1] Reify        iris   native → iris_event (1:1, arrival order, platform-neutral)
      │
      ▼
[2] Intercept    host   on_event(&ev) per event — the ONLY rewrite/drop point
      │  surviving, possibly rewritten events
      ▼
[3] Accumulate   iris   per-frame platform accumulation state
      │
      ▼
[4] Fold         iris   accumulation → one lens_input snapshot (coalescing)
      │
      ▼
[5] Forward      iris   lens consumes the snapshot; widgets answer synchronously
      │
      ▼
[6] Read         host   build(ui, in) sees a read-only copy
```

1. **Introduce `iris_event` as a first-class public type** in
   `<iris/event.h>`. It is a closed, tagged, platform-neutral reification of
   exactly one native event: a `type` (C23 fixed-underlying enum), a `flags`
   mask, a monotonic `sequence`, and a tagged-union payload. It reuses the
   existing `lens_key` / `lens_mods` vocabulary rather than inventing parallel
   enums, so there is exactly one input vocabulary in the stack.

2. **Add `iris_event_fn on_event` to `iris_app_opts`.** It is invoked once per
   reified event, in arrival order, synchronously on the `iris_app_run` thread,
   *before* folding, and *outside* any `lens_begin` / `lens_end` pair. The host
   may mutate `*ev` fields and/or return a disposition:

   ```c
   typedef enum iris_event_disposition : uint32_t {
       IRIS_EVENT_PASS = 0,   /* deliver the (possibly rewritten) event       */
       IRIS_EVENT_DROP = 1,   /* discard it; it never reaches the snapshot    */
   } iris_event_disposition;

   typedef iris_event_disposition (*iris_event_fn)(iris_event *ev, void *user);
   ```

   `on_event == nullptr` preserves today's behaviour at zero cost — no
   allocation, no per-event indirection.

3. **Lifecycle events are undroppable.** `IRIS_EVENT_F_UNDROPPABLE` marks
   events iris itself must observe to keep window state coherent (`CLOSE`,
   `RESIZE`, `SCALE`, `FOCUS`). For those, iris ignores `IRIS_EVENT_DROP`; the
   host may still read them.

4. **iris stays policy-free.** iris reifies, orders, folds, and forwards; it
   never interprets event semantics. No "`Cmd+W` means quit", no default key
   routing, no rewriting. Every semantic decision lives in the host seam or in
   `lens`.

5. **Full input coverage from day one.** Pointer, button, scroll, keyboard,
   text commit and preedit, tablet/pen (pressure, tilt, eraser, barrel), drag
   and drop, resize, scale, focus, and close all flow through `iris_event`. A
   partial seam that some backends bypass would be the legacy burden this ADR
   exists to avoid.

6. **Folding is the only lossy step, and it is structural, never semantic.**
   Coalescing many pointer moves into the latest position is snapshot
   compression. Because it happens *after* interception, a host that needs
   every intermediate event sees the full stream before it is compressed.

7. **The seam lives inside `libiris`; no new library is created.** The
   tripartite model of [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md)
   forbids a separate event library, which would expose the loop internals,
   duplicate compile-time backend selection, and add lockstep/package weight
   for zero orthogonality.

### Invariants & Behavioral Boundaries

- iris **must not** read the semantic content of an `iris_event` to make a
  decision. It may only reify, order, mark undroppable, fold, and forward.
- Backends **must not** write `lens_input` directly. All platform input reaches
  `lens` exclusively through the single fold stage ([3]→[4] above).
- `on_event` **must** run on the `iris_app_run` thread, before folding, in
  arrival order, and outside any `lens_begin` / `lens_end` pair. It must not
  re-enter `iris_app_run` or call `lens` APIs.
- `iris_event` **must** remain platform-neutral: no native handles, pointers,
  or compositor serials in the public type. Platform-specific capabilities
  continue to travel through capability APIs
  ([ADR-0099](0099-desktop-integration-fkeys-activation-and-clipboard.md)).
- Lifecycle events **must** be flagged `IRIS_EVENT_F_UNDROPPABLE` and survive a
  host `IRIS_EVENT_DROP`.
- `on_event == nullptr` **must** be behaviourally identical to the pre-seam
  pipeline and incur no per-event cost.

## Alternatives Considered

- **An SDL/GLFW-style public event queue (`iris_poll_event`).** Rejected. It
  reintroduces a deferred state machine and forces every caller to defer
  widget branches, directly contradicting the synchronous `lens_response`
  contract that [ADR-0029](0029-lens-interaction-model.md) and
  [ADR-0036](0036-lens-input-clipboard-ime.md) established.

- **Keep the status quo: hosts own the event source to filter.** Rejected. It
  contradicts iris owning the loop, forces hosts to fork and maintain three
  backends, and leaks Wayland/Win32/Cocoa into application code.

- **Place the seam *after* folding (a post-snapshot hook).** Rejected. The host
  would see only the compressed terminal state — no ordering, no per-event
  identity, no ability to intercept a single event. This is the "photo after
  the meal" failure mode: observable, not actionable.

- **A consume-only seam (drop, no rewrite).** Rejected. It cannot serve the
  host needs that motivate the seam in the first place (scroll scaling,
  coordinate/DPI remap, pen calibration, synthetic remap), which would then
  fall back to forking a backend.

- **A new `liboptics-events` (or `libiris-events`) library.** Rejected. It
  breaks the tripartite separation of
  [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md), exposes
  the event loop as a cross-library contract, duplicates per-OS backend
  selection with no benefit, and adds lockstep and packaging weight.

- **Expose raw native events (or Wayland serials) to the host.** Rejected.
  Violates the platform-neutral public surface of
  [ADR-0043](0043-iris-foundations.md). The activation-serial use case is
  already served internally by
  [ADR-0099](0099-desktop-integration-fkeys-activation-and-clipboard.md).

- **Put the interception point in `lens`.** Rejected. `lens` is headless and
  embeddable by design ([ADR-0024](0024-lens-foundations.md),
  [ADR-0025](0025-lens-draws-through-flux-canvas.md)); OS event concerns
  belong to `iris`.

- **Defer seam callbacks (batch per frame / background thread).** Rejected.
  Breaks the single-lifecycle-on-the-calling-thread and thread-affinity
  guarantees of [ADR-0043](0043-iris-foundations.md) and would race the fold.

- **Event injection through the same seam.** Rejected *for this ADR*, not
  discarded in principle. A per-event callback can observe, drop, and rewrite
  but cannot append; synthesis needs its own ordering and lifetime contract.
  Injection is deferred to a separate, explicitly-scoped decision.

- **A `size` guard or version sentinel on `iris_event` / `iris_app_opts`.**
  Rejected. Events never cross a binary boundary (they live and die inside one
  `iris_app_run` frame), and the monorepo rebuilds all consumers in lockstep
  pre-1.0. A guard would be pure ceremony — the legacy burden this ADR avoids.

## Consequences

### Positive

- Hosts gain complete input control — shortcuts, remapping, calibration,
  synthetic input for tests and demos, and live debug inspection — without
  forking a backend or leaking platform code.
- Exactly one interception point exists across all three backends, so
  cross-platform parity is enforced by construction, not by review.
- The governance description of `iris_event` (`docs/governance/backends.md`,
  [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md)) becomes
  code-true, closing a documentation/code drift.
- `lens` remains headless and unchanged; `flux`, `glyph`, `prism`, `transit`,
  and `vista` are untouched.

### Negative / Trade-offs

- Three backends must be refactored from "write `lens_input` directly" to
  "reify → intercept → accumulate → fold"; this is the bulk of the work
  (`app_wayland.c`, `app_win32.c`, `app_cocoa.m`).
- `iris_app_opts` and the installed public surface grow. Mitigated: pre-1.0
  lockstep makes a clean addition safe, and no compatibility shim is added.
- A per-event host callback adds a call per event when `on_event` is set.
  Mitigated by the `nullptr` fast path.
- Hosts could be tempted to reimplement routing policy in the seam. The
  invariants above and the documentation must steer them toward `lens` or
  host-level policy, not a parallel input system.

### Required Follow-up

- `libs/iris/include/iris/event.h` (new), `iris/app.h`, `iris/iris.h`
  (umbrella), `libs/iris/meson.build` (`sources` / `install_headers`).
- `libs/iris/src/platform_input.h`, `src/platform_internal.h`, `src/app.c`,
  and the three backends.
- `tools/gen_symbols.py` — add the new header to the hard-coded iris list so
  `docs/reference/symbols.md` stays complete.
- `tests/iris/` — headless tests for arrival order, drop, rewrite,
  undroppable handling, and `on_event == nullptr` equivalence.
- `bindings/iris-rs` — `crates/iris/src/lib.rs` safe wrapper over `on_event`;
  `crates/iris-sys` picks up the header via the umbrella include.
- Docs: `docs/governance/backends.md`,
  `docs/explanation/application-architecture.md`, `docs/dev/cross-platform.md`
  (three-backend contract), and `docs/explanation/rust-bindings.md`.

## References

- [ADR-0043](0043-iris-foundations.md) — iris L3 foundations; iris owns the loop.
- [ADR-0044](0044-iris-backend-selection.md) — compile-time backend selection.
- [ADR-0055](0055-watch-apis-and-wakeup-seam.md) — the backend wakeup seam the
  interception thread-affinity mirrors.
- [ADR-0029](0029-lens-interaction-model.md) — synchronous `lens_response`.
- [ADR-0036](0036-lens-input-clipboard-ime.md) — per-frame snapshot over an
  event queue.
- [ADR-0084](0084-iris-tripartite-architecture-and-c23-app-opts.md) — tripartite
  architecture; names `iris_event`.
- [ADR-0099](0099-desktop-integration-fkeys-activation-and-clipboard.md) —
  platform capabilities that stay out of the neutral event type.
- `libs/iris/include/iris/app.h`, `libs/iris/src/app_wayland.c`,
  `docs/governance/backends.md`.
