---
id: ADR-0129
title: "The observation plane: one allocation ledger for every library, scope tag, and the end of ad-hoc leak triage"
status: accepted
date: 2026-11-02
scope: [flux, glyph, lens, vista, prism, iris, bindings, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0129: The observation plane — one allocation ledger for every library

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: `flux` core, `lens` store, `glyph` atlas, `iris` platform layer
- Informed: Core Engineering
- Amends: [ADR-0072](0072-long-session-resource-governance.md) (extends
  the bounded-resource doctrine to accounting), [ADR-0085](0085-lens-borrowed-string-registry.md)
  (generalises its debug registry)
- Does **not** amend: [ADR-0092](0092-resource-planning-and-retirement.md)
  (the RenderPlan retirement authority is untouched)

---

## Context and Problem Statement

The stack already spends real engineering on *prevention*: a bounded retire
FIFO with deliberate backpressure (`FLUX_RETIRE_MAX_PENDING`, ADR-0072), a
slab GPU allocator (ADR-0007), an O(1) glyph LRU, a live-list store GC, and a
three-layer memory accounting triple (`bytes_in_use` / `bytes_reserved` /
`lost_ranges_bytes`) that already distinguishes leak from high-water from
fragmentation.

What the stack does *not* have is an answer to the second question. When a
leak occurs, the sum total of library output is one line:

```
flux_vk_allocator: %u live allocations at teardown (possible resource leak)
```

No type, no name, no creation site, no scope. Worse, the coverage is lopsided:

| Allocation domain | Funnel | Leak assertion |
|---|---|---|
| flux core host objects | `flux_internal_alloc` (single chokepoint) | 7 test files |
| flux GPU memory | `flux_vk_allocate` (single chokepoint) | same |
| lens / glyph / vista / prism host objects | **none** | **none** |
| file descriptors, `wl_proxy`, `wl_buffer`, `VkSurfaceKHR` | **none** | **none** |
| Rust binding ownership | `Drop` impls, hand-audited | **none** |

Three concrete consequences follow, all observed rather than hypothesised:

1. A leak in `lens` cannot be attributed to a component even in principle,
   because no lens allocation crosses a counted boundary.
2. The most severe leak class in this stack — descriptors and Wayland
   protocol objects, which are unbounded and process-global — is invisible to
   every counter the library exposes. `libs/iris/src/app_wayland.c` carries
   comments that admit this (`:1643`, `:2182`: "…or it leaks").
3. Existing leak tests compensate with tolerance constants
   (`test_text_atlas_leak.c:174` asserts `<= baseline + 2`). Every tolerance
   is a future leak's escape hatch, and the regression that comment documents
   was exactly a leak page-sized enough to be swallowed by a tolerance.

The preventive architecture is not the problem. **The absence of a ledger
is.** Prevention and attribution are orthogonal axes, and the project has
been paying full price on one axis and nothing on the other.

## Decision Drivers

1. Attribution must not require re-architecting the resource model. Opaque
   pointers plus `retain`/`release` already give a *precise* leak signal for
   reference-counted objects (`ref_count != 1` is assertable; a generational
   handle index is not).
2. The diagnostic path must remain usable when everything else has failed:
   under OOM, after device loss, with no frame submitted. A diagnostic
   facility that can fail is not a diagnostic facility.
3. Layer 0 must not acquire a link dependency on higher layers, and must not
   acquire a libunwind/DbgHelp dependency (ADR-0063 boundary discipline,
   extended by analogy).
4. Non-functional goals must not drive ownership-model rewrites. A leak
   program that forces an ABI break buys risk, not coverage.

## Architectural Decisions

### 1. One ledger, four scopes, zero allocation

A new header `<flux/observe.h>` declares a single counting ledger. It
includes no Vulkan header, allocates nothing, and takes no lock on the hot
path (per-scope atomic counters; mutex only around the bounded name table).

```c
typedef enum flux_scope : uint32_t {
    FLUX_SCOPE_DEVICE  = 0, /* flux GPU + host objects: per-device teardown */
    FLUX_SCOPE_CONTEXT = 1, /* lens store, glyph atlas, vista scene, prism */
    FLUX_SCOPE_PROCESS = 2, /* descriptors, wl_proxy, wl_buffer, VkSurfaceKHR */
    FLUX_SCOPE_COUNT   = 3,
} flux_scope;

typedef enum flux_scope_lifetime : uint32_t {
    FLUX_LIFETIME_FRAME   = 0, /* must return to zero every frame */
    FLUX_LIFETIME_CONTEXT = 1, /* must return to zero when the context dies */
    FLUX_LIFETIME_PROCESS = 2, /* must return to zero when the process exits */
} flux_scope_lifetime;
```

A scope declares its lifetime contract and its expected drain point. The
ledger stores, per `(scope, kind, owner)`:

```c
typedef struct flux_observe_counters {
    uint64_t live;         /* acquire - release */
    uint64_t peak;         /* high-water mark */
    uint64_t acquire_total;
    uint64_t release_total;
    uint64_t bytes_live;   /* for kinds that carry a size; 0 otherwise */
    uint64_t bytes_peak;
} flux_observe_counters;
```

`live` is the only field a test asserts on. `peak` and `bytes_peak` exist so
that high-water regressions (ADR-0072 class) and true leaks are distinguishable
without conflating them, and so that `bytes_reserved` behaviour is never
mistaken for a leak.

The ledger is compiled into every build. Its cost is a relaxed atomic
increment/decrement per counted event; nothing is stringified, hashed, or
logged unless the observer capability is on. Measured cost budget is stated
in the acceptance criteria below and is a hard gate, not an aspiration.

### 2. Attribution is a capability, not a build type

Object naming, pass labelling and creation-site capture are gated by one
descriptor, not by five independent `#ifdef`s and not by the build type:

```c
typedef struct flux_observe_opts {
    flux_struct_type type; /* FLUX_TYPE_OBSERVE_OPTS */
    const void *next;
    uint32_t capabilities;  /* OR of flux_observe_capability bits */
    uint32_t name_capacity; /* bounded name/site table entries; 0 = default */
    flux_log_fn log;        /* NULL = silent teardown report */
    void *log_user;
} flux_observe_opts;

typedef enum flux_observe_capability : uint32_t {
    FLUX_OBSERVE_COUNTS      = 1u << 0, /* always on; the rest are opt-in */
    FLUX_OBSERVE_NAMES       = 1u << 1, /* derive VkDebugUtils names */
    FLUX_OBSERVE_PASS_LABELS = 1u << 2, /* vkCmdBeginDebugUtilsLabelEXT */
    FLUX_OBSERVE_SITES       = 1u << 3, /* first-acquire creation site */
} flux_observe_capability;
```

A release build configured with `FLUX_OBSERVE_COUNTS` only must not carry the
string tables, the label entry points, or the site ring. This is mechanically
verified (see acceptance criteria), because "it's debug-only" has a
half-life of one release cycle.

### 3. Teardown produces evidence, not a warning

The single teardown `WARN` is replaced by a structured report emitted from
`flux_observe_report_close`, which runs **before** `vkDestroyDevice` (names
are unrecoverable afterwards — `vkSetDebugUtilsObjectNameEXT` needs a live
device) and after the retire queue drain has established that remaining
objects are genuinely leaked rather than in flight:

```c
typedef struct flux_observe_entry {
    flux_scope scope;
    const char *kind;        /* "image", "wl_buffer", "VkSurfaceKHR", … */
    const char *owner;       /* "flux/core", "lens/store", "iris/wayland" */
    const char *name;        /* NULL unless FLUX_OBSERVE_NAMES */
    const char *site;        /* "file.c:123" unless FLUX_OBSERVE_SITES */
    uint64_t live;           /* >0 means leaked at this scope's drain point */
    uint64_t bytes_live;
} flux_observe_entry;
```

The report is delivered through the `flux_log_fn` (stderr in the default
logger), never `stdout` (INV-ARCH-STDOUT). One entry per `(scope, kind,
owner)` that failed to drain. Rule of the report: **no entry may say
"possible"**. A scope that has reached its drain point has either drained or
leaked; ambiguity at that moment is a design defect in the scope, not a
property to be hedged in the message.

### 4. The frame scope is stricter than the context scope

Scopes declare their drain point as data, and the runtime enforces it:

- `FLUX_LIFETIME_FRAME`: asserted at `flux_frame_present` / `abandon` in
  debug configurations. Any non-zero live count after that boundary is a bug,
  not a statistic.
- `FLUX_LIFETIME_CONTEXT`: asserted at context teardown.
- `FLUX_LIFETIME_PROCESS`: asserted only by `tools/optics check leaks`.

This ordering matters because the frame scope is the only one that can be
enforced continuously without stalls, and it catches the largest class of
regression (a per-frame acquire that lacks its release) within one frame
instead of at process exit.

### 5. Existing ad-hoc diagnostics are retired, not duplicated

`libs/lens/src/core/borrow.c` (ADR-0085) keeps its *contract* (caller-borrowed
strings must live through the frame) but moves its bookkeeping onto the
ledger as a `FLUX_SCOPE_CONTEXT` / `FLUX_LIFETIME_FRAME` kind. Two parallel
debug registries answering "did this frame balance?" is exactly the
dual-authority pattern ADR-0092 forbids for retirement; the same reasoning
applies to accounting.

## Alternatives Considered

### 1. Generational handles / slotmap for flux resources
- **Why considered**: claimed architectural protection against dangling use.
- **Rejected**: it treats use-after-free, not leaks, and it *degrades* leak
  detection. A reference-counted pointer with a wrong count is an assertable
  defect; a generational index carries no count, so "the handle was never
  destroyed" becomes unobservable. It also breaks the public ABI, all seven
  binding crates, and the abigail baseline, in exchange for zero improvement
  on the stated goal. Generational tagging is retained where it already
  exists (lens slot/generation, canvas cache entries) and is the correct
  remedy for *bindless slot* double-release — see ADR-0131.

### 2. A second deferred-destruction queue for context objects
- **Why considered**: symmetry with the GPU retire FIFO.
- **Rejected**: two queues means two completion predicates for one question,
  and the question ("may this memory be reused?") already has one authority.
  The GPU retire queue is bounded and back-pressured per ADR-0072; adding a
  host-side peer creates the second answer ADR-0092 prohibits.

### 3. A new frame graph to "prevent leaks"
- **Why considered**: render graphs are often presented as leak prevention.
- **Rejected**: a frame graph's contribution is transient GPU aliasing. It
  has no bearing on host object lifetime, descriptor lifetime, or protocol
  object lifetime — the three domains where this project actually leaks. The
  RenderPlan (ADR-0089/0092) is the single authority; a second graph would be
  a competing authority. Prevention comes from bounded structures, which the
  project already has.

### 4. Call-stack capture on every acquire, in the library
- **Why considered**: "where was this allocated" is the most useful single
  fact during triage.
- **Rejected in this form**: symbolisation pulls libunwind/DbgHelp into
  Layer 0 (boundary violation), `backtrace()` allocates (re-entrancy on the
  allocator path), and the registry itself becomes an OOM vector precisely
  when memory is short. Reduced to `FLUX_OBSERVE_SITES`: an opt-in,
  fixed-capacity, allocation-free ring of `(file, line)` strings that are
  copy-on-register from literals only. Full stacks remain an offline tool
  concern, not a library concern.

### 5. A zombie/orphan live-object table with periodic scanning
- **Why considered**: a swept table can detect "this object was retained a
  long time ago" without waiting for teardown.
- **Rejected**: it taxes the fast path permanently to serve only the
  debug configuration; its correctness requires per-object "expected
  long-lived" annotations, which is a pseudo-invariant by ADR-0024/`invariants.md`
  definition; and false positives from legitimately persistent objects
  (glyph atlas pages, pipeline caches, pinned snapshots) produce report
  fatigue, which is the actual cause of death for detectors of this class.
  The ledger's `peak` counter plus the existing `bytes_in_use` /
  `bytes_reserved` split answers the same question with data already
  collected.

## Consequences

### Positive
- Every leak in every library gets a `(scope, kind, owner, name, site)`
  tuple, not a byte total.
- fd and protocol-object leaks — previously invisible by construction — become
  first-class counted kinds.
- Existing tolerance constants in leak tests can be tightened toward exact
  equality, because the ledger turns "roughly flat" into "exactly zero".
- Prevention and attribution become independently testable, so a boundary
  regression (an object escaping its scope's drain point) fails a test rather
  than waiting for a human to notice RSS growth.

### Negative & Trade-offs
- Every counted acquire/release pays a relaxed atomic. Set against the
  existing device-mutex and queue-lock traffic, this is noise, but it is
  measured rather than assumed, and the measurement is a gate.
- Scope and kind strings must be stable literals. Anything computed (a
  material's user-facing name, a caller's asset path) may become a *name*,
  never a *kind*; `kind` is a closed vocabulary owned by the library.
- The report runs on the teardown path, which is already delicate. It must be
  allocation-free and must not take the allocator lock (it reads counters that
  are atomics).
- Adding a new resource kind without a leak assertion becomes a CI failure
  (ADR-0130), which is friction by design.

## Acceptance Criteria

`docs/dev/architecture-implementation-status.md` records progress; the
following are the gate conditions, not aspirations:

1. `flux_observe_report_close` produces one structured entry per un-drained
   `(scope, kind, owner)` before `vkDestroyDevice`.
2. Release build with `FLUX_OBSERVE_COUNTS` only exports no
   `flux_observe_name*` / `flux_observe_site*` / pass-label symbols,
   verified by `tools/optics check leaks`.
3. `tests/flux/integration/test_text_atlas_leak.c` asserts exact equality
   (`== baseline`) rather than `<= baseline + 2`.
4. A lens-scope leak test exists and fails when the store is deliberately
   leaked.
5. `tests/observe/test_observe_scope.c` proves frame-scope enforcement fires
   within one frame.

## Links

- Related Standards: [Architecture](../dev/standards/architecture.md),
  [Invariants](../dev/standards/invariants.md),
  [API Design](../dev/standards/api-design.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0007](0007-vk-slab-allocator.md),
  [ADR-0072](0072-long-session-resource-governance.md),
  [ADR-0085](0085-lens-borrowed-string-registry.md),
  [ADR-0092](0092-resource-planning-and-retirement.md)
