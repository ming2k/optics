# ADR-0073: Widget-kind extension range and the user-widget contract

- Status: Accepted
- Date: 2026-08-22

## Context

`lens_widget_kind` is a closed enum ending in `LENS_WIDGET_KIND_COUNT`, and
five separate arrays are sized by that count (`lens::skins`,
`lens::skins_userdata`, `lens::skins_user`, plus validation bounds scattered
through `skin.c`). The public API offers no way to register a widget the
library did not ship: a host building, say, a color picker or a dial today
must compose it out of containers + `lens_pressable_begin` +
`lens_node_state`, and cannot give the result a stable *widget identity* —
it has no `lens_widget_kind` of its own, so it cannot carry a skin, appear
in `lens_widget_record`-driven tooling, or be re-skinned independently of
whatever built-in kind it borrowed.

Reviews of the API surfaced this as the largest closed extension point in
lens. Two forces pull against simply opening the enum:

1. `LENS_WIDGET_KIND_COUNT` is used as an array bound *inside the library*.
   Caller-supplied values flowing into `lens_set_skin` must not be able to
   index past those arrays.
2. `lens_widget_record.content` is a flattened kind-tagged union whose
   per-kind fields are documented per widget; a caller-invented kind has
   no entries there, and inventing a general "user content" payload is a
   much larger design (it wants a `next`-chain like `flux_paint`) than the
   identity problem does.

## Decision

1. The upper half of the kind space is reserved for hosts:
   `LENS_WIDGET_KIND_USER_BASE = 0x4000_0000`. Values from
   `LENS_WIDGET_KIND_USER_BASE` through `0xFFFF_FFFF` are never assigned
   by lens; they are free for the host's own composite widgets. Values
   below the base remain library-assigned and append-only.
2. `lens_set_skin` / `lens_set_skin_userdata` / `lens_default_skin` accept
   user-range kinds: registration lands in a host-owned table, not the
   library's count-sized arrays. The built-in default for a user kind is
   `NULL` (no emission — the host's skin *is* the default).
3. The composition path stays as-is: a user widget is still built from
   `lens_pressable_begin` + `lens_node_state`, but it may now stamp its
   own kind into the record it passes to the skin seam, so its skin is
   selectable and replaceable exactly like a built-in's.
4. `lens_widget_record.content` is NOT extended for user kinds in this
   ADR. User content rides in the host's own state (`lens_node_state`) or
   the skin's `user` pointer (ADR-0059's closure slot, shipped alongside
   this decision). If a use case demands record-visible user content, that
   arrives as a `next`-chained extension struct — the pattern flux's
   `flux_paint` now uses — in a separate decision.

## Alternatives Considered

- **Open registration API (`lens_widget_register(measure, interact, emit)`).**
  Rejected for now: measure/interact/emit hooks into the retained store
  would let hosts create nodes the library's layout, GC (ADR-0038), and
  a11y walk (ADR-0035) do not know about. That is a genuine plugin
  architecture with a much larger correctness surface — worth doing only
  with a concrete consumer. This ADR ships the identity/skin half, which
  is safe today, and leaves the vtable half explicitly out.
- **Widen `lens_widget_record.content` with a `user` union arm.** Rejected:
  a void* in a public record makes the record's lifetime rules host-owned,
  which fights the record's by-value emission design.
- **Do nothing (composition only).** Rejected: identity is the one thing
  composition cannot synthesize, and skin selection keyed on borrowed
  built-in kinds makes two different composites fight over one override.

## Consequences

- Positive: hosts get stable, collision-free widget identity and full skin
  participation; the library's count-sized internals are untouched.
- Positive: the reserved range is a pure header change — no ABI movement
  of any struct.
- Negative: user kinds still have no library-side measure/interact
  behavior; a user widget is only as accessible as the composites it is
  built from. Recorded here so the limitation is deliberate, not silent.
- The `LENS_WIDGET_KIND_USER_BASE` boundary is asserted in `lens.h`
  (static_assert) alongside the existing ABI guards.
