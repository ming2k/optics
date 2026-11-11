---
id: ADR-0157
title: "DisplayList serialization is a versioned byte protocol with caller-owned resource IDs"
status: accepted
date: 2026-11-11
scope: [flux, canvas, bindings, tests]
superseded_by: null
negative_knowledge: true
---

# ADR-0157: DisplayList serialization is a versioned byte protocol with caller-owned resource IDs

- Status: Accepted
- Date: 2026-11-11
- Deciders: Optics Architecture Working Group
- Scope: `libs/flux/include/flux/canvas.h`, `libs/flux/src/canvas/encoder.c`, `bindings/flux-rs`, display-list tests.
- Extends: [ADR-0090](0090-display-list-resource-ownership.md) (the in-process ownership contract stays authoritative; this record adds a separate wire protocol).
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md), [ADR-0092](0092-resource-planning-and-retirement.md).

---

## Context and Problem Statement

ADR-0090 deliberately kept serialization outside its scope: a `flux_display_list`
owns native command payloads, image/sampler references, and child lists. Dumping
that representation to bytes would serialize C pointers, native struct layout,
ABI padding, and device handles. It would also make the on-disk format depend on
the compiler, the host endianness, and the current `flux_struct_type` numbering.

The product still needs a way to persist a recorded list and load it later, so
the protocol must be specified before code exists. The protocol must:

- never contain pointers, native struct bytes, or device handles;
- identify external images and samplers by caller-owned integer IDs;
- preserve state isolation for child lists;
- validate every length, count, enum, resource reference, and alignment before
  allocating or replaying;
- publish nothing and retain nothing on failure;
- carry an explicit format version so a future change can reject old bytes.

## Decision

### 1. A canonical little-endian stream

All multi-byte fields are explicitly little-endian. The header is 24 bytes:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | magic `0x4C445846` (`FLXD`) |
| 4 | 2 | version, currently `1` |
| 6 | 2 | flags, zero in v1 |
| 8 | 4 | resource-table entry count |
| 12 | 4 | command-section byte length |
| 16 | 4 | total stream length |
| 20 | 4 | reserved, zero in v1 |

Each resource entry is 8 bytes: `kind:u8`, three zero bytes, `id:u32`.
Kinds are `IMAGE = 1` and `SAMPLER = 2`. ID zero means "absent" and is never a
table entry. IDs are assigned by the caller's `resource_id` callback; one ID
cannot name two resources and one resource cannot change ID within a stream.

Each command is:

| Field | Size |
|---|---:|
| opcode | 1 |
| flags | 1 |
| reserved | 2 (zero) |
| payload length | 4 |
| payload | payload length |
| padding | zero to the next 8-byte stream boundary |

The opcode set is the existing command vocabulary: draw geometry, draw glyph
run, save, restore, clip, save layer, translate, scale, rotate, transform.
Unknown opcodes are invalid, not ignored.

### 2. Resources are identified, not embedded

An image brush carries an image ID and a sampler ID (zero for null). A glyph
run carries an atlas image ID and a sampler ID. The loader resolves every table
entry through the caller's `resolve` callback, then records the referenced
resources through the ordinary encoder path: images become immutable snapshots
(ADR-0090) and samplers are retained.

A borrowed dma-buf image therefore cannot be serialized into a retained list.
Callers that need repeatable external content must either use an exclusive
import or bind the image through a dynamic DisplayList input slot at execution.

### 3. Child lists are flattened with explicit state isolation

A child command is emitted as `SAVE`, the child's commands recursively, then
`RESTORE`. Sharing is not preserved on the wire; the loaded list is a single
flat list with the same visual and state-isolation semantics. Recursion depth
is bounded by the existing 256-level tree limit and is revalidated on load.
This avoids a second resource kind for child-list handles and prevents a
serialized graph from inventing cycles.

### 4. Failure is transactional

