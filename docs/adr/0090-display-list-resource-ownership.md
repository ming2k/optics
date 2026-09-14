# ADR-0090: DisplayList snapshots and resource ownership

- Status: Accepted
- Date: 2026-09-14
- Scope: Recording, resources, caches, and cross-thread publication.
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md).
- Implementation: Complete; verified against acceptance criteria in [the implementation matrix](../dev/architecture-review-2026-09-14.md).

## Context

An immutable command buffer can still contain dangling pointers or refer to
mutable images. The current `libs/flux/src/canvas/encoder.c` shallow-copies
descriptors and returns Arena storage. Neither cross-frame caching nor
cross-thread publication follows from that representation.

## Decision

A recorder is uniquely owned mutable state. Successful finish consumes its
recording state and publishes a DisplayList that owns all command payloads
and retains immutable versions of referenced resources. Inputs are copied
or retained through an explicit ownership transfer; untracked caller pointers
cannot survive recording. Paths, gradient stops, glyph positions, and font
identity follow the same rule as images.

An Arena is an allocation technique, not a lifetime contract. Freeze may
transfer backing chunks to the published owner. Resetting or destroying the
builder cannot invalidate a published list. Allocation failure publishes no
list and is reported through the recorder's terminal result.

Classify objects by actual ownership:

| Category | Contract |
|----------|----------|
| Value | Copying duplicates the complete value; no hidden resource release. |
| Unique owner | One owner controls mutation and cleanup; explicit move or transfer. |
| Shared immutable owner | Retention keeps content and identity stable; release drops a reference. |
| Borrowed view | Valid only within a documented owner lifetime; cannot release its owner. |

Reference counting is used where shared ownership requires it. Its atomicity
does not establish concurrent mutation safety. GPU retirement is orthogonal
and follows [ADR-0092](0092-resource-planning-and-retirement.md).

### Resource identity and cache validity

Immutable resource identity includes a generation. Updating logical content
publishes a new version; existing lists continue to reference the old one.
Device realization caches key by resource version and relevant device
properties. Content equality must not rely on raw addresses or unchecked
hash equality. Child-list references form an acyclic owned graph.

Eviction can remove recreatable device realizations after in-flight use
ends. It cannot discard the semantic content of a live snapshot. Pinned
content counts against budgets; when it prevents an allocation, the operation
fails explicitly instead of silently mutating old content.

### External and dynamic content

An imported image or buffer requires format, extent, color interpretation,
ownership, synchronization, and release obligations. A submission acquires
an exclusive or appropriately synchronized access lease for the declared
use. Foreign memory cannot be treated as an immutable snapshot merely by
retaining its handle.

A DisplayList may instead declare a typed dynamic input slot. Each execution
binds a resource version and lease explicitly. Such a list is a program with
inputs, not a self-contained pixel snapshot. Bindings participate in cache
keys, damage propagation, and lifetime validation. Missing bindings fail
before submission; hidden reads of the latest global resource are forbidden.

### Publication and serialization

Published lists permit concurrent read-only use. Mutable caches must provide
their own synchronization. Serialized programs are a separate, versioned
format using resource identifiers and validated payloads, never C pointers
or native struct bytes. This decision does not promise a serialization API;
that feature requires its own protocol specification and validation tests.

## Alternatives Considered

- **Require callers to keep an Arena alive.** Makes caching and resource
  retention depend on transitive, externally enforced lifetimes.
- **Retain only image handles.** Does not freeze mutable image content or
  retain path, gradient, glyph, and font inputs.
- **Copy all GPU images at finish.** Forces GPU access during CPU recording
  and creates unnecessary copies for already immutable resources.

## Consequences

Snapshot retention can increase memory use. Structural sharing and chunk
ownership transfer are permitted optimizations. Public structs with raw
payload pointers are replaced by owned handles and scoped views; the old
borrowed DisplayList ABI is removed without an adapter.

## Acceptance Criteria

- Destroy or mutate every recording input and reset the builder after
  finish; the published list still renders the captured content.
- Concurrent readers and resource-version updates do not alter old lists.
- Allocation failure at each capture point leaves no published partial list
  and no leaked reference; cyclic child references are rejected.
- Delayed submissions, dynamic rebinding, cache eviction, and external
  release callbacks preserve ownership and execute release exactly once.
