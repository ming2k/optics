# ADR-0035: Lens accessibility semantic tree — per-node records and post-end walk

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the accessibility data model and
  export.

## Context

A headless UI toolkit must expose its widget tree to assistive
technology (AT-SPI on Linux, UIAutomation on Windows, NSAccessibility on
macOS). Lens is headless and links no AT library itself; the platform
layer (iris, [ADR-0043](0043-iris-foundations.md)) owns the transport.
The question is what lens exposes and how the platform layer reads it.

Forces:

1. **Same host separation as input and text.** Lens calls no
   assistive-technology API; the platform layer does
   ([ADR-0029](0029-lens-interaction-model.md), [ADR-0033](0033-lens-text-seam.md)).
2. **Per-frame, post-layout.** Semantics need solved bounds (`final_rect`)
   and a stable parent chain, both only available after `lens_end`.
3. **Decorative vs. semantic.** Many nodes (spacers, layout-only
   containers) should not be surfaced; only nodes carrying a role are
   visited.
4. **Arena-owned strings.** A caller's stack/loop-local label must
   survive the post-end walk, so name/value strings are copied into the
   per-frame arena.

## Decision

1. **Per-node `lens_semantics` record.** Each node carries
   `(role, name, value, flags)` for the frame. Widgets call
   `lensi_node_semantics` during build to populate it. Implemented in
   `libs/lens/src/a11y/semantics.c`.
2. **Role enum.** `lens_role` covers the widget kinds AT cares about:
   `NONE` (decorative), `GROUP`, `LABEL`, `BUTTON`, `CHECKBOX`,
   `SLIDER`, `DISCLOSURE`, `SCROLLAREA`, `TEXTFIELD`, `TEXTAREA`,
   `MENU`, `RADIO`, `DIALOG`.
3. **State flags.** `LENS_A11Y_FOCUSED`, `_DISABLED`, `_CHECKED`,
   `_EXPANDED`, `_READONLY` mirror the cross-platform AT state set.
4. **Public override hook.** `lens_a11y(ui, &desc)` enriches or overrides
   the most recently built widget's semantics — for icon-only controls
   whose visible label is not the accessible name. Zeroed fields are
   left; `flags` are OR'd on.
5. **`lens_accessibility_walk`.** A read-only pre-order walk, valid
   between `lens_end` and the next `lens_begin`. The visit callback
   receives `(semantics, bounds, id, parent_id, user)`. The walk skips
   decorative nodes; descendants nest under their nearest semantic
   ancestor. Lens itself calls no AT API.
6. **Overlay sub-roots visited explicitly.** Overlay layers
   ([ADR-0037](0037-lens-overlay-layers.md)) are not children of
   `ui->root`, so the walk visits each registered layer under root id 0.
7. **Strings arena-copied.** `name` is truncated at `"##"` and copied
   into the per-frame arena so a caller's short-lived label survives the
   post-end walk.

References: `libs/lens/include/lens/lens.h` (Accessibility semantics
section), `libs/lens/src/a11y/semantics.c`, `libs/lens/src/internal.h`
(`lensi_node_semantics`, node `semantics` field).

## Alternatives Considered

- **Lens links AT-SPI directly.** Reject: breaks headless parity and the
  host-separation principle; AT-SPI is a Linux-only D-Bus API.
- **Platform layer re-derives semantics from the node tree.** Reject:
  the platform layer cannot know each widget's role; the widget is the
  source of truth.
- **Synchronous AT calls during build.** Reject: AT queries need solved
  bounds (post-layout) and must not re-enter build.
- **String keys into a property bag.** Reject: the struct + role enum is
  statically typed, cheaper, and maps cleanly to AT role enumerations.

## Consequences

Positive:

- Lens stays headless and AT-free; the platform layer
  ([ADR-0043](0043-iris-foundations.md)) consumes one stable walk.
- Decorative nodes do not pollute the AT tree.
- The override hook handles icon-only controls without per-widget
  plumbing.

Negative:

- The walk is only valid in the post-end window; calling it during build
  returns stale or empty data.
- Adding a new widget means remembering to assign a role or it is
  invisible to AT.

## References

- [ADR-0024](0024-lens-foundations.md) — headless foundations.
- [ADR-0029](0029-lens-interaction-model.md) — same host-separation
  principle.
- [ADR-0037](0037-lens-overlay-layers.md) — overlay sub-roots visited by
  the walk.
- [ADR-0043](0043-iris-foundations.md) — the AT-SPI consumer.