Serialization builds commands and the resource table in temporary storage and
only allocates the final stream after both succeed. Deserialization parses and
validates the header and table before creating an encoder; any malformed
length, count, enum, duplicate ID, unresolved resource, overflow, or invalid
alignment returns an error, publishes `*out_list = nullptr`, and destroys the
partial encoder. No retained image/sampler reference escapes a failed load.

### 5. API contract

```c
flux_result flux_display_list_serialize(
    const flux_display_list *list,
    const flux_display_list_serialize_opts *opts,
    uint8_t **out_data, size_t *out_size);

flux_result flux_display_list_deserialize(
    const uint8_t *data, size_t size,
    const flux_display_list_deserialize_opts *opts,
    flux_display_list **out_list);

void flux_display_list_serialized_free(uint8_t *data);
```

`resource_id` and `resolve` are caller callbacks so resource identity can be
persisted in whatever asset database or IPC schema the host already uses.
A list with no external resources may pass `nullptr` options.

## Validation

`tests/flux/fuzz/fuzz_display_list_deserialize.c` feeds arbitrary bytes
through the public loader with a resolver that refuses every resource, so the
header, table, opcode, length and alignment checks run without a Vulkan device.
`tests/flux/fuzz/corpus/display_list_deserialize/` seeds valid empty/solid/
dynamic-slot streams plus truncated headers, bad magic/version/flags/reserved,
zero/duplicate resource IDs, and oversized payloads. The ci-fuzz matrix runs
the committed corpus and a bounded campaign under ASan/UBSan.

## Invariants

- **INV-1 — No native representation on the wire.** No pointer, `sizeof`-based
  payload, enum value without a documented mapping, or device handle appears.
- **INV-2 — IDs are caller-owned and bidirectional.** The same ID resolves to
  one resource; the same resource keeps one ID within a stream.
- **INV-3 — Validate before allocate/replay.** Header, table, command lengths,
  counts, enums, slot bounds, and total size are checked first.
- **INV-4 — Failed load publishes nothing.** `*out_list` stays null and every
  partial resource reference is released.
- **INV-5 — Flattening preserves state isolation.** Child state cannot leak
  into following commands after load.
- **INV-6 — Version is explicit.** A future v2 must not silently reinterpret v1.

## Consequences

### Positive

- Lists can cross process, cache, or asset-store boundaries without depending
  on compiler layout or address identity.
- The resource table makes external ownership explicit and auditable.
- The loader reuses the encoder's validation and snapshot rules, so a loaded
  list obeys the same ownership contract as a recorded one.
- Malformed input is rejected before it can reach a command buffer.

### Negative / Trade-offs

- Child-list sharing is duplicated on the wire; a heavily shared tree grows.
- Resource identity is the caller's responsibility; a wrong ID binding is a
  caller error, not something the stream can infer.
- Pixel payloads are not embedded. An image ID still refers to an external
  resource at load time.
- The format must be versioned and reviewed like any other wire protocol.

## Rejected Alternatives & Negative Knowledge

### Dump the native command buffer

- **Why considered:** zero implementation work.
- **Why rejected:** leaks pointers and ABI padding, is not portable, and makes
  every struct-layout change a silent wire-format change.

### Embed image pixels in the stream

- **Why considered:** self-contained assets.
- **Why rejected:** destroys DisplayList's value-semantic separation from GPU
  resources, duplicates device realizations, and turns serialization into an
  image-container format with its own compression and color requirements.

### Use child-list handles as a third resource kind

- **Why considered:** preserves sharing.
- **Why rejected:** the wire graph would need cycle detection, occurrence
  ordering, and a second retention model. Flattening preserves semantics while
  keeping validation and ownership local.

### Ignore unknown opcodes for forward compatibility

- **Why considered:** easier version evolution.
- **Why rejected:** an unknown opcode changes pixels silently. A new format
  must bump the version and be rejected by older loaders.
