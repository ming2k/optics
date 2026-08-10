# ADR-0062: Bidirectional accessibility — a11y action invocation and text-changed events

- Status: Accepted
- Date: 2026-08-10
- Scope: lens (action injection seam) + iris (AT-SPI Action/Text wiring).
  Extends [ADR-0035](0035-lens-accessibility-tree.md) (the semantic tree
  becomes writable-through-action) and follows the audit-fix tranche that
  made the AT-SPI bridge protocol-correct (Event.Object, role constants,
  diff coverage).

## Context

The accessibility stack so far is **read-only**: AT clients (orca) can
discover the tree, read names/roles/states, and receive focus/structure
events — but cannot *act*. Two gaps block real usage:

1. **No action invocation.** AT-SPI exposes `org.a11y.atspi.Action`
   (`GetNActions`/`DoAction`) and the bridge advertises it, but a
   `DoAction("click")` has no path into lens. A screen-reader user can
   hear a button announced yet cannot press it.
2. **No text-changed events.** Typing in a textfield changes
   `lens_semantics.value`, and the bridge's diff reports it as a
   `PropertyChange:accessible-value` at best — but orca's typing echo
   listens to `Event.Object::TextChanged`, so text input is silent.

Forces:

1. **Same host separation as everything else** (ADR-0029/0033/0035):
   lens calls no AT API; iris owns the transport. Action requests must
   enter lens as *interaction*, through the same arbitration path as
   pointer/keyboard input — not by poking widget internals from the
   bridge.
2. **One-frame latency is the house model** (ADR-0029): activation
   applies to the frame after the request, like every other input.
3. **AT activation is intentional, not spatial.** A screen-reader user
   activating a control is not aiming a pointer; occlusion bands are
   irrelevant to them. `disabled` is still respected — an unavailable
   control must not activate.
4. **Text-changed needs delta shape.** AT-SPI's `TextChanged` carries
   (kind, offset, length, text); the bridge diff must compute common
   prefix/suffix between prev/current values, not just flag a change.

## Decision

1. **`lens_a11y_activate(ui, id)` — public lens seam.** The host (iris's
   AT bridge) requests activation of a widget by id. Lens records a
   single pending activation id; during the next frame's build,
   `lensi_interact` reports `clicked` for that node if it is focusable
   and not disabled — sharing the pointer/keyboard response path, so
   every widget gains AT activation without per-widget code. The pending
   id is cleared on consumption or at frame end. Pointer occlusion does
   not block it; `disabled` does.
2. **iris wires `Action::DoAction`.** The AT-SPI bridge's
   `DoAction("click")` (the only advertised action) resolves the
   accessible's lens id and calls `lens_a11y_activate`. Runs on the main
   thread inside the pump, so no cross-thread hand-off is needed; the
   effect lands next frame, like all input.
3. **Text-changed events from the existing diff.** The bridge computes
   the common prefix/suffix between the previous and current `value`
   strings of TEXTFIELD/TEXTAREA nodes and emits
   `Event.Object::TextChanged` (`"insert"`/`"delete"`, offset, length,
   variant text). Pure bridge-side work; lens's semantics payload is
   already sufficient.
4. **No Value write interface yet.** `SetCurrentValue` on sliders is
   deferred: it requires per-widget write paths (not just activation),
   and no consumer scenario is on the table. Listed here so the
   extension point is named, not built.
5. **Verification.** lens: unit tests pin activation semantics
   (click-through, disabled respected, one-frame latency, focus moves).
   iris: the bus-level harness pattern established in the audit-fix
   tranche (build a real bridge, drive it with busctl/pyatspi) verifies
   `DoAction` end-to-end and the TextChanged payload shape.

Implementation: `libs/lens/src/input/input.c` (`lensi_interact`
consumption), `libs/lens/src/core/context.c` (pending-id lifecycle),
`libs/lens/include/lens/lens.h` (seam declaration),
`libs/iris/src/a11y_atspi.c` (DoAction + TextChanged),
`libs/iris/src/a11y_util.c` (prefix/suffix delta).

## Alternatives Considered

- **Synthesize a pointer click at the widget's centre.** Reject:
  fragile under occlusion, scroll, and animation; fakes the wrong
  modality (a11y activation is not a mouse event); untestable headless.
- **Bridge pokes widget state directly (toggle bits, call internal
  handlers).** Reject: violates the host-separation principle — iris
  would learn widget internals; the interaction path is the single
  source of truth for "a control was activated".
- **Full Action/Value surface now (SetCurrentValue, expand/collapse
  actions).** Deferred: activation covers the operating majority; the
  rest needs per-widget write paths that should be designed against a
  real AT client session, not anticipated.
- **TextChanged via lens-side event stream.** Reject: lens semantics
  are per-frame snapshots by design (ADR-0035); the bridge already owns
  prev/current state for diffing — deltas are transport-side
  information.

## Consequences

Positive:

- Screen-reader users can operate the UI, not just hear it; the a11y
  story becomes bidirectional.
- One seam (`lens_a11y_activate`) covers every focusable widget — no
  per-widget adoption cost, consistent with the interaction-model
  single-path invariant.

Negative / invariants:

- The pending-activation path must respect `disabled` and must not
  fire twice for one request; tests pin both.
- AT activation bypasses occlusion by design — a modal-pinned dialog's
  controls remain activatable through AT even when visually occluded.
  Accepted: AT users navigate the semantic tree, not pixels. The modal
  focus trap (ADR-0039) still governs keyboard focus.
- UIA (Windows) and NSAccessibility (macOS) bridges remain stubs; this
  seam is designed so both can call the same `lens_a11y_activate`
  without lens changes.

## References

- [ADR-0035](0035-lens-accessibility-tree.md) — semantic tree, extended.
- [ADR-0029](0029-lens-interaction-model.md) — one input path,
  one-frame latency.
- [ADR-0039](0039-lens-modal-dialog.md) — focus trap interaction.
- [ADR-0043](0043-iris-foundations.md) — iris owns the AT transport.
- AT-SPI `org.a11y.atspi.Action` / `Event.Object::TextChanged` (D-Bus).
