# ADR-0026: Lens widget identity — FNV-1a hashing over an id stack

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines how widgets get stable ids across
  frames.

## Context

Lens reconciles per-frame immediate-mode build calls against a retained
store keyed by a `lens_id`. For state (animation, focus, per-node user
data) to survive across frames, the id computed for "the Save button"
must be the same id every frame — independent of sibling order, parent
layout changes, or how many siblings appear or disappear.

Forces:

1. **Stability across frames.** The same `(scope, label)` must hash to
   the same id for the lifetime of a widget.
2. **Sibling-order independence.** Reordering siblings must not change
   their ids.
3. **No global registry.** Ids must be computable locally, without
   consulting other widgets.
4. **Empty-label safety.** An empty label must not collide with its
   parent container's id.

## Decision

1. **64-bit FNV-1a hash over an id stack.** `lensi_hash` mixes a byte
   sequence into a `uint64_t` seed using the FNV-1a offset basis and
   prime. The top of the id stack is the seed; pushing an id hashes it
   into a new top. Implemented in `libs/lens/src/core/id.c`.
2. **Widget id = `(scope, label)`.** `lensi_gen_widget_id` hashes the
   full label string (including any `"##key"` suffix) against the
   current id-stack top. The id is stable across frames and independent
   of sibling order.
3. **Container id = `(scope, kind, sibling-seq)`.** Containers carry no
   label, so a per-parent sequence counter disambiguates siblings and
   gives each loop iteration's container a distinct scope automatically.
4. **Empty labels use a sentinel.** Hashing an empty label would yield
   the raw scope — exactly the container's id — and let an empty-label
   widget steal its parent's node. `lensi_gen_widget_id` substitutes
   `"##__flux_empty__"` so empty labels scope *under* the container
   instead of replacing it.
5. **Zero is reserved.** `0` means "no id"; every generator maps a zero
   hash to `1`.
6. **Public id stack.** `lens_push_id` / `lens_push_id_int` /
   `lens_pop_id` / `lens_current_id` let callers add stable scopes (e.g.
   inside a loop over data) without depending on labels.

References: `libs/lens/include/lens/lens.h` (Identity section),
`libs/lens/src/core/id.c`.

## Alternatives Considered

- **Sequential ids from a counter.** Reject: not stable across frames —
  a missing sibling shifts every later id and re-binds state to the
  wrong widget.
- **Pointer-based ids.** Reject: addresses change between frames and
  break retained state.
- **String interning.** Reject: requires a global registry, allocation,
  and lookup per widget; FNV-1a over the stack is local and allocation
  -free.
- **Hashing only the visible label prefix (before `"##"`).** Reject: the
  `"##key"` suffix is the documented way to give two same-label widgets
  distinct ids; it must seed the hash even though it is not painted.

## Consequences

Positive:

- Widget state survives reordering, insertion, and deletion of siblings.
- The id stack composes naturally: a loop body can `lens_push_id_int(i)`
  to give each iteration a stable scope.
- No allocation on the id path.

Negative:

- Hash collisions are possible in principle (64-bit FNV-1a); mitigated
  by the open-addressing store
  ([ADR-0027](0027-lens-retained-store.md)) treating the id as an opaque
  key.
- The `"##key"` convention and the empty-label sentinel are now
  load-bearing; changing them silently breaks retained state.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0027](0027-lens-retained-store.md) — store keyed by `lens_id`.
