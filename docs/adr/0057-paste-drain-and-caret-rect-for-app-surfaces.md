# ADR-0057: Paste drain and caret rect for app-owned editing surfaces

- Status: Accepted
- Date: 2026-08-09
- Scope: lens public API (`include/lens/lens.h`,
  `src/input/clipboard.c`, `src/internal.h`) and the Rust binding
  (`bindings/lens-rs/.../lens/src/lib.rs`).

## Context

ADR-0036 designed the clipboard as a host channel: `lens_request_paste` asks
the host, the host later calls `lens_paste`, and the *focused text widget*
drains the payload the next frame. The same assumption holds for the IME
caret rect: only a focused text widget reports one (`lensi_set_caret_rect`
is internal).

Hosts that render their own editing surface — drawn directly through flux,
owning their own document model, caret and hit-testing — build no lens text
widgets while editing. For them the paste queue is undrainable and the IME
candidate window has no caret to follow. Synapse's document editor is the
first such host.

The 1 KiB staging buffer (`LENSI_PASTE_MAX`) also predates app-level paste:
a document editor legitimately pastes hundreds of KiB of Markdown.

## Decision

Two internal helpers become public API:

1. `uint32_t lens_take_paste(lens *ui, char *dst, uint32_t cap)` — drains a
   pending paste payload (one-shot) into caller storage. For app-owned
   editing surfaces; text widgets keep their internal drain, and the doc
   comment tells app code to call it only while its own surface is the
   paste target.
2. `void lens_set_caret_rect(lens *ui, flux_rect r)` — lets an app-owned
   surface report the caret rect that the platform layer forwards to the
   IME, exactly as the widget-reported rect is consumed.

`LENSI_PASTE_MAX` grows from 1 KiB to 1 MiB (one staging buffer per lens
context). The Rust `Frame` gains `copy` / `request_paste` / `take_paste` /
`set_caret_rect` wrappers.

## Alternatives Considered

- **A hidden lens text widget behind the custom editor** (paste and caret
  rect "for free"). Rejected: two editing surfaces would share one focus,
  and the widget would fight the app's own input consumption — a fake
  integration strictly worse than two one-line public wrappers.
- **Synchronous `iris_clipboard_get/set` bypassing the paste queue.**
  Rejected: Wayland clipboard reads are asynchronous by contract (the
  roundtrip happens inside the platform backend), and a second clipboard
  path beside ADR-0036 would fork the abstraction. Draining the existing
  queue keeps one model.

## Consequences

- The paste queue now has two consumer kinds (widgets, app surface); the
  one-shot drain means whichever runs first in a frame wins. Apps must only
  drain while their own surface is the intended target (no focused lens
  text widget).
- Every lens context grows by ~1 MiB of staging.
- No ABI break: symbols are additive.
- Consumers: synapse `synapse-app` (ADR-0010 there).

## References

- ADR-0036 (host clipboard interface), ADR-0013 (input snapshot).
