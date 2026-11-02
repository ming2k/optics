---
id: ADR-0132
title: "Debug names are ledger-derived per-instance handles, not stack-buffer snprintf"
status: accepted
date: 2026-11-02
scope: [flux, prism, vista, lens, glyph, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0132: Debug names are ledger-derived per-instance handles

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `flux` core (device, allocator, image, buffer, sampler, pipeline, target)
- Informed: Core Engineering
- Depends on: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md)
- Amends: [ADR-0003](0003-bindless-handle-packing.md) (does not change the
  handle format; changes only how the debug name is produced)

---

## Context and Problem Statement

`FLUX_OBSERVE_NAMES` was declared as a capability in ADR-0129 and shipped
unimplemented: `flux_observe_object_name` had zero callers, so the capability
bit documented a diagnostic that could never appear. ADR-0130's guardrail grew
a capability-consumer check specifically because of it.

Implementing it naively would harden an existing defect. Every current call
site passes a **stack buffer** to `vkSetDebugUtilsObjectNameEXT`:

```c
char name[80];
snprintf(name, sizeof(name), "flux_image %ux%u fmt=%d", desc->width, desc->height,
         (int)desc->format);
flux_vk_set_name(d, VK_OBJECT_TYPE_IMAGE, (uint64_t)im->image, name);  /* name dies here */
```

`flux_vk_set_name` hands `name` straight to
`VkDebugUtilsObjectNameInfoEXT::pObjectName`. Vulkan requires that pointer to
remain valid for the duration of the call; the call returns before the stack
frame dies, so in practice drivers copy the string and this has never been
observed to fail. "Driver copies it" is not a contract, and the pattern is one
`memcpy` inside a driver away from reading freed stack — unreproducible, in the
one code path that exists to make bugs reproducible.

Two further problems make the obvious fix wrong:

1. The names are **not unique**. Every 512x512 RGBA8 image gets
   `"flux_image 512x512 fmt=..."`. Under RenderDoc or a validation capture, a
   hundred indistinguishable objects is barely better than none. The missing
   information is *which instance*, not *which kind*.
2. They are **device-expensive**. `vkSetDebugUtilsObjectNameEXT` is a driver
   call, executed once per object creation. `flux_buffer` and `flux_image` are
   created in bulk during asset load; paying a driver round-trip per object to
   produce a non-unique string is cost without benefit.

## Decision Drivers

1. A debug name must be unique per live object, or it cannot answer the only
   question it exists for: "which one leaked?"
2. No pointer handed to the driver may outlive its storage. This is a
   correctness requirement, not a style preference.
3. The cost must be zero when `FLUX_OBSERVE_NAMES` is off — which is the
   default build — and must not add allocation to any path.
4. The ledger is already the authority on what is live. Names should be a
   projection of it, not a parallel bookkeeping system.

## Architectural Decisions

### 1. Names live in the ledger row, in a bounded ring, keyed by instance id

Each `flux_observe_row` gains a fixed-capacity ring of live instance ids for
its kind:

```c
#define FLUX_OBSERVE_NAME_RING 32u

typedef struct flux_observe_row {
    ...
    /* Live instance ids for this kind, newest last. Populated only when
     * FLUX_OBSERVE_NAMES is on. A ring, not a table: bounded memory, no
     * allocation, and the oldest entry is overwritten on overflow (reported,
     * never silent). */
    uint32_t name_ids[FLUX_OBSERVE_NAME_RING];
    uint32_t name_top;
    uint32_t name_overflow;
    ...
} flux_observe_row;
```

`flux_observe_acquire` returns the instance id it assigned
(`uint32_t`, monotonically increasing per device). That id is the object's
identity in the ledger, and the basis of its debug name.

### 2. The name is *rendered into device-owned storage*, then handed to Vulkan

The lifetime problem is solved by rendering into a buffer that outlives the
call by construction. `flux_vk_set_name` gains a "derive from ledger" entry
point that owns its storage:

