# ADR-0055: Callback-driven watch APIs + the backend wakeup seam

- Status: Accepted
- Date: 2026-08-08
- Scope: iris public API (`theme.h`, `a11y.h`) and the internal backend
  contract (`platform_wakeup.h`, `platform_internal.h`).

## Context

`iris/theme.h` and `iris/a11y.h` exposed Unix-isms in the public API: a
watcher file descriptor, a poll mask, and pump calls the host had to
fold into its event loop (`iris_color_scheme_watcher_fd`,
`iris_a11y_fd`, …). pollable fds are not a portable abstraction: Win32
runs a message loop and waitable handles, Cocoa runs a CFRunLoop. Every
backend needs *some* way for a helper thread to reach the event-loop
thread; the question was what shape that takes so the public API stays
identical on all platforms (ADR-0043).

## Decision

1. **Public theme watching is a callback.**
   `iris_color_scheme_watch(cb, user)` / `iris_color_scheme_unwatch()`;
   `cb` is guaranteed to run on the iris main thread, serialized with
   the event loop. The fd/poll/pump functions are removed from the
   public headers. One watcher per process; re-watching replaces the
   registration; unwatch blocks until the platform watcher thread
   exited and the callback can never fire again.
2. **The internal wakeup seam (`src/platform_wakeup.h`)** is split into
   a backend-supplied *kick* (a thread-safe, non-blocking function that
   makes the loop's wait return: eventfd in the poll set on Wayland,
   `PostThreadMessage` on Win32, `CFRunLoopPerformBlock` +
   `CFRunLoopWakeUp` on Cocoa) and a shared lock-protected FIFO drained
   on the loop thread (`iris_platform_wakeup_post` /
   `iris_platform_wakeup_drain`). Posts with no registered loop return
   -1 and drop the callback.
3. **The portal theme watcher** (`theme_watch_portal.c`) pumps sd-bus on
   its own thread (poll on the sd-bus fd with `sd_bus_get_events` mask +
   a stop pipe) and delivers changes through the seam. The stub
   semantics (no libsystemd → watch returns -1) are unchanged.
4. **a11y keeps a main-thread integration point.** The AT-SPI bridge's
   fd accessors moved to `src/a11y_internal.h` as `iris_a11y__fd` /
   `__poll_events` / `__pump` (unexported). It is deliberately *not*
   threaded: method handlers read the semantic snapshot that
   `iris_a11y_update` writes on the main thread, and moving the pump
  off-thread would be a real data race. Win32 (UIA) and Cocoa
   (NSAccessibility) implement their own bridges; until then they ship
   `a11y_stub.c`.
5. **Pointer buttons are mapped through `src/platform_input.h`**
   (`iris_pointer_button` → `iris_pointer_button_to_lens`), so no
   backend includes another platform's headers
   (`linux/input-event-codes.h` stays Wayland-only).

## Alternatives Considered

- **Keep the fd API on Unix, different API elsewhere.** Rejected: the
  public API must be identical across platforms (ADR-0043); a
  lowest-common-denominator callback is the only shape all three event
  loops can honour.
- **Invoke the callback directly from the watcher thread.** Rejected:
  lens/flux state is main-thread affine; centralising the hop in the
  wakeup seam keeps every subsystem honest by construction.
- **Thread the AT-SPI pump like the theme watcher.** Rejected for now:
  it would race `iris_a11y_update`'s writes; the internal-fd integration
  point preserves the exact pre-refactor ordering semantics.

## Consequences

- Public API break in `theme.h`/`a11y.h` (pre-1.0; the Rust binding was
  updated in lockstep). Hosts gain a simpler contract: register a
  callback, everything arrives on the main thread.
- Every future backend implements exactly one wakeup primitive and
  follows the recipe in `platform_wakeup.h`.
- Watcher threads never touch lens/flux state; they only post.

## References

- [ADR-0043](0043-iris-foundations.md),
  [ADR-0044](0044-iris-backend-selection.md) — the backend seam this
  refines.
- `libs/iris/src/platform_wakeup.h`, `libs/iris/src/theme_watch_portal.c`,
  `libs/iris/src/a11y_internal.h`, `libs/iris/src/platform_input.h`.
