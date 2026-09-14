# ADR-0089: Rendering programs and dependency-driven execution

- Status: Accepted
- Date: 2026-09-14
- Scope: Flux, flux-text, scene producers, Prism, Lens, Iris, C and Rust APIs.
- Implementation: Complete; verified against acceptance criteria in [the implementation matrix](../dev/architecture-review-2026-09-14.md).

## Context

[ADR-0088](0088-optics-clean-break-architecture-display-lists-orthogonal-brushes-and-transient-graphs.md)
combines recording, immutable data, effects, scheduling, and language safety
without closing their ownership and execution contracts. Adding another
Canvas adapter would preserve two sources of drawing semantics.

The project chooses a source and ABI break. Existing public names, layouts,
behavior, and downstream build compatibility are not constraints on this
replacement. Correct rendering and explicit failure remain constraints.

## Decision

Adopt one authoritative path:

```text
Application state and events
  -> SceneSnapshot (UI and other retained producers)
  -> DisplayList with immutable resource versions
  -> target-specific compilation
  -> RenderPlan
  -> backend execution
  -> Submission and completion
```

A **DisplayList** describes ordered drawing semantics without a device or
target. Direct drawing applications can record one without a SceneSnapshot.
A **RenderPlan** describes the passes, resource accesses, dependencies, and
allocations needed for a particular execution. A **Submission** owns the
in-flight execution dependencies until completion or terminal failure.

The semantic recorder and compiler belong to Flux. Vulkan allocation,
command emission, and synchronization remain backend implementation details.
The CPU executor implements the declared supported semantic subset. This
does not introduce a runtime abstraction across unrelated GPU APIs;
[ADR-0006](0006-no-runtime-rhi.md)'s GPU-backend policy remains in force.

Canvas-style save/restore is optional recorder syntax. It has no independent
immediate execution path. DisplayLists resolve state and own their resource
dependencies. Replay cannot inherit an ambient transform, clip, or brush.
Reusable child lists enter through explicit placement and clipping.

Compilation takes an explicit target contract: extent, logical-to-device
mapping, format, color space, sample policy, load/store intent, capabilities,
and budget. Target dimensions alone do not determine UI scale. A compiled
plan is reusable only while all of its declared assumptions remain valid.
Device-specific realizations never become part of DisplayList semantics.

Scene rendering, uploads, custom compute, and Prism effects participate via
declared plan nodes. They cannot secretly submit writes to plan-managed
resources. Iris owns host integration and presentation; Lens owns UI state;
Prism constructs effects; Flux owns rendering semantics and execution.

### Decision family

| ADR | Contract |
|-----|----------|
| [0090](0090-display-list-resource-ownership.md) | Published data and resource ownership |
| [0091](0091-drawing-group-and-effect-semantics.md) | Drawing, composition, and legal optimization |
| [0092](0092-resource-planning-and-retirement.md) | Dependencies, allocation, synchronization, and retirement |
| [0093](0093-c-state-contracts-and-rust-sessions.md) | C state machines and Rust safety |
| [0094](0094-lens-scene-snapshots-and-invalidation.md) | UI snapshots and incremental updates |

These records form one proposal. They may be reviewed separately; accepting
the architecture requires resolving their shared contracts together.

### Replacement scope

The following changes take effect on acceptance, not while this is Proposed.
Existing accepted records retain their historical text.

| Earlier decision | Proposed disposition |
|------------------|----------------------|
| ADR-0087 | Replace lifecycle categories, Canvas driver, export policy, and caller-managed synchronization. Retain the C23 baseline and typed descriptor initialization; convenience syntax must not create another semantic path. |
| ADR-0088 | Replace in full with ADR-0089 through ADR-0094. |
| ADR-0001 / ADR-0016 | Replace the exclusion of planning from Flux and the raw-peer execution boundary for plan-managed work; retain separation of text/layout from low-level GPU code. |
| ADR-0004 / ADR-0083 | Replace paint-selected execution and public shape/paint representation as authoritative drawing contracts with ADR-0091. Retain specialized backend fast paths only as equivalent lowerings. |
| ADR-0008 / ADR-0017 / ADR-0080 | Replace caller-side effect execution, ambient capture, and the separate Rust composition planner with one Flux plan compiler. Preserve explicit input edges and conservative region propagation. |
| ADR-0019 / ADR-0071 | Replace the immediate Canvas backend seam and pass configuration entry points; retain CPU rendering and explicit antialiasing policy at compilation. |
| ADR-0025 / ADR-0068 | Replace live-tree Canvas replay and per-draw propagation of subtree opacity with snapshots and group composition. |
| ADR-0029 / ADR-0030 / ADR-0035 | Replace implicit previous-frame geometry, live-tree accessibility extraction, and draw-hash-only validity with presented snapshot generations and explicit invalidation. Retain stable widget identity, host event ownership, and accessibility semantics. |
| ADR-0021 / ADR-0022 | Replace caller-visible upload scheduling for managed work with plan nodes; retain asynchronous completion and bounded resource reuse. |
| ADR-0063 / ADR-0079 | Retain Prism material ownership and visual intent; replace private effect dispatch with declared effect nodes. |
| ADR-0069 / ADR-0070 / ADR-0072 | Retain color management and bounded resource governance; implement them through the new execution contract. |

### Clean-break completion

No deprecated forwarding API, legacy behavior switch, ABI alias, or second
planner is part of the delivered architecture. Migrate all in-tree bindings,
examples, tests, and callers with the corresponding replacement. External
callers must migrate; they do not determine the new API shape.

Implementation can use reviewable internal steps, but incomplete steps are
not a compatibility release. Completion requires deleting superseded Canvas
execution routes, borrowed command storage, duplicate shape/paint models,
and the independent `flux-composition-graph` planner after its required
capabilities exist in Flux. Helpers construct the new representation only.

## Alternatives Considered

- **Patch the current Canvas.** Leaves resource capture and scheduling
  implicit and duplicates the new execution authority.
- **Keep an external graph and an internal layer planner.** Splits lifetime,
  background sampling, and memory-budget reasoning across two schedulers.
- **Implement a universal GPU RHI.** Adds an unrelated backend commitment;
  semantic portability does not require such an abstraction.

## Consequences

Compilation and immutable snapshots add CPU work and retained memory.
Budgets, bounded caches, plan reuse, and diagnostics are required; zero-copy,
lock-free operation, and a fixed descriptor byte size are not guarantees.

Implementation starts from the current Canvas recorder/executors in
`libs/flux/src/canvas/`, resource code in `libs/flux/src/core/`, and consumers
in `libs/lens/`, `libs/prism/`, and `bindings/`. These are replacement sites,
not evidence that the new contracts already exist.

## Acceptance Criteria

- One end-to-end path renders paths, text, images, nested opacity groups,
  and dependent backdrop effects to offscreen and presentation targets.
- Optimization-disabled and optimized execution agree under the documented
  numeric tolerances; unsupported operations return an explicit error.
- Plan diagnostics expose dependencies, resource lifetimes, peak memory,
  cache invalidations, and reasons for applied or rejected optimizations.
- All six ADR contract suites pass, all in-tree consumers migrate, and
  superseded execution paths and compatibility entry points are removed.
- A tracked implementation matrix links each invariant to code and tests.
  Symbol counts, build success, and test totals do not establish semantic
  correctness or formal verification.
