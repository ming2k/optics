---
id: ADR-0134
title: "Material and scene ownership is attributed to its own library, not to flux"
status: accepted
date: 2026-11-02
scope: [prism, vista, flux, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0134: Material and scene ownership is attributed to its own library

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `prism` materials, `vista` scene graph
- Informed: Core Engineering
- Depends on: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md)

---

## Context and Problem Statement

ADR-0129 instrumented `flux`, `lens` and `glyph`, and ADR-0130's guardrail
mapped every public kind to a lane. Two libraries were left with lane entries
but no ledger points: `prism` and `vista`.

The consequence was not "no report" — it was **a wrong report**. A
`prism_mica_filter` is allocated with `calloc` and freed with `free`, so nothing
counted it. But it holds frame-slot images and pipelines, which are flux objects
and therefore counted. A leaked Mica filter therefore appeared in the teardown
report as:

```
leak scope=device kind=vk_image owner=flux/core live=3 bytes=16777216
```

Every word of that row is true and every word points the reader at the wrong
library. Whoever opens it starts in `flux`, finds nothing wrong, and loses the
time the report was supposed to save. **Misattribution is worse than silence**,
because silence at least does not misdirect.

`vista` had the same shape plus a structural gap: `vista_scene` did not retain
its `flux_device`, so at release time it had no way to reach the ledger at all.
That is the reason it was skipped, and it is a defect in its own right — a
refcounted object that cannot answer "which device do I belong to?" cannot
participate in any device-scoped contract.

## Decision Drivers

1. A report row must name the library whose code the reader should open.
2. Kinds must remain the unit the guardrail maps to lanes, so attribution needs
   per-material granularity, not one generic "filter".
3. No new allocation and no new lifetime: the row must ride on state the object
   already owns.
4. Fixing attribution must not require the reader to cross-reference a second
   table to learn what `filter_mica` is.

## Architectural Decisions

### 1. Every material filter is its own ledger kind

`prism` gains `libs/prism/src/prism_observe.h` with one kind literal per
material:

```
filter_mica  filter_frosted  filter_acrylic  filter_liquid_glass  filter_backdrop_layer
```

Owner is `prism/material`, scope `DEVICE`, lifetime `CONTEXT` (a filter outlives
frames and dies with the app's material set).

Per-material kinds rather than one kind plus a material field: the ledger's kind
is the unit a leak lane maps to and the unit a reader acts on. "The Mica filter
leaked" is actionable; "a filter leaked" requires another lookup.

### 2. `vista_scene` retains its device

`vista_scene` gains a retained `flux_device *`. This is required, not
cosmetic — without it the scene cannot settle its own row, which is exactly why
it had none. The retain is dropped at final release, after the row is settled,
so the report is emitted while the device is still alive.

Kind `scene`, owner `vista/scene`, scope `CONTEXT`, lifetime `CONTEXT`.

### 3. Lane entries now point at exact-delta tests, not at neighbouring tests

`tools/config/leak-lanes.toml` mapped prism and vista kinds to
`test_mica.c` / `test_scene_mesh.c` — tests that exercise the material but
never asserted the ledger. That is coverage in name only, the same failure ADR-0130
was written to eliminate.

The prism kinds now point at `tests/prism/integration/test_prism_material_leak.c`,
which asserts per kind:

- the prism row rises by exactly one on create and returns to exactly its
  previous count on release;
- the `vk_image` and `vk_pipeline` rows it borrows also return to their previous
  counts.

The second assertion is the one that matters: before this ADR it was the *only*
half that could be observed.

## Alternatives Considered

### 1. Count prism objects at the `flux_internal_alloc` funnel
- **Why considered**: zero call-site work, if prism used the funnel.
- **Rejected on fact**: prism uses `calloc`/`free` directly, so it never crosses
  the funnel. (That is a separate defect against the `flux_device_alloc`
  contract in `core.h:711`, recorded below, not silently fixed here.)

### 2. One `filter` kind with a material tag
- **Why considered**: fewer kinds, less lane bookkeeping.
- **Rejected**: the kind is the reader's entry point. A tag requires a second
  lookup at exactly the moment the reader is trying to find out what went wrong.

### 3. Give `vista_scene` a device back-pointer without retaining it
- **Why considered**: avoids a refcount cycle risk.
- **Rejected**: a borrowed device can be destroyed before the scene, so the
  release path would dereference freed memory — a use-after-free added to the
  leak detector. Retaining is the only sound option.

## Consequences

### Positive
- A prism or vista leak is reported against prism or vista, with a kind the
  reader can act on.
- The lane ledger's prism and vista entries now assert something. `[INV-ARCH-LEDGER]`
  stops being true-by-vacuity for half the tree.
- `vista_scene` gains a device association it needed for any device-scoped
  contract, not just this one.

### Negative & Trade-offs
- Five new kinds and one new owner string: the closed vocabulary in
  `<flux/observe.h>` must be updated in the same change, which is the friction
  by design.
- `vista_scene` now holds a device reference, so a scene outliving its device
  keeps the device alive. That is the correct ownership answer and also a
  behaviour change for any host that relied on scene destruction to drop the
  device — none does, because the device was always released last.
- Prism's filters still bypass the caller's allocator. This ADR does not fix
  that, and does not pretend to.

## Recorded, not fixed here

`libs/flux/include/flux/core.h:711` states that peer code should use
`flux_device_alloc`/`flux_device_free` "instead of libc malloc/free … so the
caller's allocator is honoured". Measured compliance:
`prism` 5 libc allocations, `lens` 4, `glyph` 32, all zero `flux_device_alloc`.
That is a real defect against a written contract, it is orthogonal to leak
detection, and it needs its own change with its own measurement (allocation
count, OOM-injection behaviour). It is recorded here so it is not lost, and
deliberately not bundled into this one.

## Acceptance Criteria

1. A leaked prism filter is reported with `owner=prism/material` and its
   material's kind, verified by `test_prism_material_leak` asserting the row
   directly.
2. A leaked scene is reported with `owner=vista/scene`.
3. `tools/gate check leaks` is green, with prism and vista lanes pointing at
   exact-delta tests.
4. The kind vocabulary in `<flux/observe.h>` lists the new kinds.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md),
  [ADR-0132](0132-ledger-derived-debug-names.md)
