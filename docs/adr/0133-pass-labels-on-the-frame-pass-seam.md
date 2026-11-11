---
id: ADR-0133
title: "Pass labels ride the frame pass seam, and the CPU backend is a no-op rather than a special case"
status: accepted
date: 2026-11-02
scope: [flux]
superseded_by: null
negative_knowledge: true
---

# ADR-0133: Pass labels ride the frame pass seam

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `flux` canvas (Vulkan + tile-compute + CPU backends), scene
- Informed: Core Engineering
- Depends on: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md),
  [ADR-0132](0132-ledger-derived-debug-names.md)

---

## Context and Problem Statement

`FLUX_OBSERVE_PASS_LABELS` was the second capability that ADR-0129 declared and
never implemented. It shipped as a bit nothing read, which ADR-0130's
capability-consumer check now fails on. Two options were open: implement it, or
delete it.

The argument for implementing it: object names (ADR-0132) answer "which object
leaked". They do not answer "which pass was executing", and a GPU capture
without section markers is a flat command list whose draws have no provenance.
That is a real gap in the diagnostic story, not a cosmetic one.

The argument against: a label implementation has to reach every backend, and
`flux` has three canvas backends — Vulkan immediate, tile-compute coverage, and
a pure CPU rasterizer with no command buffer at all.

## Decision Drivers

1. One seam, not three. A per-backend labelling call is three places to forget.
2. The CPU backend must not need an `#ifdef`; it simply has no command buffer.
3. Cost when the capability is off must be one predictable branch.
4. The label string must outlive recording, since it lands in a command buffer
   submitted after the caller returns.

## Architectural Decisions

### 1. `flux_frame_begin_pass` / `flux_frame_end_pass` is the single seam

Every GPU backend already routes through this pair: the Vulkan canvas begins a
pass with it, the tile-compute coverage backend begins a pass with it, scene
drawing runs inside it. Labelling here covers all of them without touching a
backend file, and the CPU rasterizer never calls it — its "pass" is a loop over
pixels and it has no command buffer to annotate. No `#ifdef` appears anywhere.

### 2. The label is per-pass data, supplied by the caller

`flux_pass_desc` gains `const char *label`. Null falls back to `"pass"`.

The string must outlive the recording call, and that requirement is stated in
the field's comment rather than assumed: `vkCmdBeginDebugUtilsLabelEXT` copies
the name, but the rule keeps a caller from passing a stack buffer through a
rebuild that later defers recording.

### 3. Function pointers resolved once, gated twice

`pfn_cmd_begin_label` / `pfn_cmd_end_label` are resolved next to
`pfn_set_name` at device creation, and both label entry points check:

- `FLUX_OBSERVE_PASS_LABELS` is set in the ledger's capabilities, and
- `VK_EXT_debug_utils` was actually advertised and the pointers exist.

Double gating matters because the capability can be requested on a driver
without debug utils; the second check turns that into a no-op instead of a call
through a null pointer.

## Alternatives Considered

### 1. Label inside each canvas backend
- **Why considered**: labels could then be finer-grained than a pass.
- **Rejected**: three backends, three call sites, three chances to forget, and
  the CPU backend needs a branch that does nothing. The pass is also the right
  granularity: it is what appears as a section in a capture.

### 2. Use `VK_EXT_debug_marker` as well, for older tooling
- **Why considered**: broader capture-tool compatibility.
- **Rejected**: `VK_EXT_debug_utils` supersedes it and is already required for
  object names. A second parallel mechanism is the dual-authority pattern this
  ADR family keeps rejecting.

### 3. Delete the capability instead of implementing it
- **Why considered**: it was honest and cheap, and it was the initial plan.
- **Rejected on examination**: the capability answers a question nothing else
  answers — provenance of a draw inside a capture. Deleting it would have made
  the ledger complete for objects and silently absent for work.

## Consequences

### Positive
- A capture from an attribution build shows named sections per pass, so a slow
  or wrong-looking command can be traced to the pass that emitted it.
- `FLUX_OBSERVE_PASS_LABELS` has a consumer, so the capability-consumer check
  passes for a real reason rather than a deletion.
- No backend file was touched; the CPU backend has no knowledge of labels.

### Negative & Trade-offs
- One driver call per pass when enabled. Negligible against the pass, but it is
  not zero, so it stays off by default and gated.
- Labels are per pass only. Sub-pass attribution (a specific material inside a
  layered backdrop) would need a second seam and is not attempted here.
- `flux_pass_desc` grows a pointer. It is a chained descriptor with a `type`
  tag, so this is an append and does not disturb the ABI guard.

## Acceptance Criteria

1. `FLUX_OBSERVE_PASS_LABELS` has a consumer outside the ledger, verified by
   `tools/gate check leaks`.
2. An attribution build with the capability set emits
   `vkCmdBeginDebugUtilsLabelEXT`/`End` around each pass; without the capability
   it does not, and without `VK_EXT_debug_utils` the calls are no-ops.
3. A build without attribution contains no label machinery.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0129](0129-observation-plane-and-resource-ledger.md),
  [ADR-0130](0130-leak-coverage-mechanical-gate.md),
  [ADR-0132](0132-ledger-derived-debug-names.md)
