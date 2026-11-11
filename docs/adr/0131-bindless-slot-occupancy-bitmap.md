---
id: ADR-0131
title: "Bindless slots carry an occupancy bitmap: a double release is an error, not a silent alias"
status: accepted
date: 2026-11-02
scope: [flux]
superseded_by: null
negative_knowledge: true
---

# ADR-0131: Bindless slots carry an occupancy bitmap

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `flux` core (bindless heap), canvas, scene, prism
- Informed: Core Engineering
- Depends on: [ADR-0003](0003-bindless-handle-packing.md),
  [ADR-0129](0129-observation-plane-and-resource-ledger.md)
- Amends: [ADR-0003](0003-bindless-handle-packing.md) (adds occupancy
  validation to the release path)

---

## Context and Problem Statement

The bindless heap allocates slots with a per-binding free-list stack
(`libs/flux/src/core/device.c`). Registration pops; release pushes. The only
guard on the push path is:

```c
static void slot_free(flux_bindless_pool *p, uint32_t slot) {
    if (slot >= p->capacity) return;
    if (p->free_top >= p->capacity) return; /* double-free guard */
    p->free_stack[p->free_top++] = slot;
}
```

That guard is not a double-free guard. It only catches pushing when the stack
is already full. A double release with the stack non-full pushes the same slot
twice. From then on, two registrations pop the same slot index and write
different descriptors into it: the first resource is silently re-pointed at
the second's view, or a shader samples one image where it expected another.
The corruption is silent, it is not a crash, and it manifests as a wrong pixel
or a validation-layer error far from its cause.

This is the one place in the stack where slot identity is not protected by a
count. GPU memory, host objects, and file descriptors all have a counted
lifetime whose imbalance is at least observable. A bindless slot does not: the
free stack has no notion of "already free".

## Decision Drivers

1. A defect must be detectable at its own site, not inferred from corruption
   downstream.
2. The fix must be provable by construction, not by a periodic audit.
3. Cost must be bounded and paid per *heap capacity*, not per frame.
4. No change to the handle's wire format is acceptable without a shader-visible
   ABI break; the low-28-bit slot index is consumed by every shader that
   indexes the heap (ADR-0003).

## Architectural Decisions

### 1. Occupancy is explicit state, not an emergent property of the stack

Each `flux_bindless_pool` gains a bitset of `capacity` bits, one per slot:

```c
typedef struct flux_bindless_pool {
    uint32_t capacity;
    uint32_t free_top;
    uint32_t *free_stack;
    uint64_t *allocated; /* capacity bits; 1 = live */
} flux_bindless_pool;
```

Cost at the default capacities (16 384 + 4 096 + 256 + 4 096 slots) is 2 KiB
for the largest pool and 3 KiB total. It is allocated once at heap init and
freed at heap destroy, in the same place the free stack already is.

`slot_free` becomes a total function: it rejects a slot that is not currently
allocated, logs it, and leaves the free stack untouched. The previously
silent corruption becomes a reported error and a stable heap.

### 2. The handle wire format is unchanged

Generational tagging was considered and rejected (see alternatives). Slot
identity stays `(binding << 28) | slot`, so no shader, push-constant struct, or
descriptor write changes. The occupant of a slot may still change while a
stale handle is outstanding — that is a *use-after-release* and remains the
caller's hazard per Vulkan's external-synchronisation rules. What this ADR
removes is the *heap corrupting itself* when a release is duplicated.

### 3. The ledger counts slots

Every slot allocator pop records `FLUX_SCOPE_DEVICE` / `FLUX_LIFETIME_CONTEXT`
/ kind `"bindless"` / owner `"flux/core"` through ADR-0129's ledger, and every
push releases it. A double release therefore also trips the ledger's
unmatched-release counter, which is reported at every teardown, and a leaked
slot shows up as a live row naming the heap. Two independent mechanisms, both
mechanical, neither relying on a comment.

## Alternatives Considered

### 1. Generational tags in the handle (4 bits of generation, 24-bit slot)
- **Why considered**: it is the textbook remedy for ABA on slot indices, and it
  was the reviewer-suggested design.
- **Rejected**: the handle is a *shader-visible* index. `canvas.h` and every
  `*.comp`/`*.frag` that reads the heap would have to mask generations out,
  and every push-constant struct would have to carry the pair. It converts a
  cheap heap-integrity bug into a cross-library shader ABI break for a hazard
  (use-after-release) that generation tags do not actually prevent — a stale
  handle with a matching generation still aliases if the slot wrapped. The
  occupancy bitmap fixes the corruption at zero ABI cost; generation tagging
  fixes a different, rarer problem at maximum ABI cost.

### 2. Poison the datum readmod (encrust the freed slot's descriptor)
- **Why considered**: zero new state.
- **Rejected**: `UPDATE_UNUSED_WHILE_PENDING` means the write itself is legal,
  the stale read is not detectable, and writing a null descriptor on release
  requires the set to be externally synchronised against every in-flight
  command buffer — a stall on the release path, which is worse than the bug.

### 3. Leave the guard as-is and rely on validation layers
- **Why considered**: `VK_LAYER_KHRONOS_validation` already flags some descriptor
  misuse.
- **Rejected**: validation only runs in the debug configuration. The default
  build is the one that ships, and this class of corruption is exactly the one
  that survives to production because it produces plausible pixels.

## Consequences

### Positive
- A duplicated `flux_bindless_release` is refused, logged, and counted, instead
  of permanently aliasing two resources.
- Slot leaks are attributable: the ledger row says how many slots are live at
  teardown, which was previously only inferable from a `live_allocations`
  delta that also counted images, buffers, and memory.
- The heap's state space becomes total: every slot is exactly one of
  allocated or free, and each transition is checked.

### Negative & Trade-offs
- 3 KiB of host memory per device at default capacities. Accepted.
- `slot_free` now has a failure path that must log; it was previously
  infallible. The log is bounded (one line) and cold.
- The occupancy bit is *not* a substitute for a generation tag on stale
  handles. That limitation is now written down instead of assumed away.

## Acceptance Criteria

1. `tests/flux/integration/test_bindless.c` gains a case that releases the same
   handle twice and asserts the second release is refused (slot count
   unchanged, error logged, no duplicate in the free stack).
2. `flux_device_memory_stats` and the ledger agree on the live slot count after
   a soak of register/release pairs.
3. `./tools/gate check` is green.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md),
  [Architecture](../dev/standards/architecture.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0003](0003-bindless-handle-packing.md),
  [ADR-0129](0129-observation-plane-and-resource-ledger.md)
