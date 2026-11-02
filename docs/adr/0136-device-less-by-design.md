---
id: ADR-0136
title: "Device-less by design: the process-level icon registry and the parser behind it allocate from libc, and say so"
status: accepted
date: 2026-11-02
scope: [lens, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0136: Device-less by design

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `lens` icon runtime, SVG parser, mask cache, replay
- Informed: Core Engineering
- Depends on: [ADR-0135](0135-host-allocation-attribution-and-contract.md)

---

## Context and Problem Statement

ADR-0135 requires that a library owning a device allocate through
`flux_device_alloc`, so the consumer's allocator is honoured and the memory is
attributed. Its guardrail immediately found 18 further sites beyond the ones
originally planned, in five files:

- `lens/src/icon_svg.c` (13) — the SVG parser
- `lens/src/icon_runtime.c` (5) — the process-level icon registry
- `lens/src/render/icon_mask.c` (7) — the coverage-mask cache
- `lens/src/render/replay.c` (6) — scene snapshot replay
- `glyph/src/face.c`, `glyph/src/glyph_cache.c` (8) — already-migrated TUs whose
  remaining sites need classification

The naive answer — route them all through the funnel — is wrong for a subset,
and "wrong" here is not a style objection:

**`lens_icon_register_svg(const char *svg_text)` has no `lens` and no device.**
It registers an icon into a **process-global** registry (`g_runtime_icons`)
that is deliberately context-free: an icon registered before any window exists
must remain valid across every later context, and must survive a context being
destroyed. The parser it calls, `lensi_svg_parse`, likewise takes text only.

Two consequences follow:

1. There is no device to pass. Inventing one (a "current device" global) would
   make registration depend on creation order — a worse bug than the one being
   fixed.
2. The memory it produces **escapes into a process-global structure** and is
   freed by `lens_icon_*` teardown, not by the context that happens to be alive.
   Attributing it to a device ledger would be a category error: it outlives every
   device by design.

The mask cache and replay buffers are different: they *are* context-bound, live
inside a `lens`, and must be migrated. Distinguishing these cases is the whole
point of this ADR.

## Decision Drivers

1. "Uses libc" must mean something specific, or the guardrail degrades into an
   allowlist that grows until it is meaningless.
2. A rule with an exception needs the exception stated as an **architectural
   property** (device-less by design), not as a filename.
3. Process-lifetime memory must not be attributed to a device-scoped ledger,
   because a device-scoped row that outlives its device is unrepresentable.

## Architectural Decisions

### 1. Process-lifetime registries declare themselves in the ledger

`lens`'s icon registry is not exempt from accounting — it is counted in the
**process** ledger (`FLUX_SCOPE_PROCESS`), which is exactly what that scope
exists for (ADR-0129 decision 1: fd, wl_proxy, and other objects created before
any device). The registry's own bookkeeping (`g_runtime_icons`, growth,
per-icon `cmds`/`runs`) is counted there.

Concretely: `lens_icon_register_svg` acquires a `FLUX_SCOPE_PROCESS` /
kind `icon` row per registered icon, and the corresponding `lens_icon_*`
teardown releases it. The parser's transient buffers inside that call are
bounded by the call and are already freed on every error path, so they carry no
row: a row that is acquired and released inside one function call is a
counter that can only mislead.

### 2. The guardrail recognises "device-less by design" as a compile-time property

The raw-allocation check (ADR-0135) accepts a raw allocation only when the line
carries `RAW_ALLOC_OK` **and** the reason names the property, in one of a closed
set:

```
RAW_ALLOC_OK(process-registry)   — memory owned by a process-lifetime registry
RAW_ALLOC_OK(no-device)          — function provably has no device parameter
RAW_ALLOC_OK(transient)          — acquired and released within one call
```

The marker is not a filename exemption: a new libc call inside
`lens/src/render/replay.c` (a context-bound file) cannot silence itself by
adding the marker, because `replay.c` is required to have a device and the
guardrail checks that the marker is only used in files it classifies as
device-less. The classification is a short, reviewed list in the guardrail with
a written justification per entry — the same shape as
`tools/config/abi-supprs.abignore`.

### 3. Context-bound sites migrate

`icon_mask.c` and `replay.c` operate on a `lens` and therefore have a device.
They migrate to `flux_device_alloc`/`flux_device_free` with
`owner="lens/core"` and kinds `"icon_mask"` and `"snapshot"`.
`icon_mask.c` already has `lens *` in reach; `replay.c`'s snapshot allocation
gains one where it does not.

### 4. `glyph`'s remaining sites are classified, not hand-waved

- `glyph_ctx` itself (`face.c:327`, `:493`) precedes any device association and
  is freed after `flux_device_release`; it is `RAW_ALLOC_OK(no-device)`.
- `txtp_charset` and `f->path` are produced by platform helpers
  (`strdup`, `txtp_charset_new`), which are in the exempt platform TUs; the
  *free* sites remain libc because the matching allocator is the platform one.
  That is a paired-allocator rule, not an exemption: freeing platform memory
  with the device allocator would corrupt the heap.
- `gc_alloc`/`gc_free` keep their `calloc`/`free` fallback for a device-less
  cache, which is `RAW_ALLOC_OK(no-device)`.

## Alternatives Considered

### 1. Give the icon registry a device
- **Why considered**: one rule, no exceptions.
- **Rejected on semantics**: registration happens before any window exists and
  must outlive every context. A device would make it order-dependent and would
  put a device-lifetime row on process-lifetime memory.

### 2. Count the registry in a device ledger anyway
- **Why considered**: keeps one scope.
- **Rejected**: a device-scoped row cannot be reported after that device is
  destroyed, and the registry provably outlives it. The report would go silent
  for the only lifetimes it describes.

### 3. Drop the raw-allocation check and rely on review
- **Why considered**: the exceptions are awkward.
- **Rejected**: the check found 18 sites that review had missed across three
  previous passes, including in files this project had already "migrated".

## Consequences

### Positive
- Process-lifetime memory is accounted in the scope that can actually report it.
- The raw-allocation check stays meaningful because its exceptions are
  architectural properties with a closed vocabulary, not filenames.
- The remaining context-bound violations are enumerated and scheduled rather
  than estimated.

### Negative & Trade-offs
- Three markers (`process-registry`, `no-device`, `transient`) must be used
  correctly; a wrong one is a lie the guardrail cannot detect, only narrow.
- The parser keeps libc internally. That is deliberate: its buffers never
  outlive the call, and routing per-call scratch through a device funnel would
  add a ledger acquire/release pair per icon parse for no diagnostic value.

## Acceptance Criteria

1. `lens_icon_register_svg` acquires/releases a `FLUX_SCOPE_PROCESS` row per
   icon, observable via `flux_observe_report(nullptr, scope=PROCESS)`.
2. `icon_mask.c` and `replay.c` contain no raw allocation except lines carrying
   a marker from the closed vocabulary.
3. `tools/optics check leaks` fails on a raw allocation in a context-bound file,
   demonstrated by a negative test.
4. `./tools/optics check` is green.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0135](0135-host-allocation-attribution-and-contract.md)