```c
/* Renders "<kind>#<id> <kind-specific detail>" into the ledger row and hands
 * that row's buffer to the driver. The row outlives every object of its kind
 * until the row is reused, and the ledger is torn down after the device, so
 * the pointer stays valid for the whole call and for the driver's lifetime
 * bookkeeping. Returns false when NAMES is off. */
bool flux_observe_name_object(flux_device *d, VkObjectType type, uint64_t handle,
                              flux_scope scope, const char *kind, const char *owner,
                              uint32_t instance, const char *detail);
```

The existing `flux_vk_set_name` keeps its signature for callers that already
own a stable literal (the slab allocator passes a static-ish derived string
today), but every call site that passes a `char buf[N]` is converted to the
ledger path. The stack-buffer pattern is deleted, not documented.

### 3. Zero cost and zero literals when the capability is off

`flux_observe_name_object` compiles to `return false` when
`FLUX_OBSERVE_ATTRIBUTION` is absent, and the name-format template is behind
the same guard. `tools/optics check leaks` already verifies the template is
absent from a non-attribution build; that check now covers a real,
live implementation rather than a stub.

### 4. Instance ids are per device, not process-global

A per-device counter keeps names stable within one device's lifetime and avoids
a process-global atomic in the object-creation path. Two devices in one process
therefore produce two `flux_image#1`s; the capture is per device, so that is
the right scope — and the ledger row is per device as well, so the name and the
row agree.

## Alternatives Considered

### 1. Keep the stack-buffer snprintf and only add the ledger
- **Why considered**: smallest change; the names already "work".
- **Rejected**: it preserves the dangling-pointer hazard and keeps names
  non-unique, so the capability would still not answer "which object leaked".

### 2. Allocate a string per object
- **Why considered**: exact per-object names with unbounded detail.
- **Rejected**: allocation on the object-creation hot path, and a second
  lifetime to manage — the ledger exists precisely to avoid that. A bounded
  ring with a reported overflow is honest; a heap string is a new leak vector
  inside the leak detector.

### 3. Generational handles so the name can be `kind ^ generation`
- **Why considered**: it is the same idea as ADR-0131's slot map.
- **Rejected**: the handle is the caller's opaque pointer; there is no id to
  embed. A ledger-assigned instance id costs one counter increment.

## Consequences

### Positive
- Names are unique per live object and derived from the same authority that
  reports leaks, so a capture and a leak report agree on identity.
- The dangling-stack-pointer pattern is deleted across all five call sites.
- `FLUX_OBSERVE_NAMES` acquires a consumer, so ADR-0130's guardrail stops
  reporting it as a document-only switch.
- A default build gains nothing new to carry: the whole path is behind
  `FLUX_OBSERVE_ATTRIBUTION`.

### Negative & Trade-offs
- 32 ids per kind ring is a bound, not a guarantee: a kind with more than 32
  live instances reports overflow and the oldest names are lost. That is stated
  in the report rather than hidden.
- Names are only as detailed as the caller-supplied `detail` string, which must
  come from a fixed vocabulary for the same reason sites do.
- Call sites that used a computed detail (image dimensions) must either drop
  the detail or express it as a small bounded vocabulary; this ADR chooses to
  drop free-form detail from the *name* and keep it in the ledger row's byte
  and count columns, which are already reported.

## Acceptance Criteria

1. `flux_observe_object_name` has a caller; `FLUX_OBSERVE_NAMES` has a consumer
   outside the ledger, verified by `tools/optics check leaks`.
2. No call site passes a stack buffer to a debug-name function; `git grep`
   for the pattern is empty and the guardrail fails if it reappears.
3. A capture from an attribution build shows distinct names for two objects of
   the same kind and shape.
4. `tools/optics check leaks` is green, and the non-attribution build still
   contains zero name-format machinery.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md),
  [Architecture](../dev/standards/architecture.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0003](0003-bindless-handle-packing.md),
  [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md)
