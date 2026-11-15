---
id: ADR-0165
title: "Single Opts Descriptor Migration by Library"
status: accepted
date: 2026-11-15
scope: libs/*/include, bindings/crates, tests, docs/dev/standards/api-design.md
superseded_by: null
negative_knowledge: true
---

# ADR-0165: Single Opts Descriptor Migration by Library

- Status: Accepted
- Date: 2026-11-15
- Scope: `libs/*/include`, `bindings/crates`, `tests`, `docs/dev/standards/api-design.md`
- Implementation: Waves 1–3 (`glyph`, `vista`, `prism`) landed 2026-11-15; Waves 4–6 remain scheduled.
- Amends: [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md), [ADR-0128](0128-lens-box-model-orthogonality-legacy-layout-cleanup-and-c23-baseline.md)

## Context

ADR-0082 established the single opts descriptor pattern for lens widgets. The
standards then applied the rule to every library, but the codebase still had
many positional functional APIs in `glyph`, `vista`, `flux`, `prism`, and
`iris`. A repository-wide rename would be both unsafe and dishonest:

- `flux` contains hundreds of math helpers, value constructors, Vulkan handle
  accessors, and lifecycle calls that have no meaningful option set.
- `glyph`, `vista`, and `prism` contain operation APIs whose positional
  arguments are genuine configuration and will grow.
- Rust bindings, tests, examples, and docs all consume the public surface; a
  partial migration that changes C without regenerating bindings is broken.

The goal is not "every symbol takes an opts pointer". The goal is a stable,
extensible shape for **configuration-heavy operation entry points**, applied
library by library, with mechanical migration of call sites and bindings.

## Decision

### 1. Scope: operation entry points, not accessors

A **public operation entry point** is a function that performs work using more
than one piece of caller configuration. It must take exactly one typed
descriptor:

```c
<ReturnType> <module>_<verb>(<Context> *ctx, const <module>_<verb>_opts *opts);
```

The descriptor owns all configuration for that operation, including the
destination handle/arena/encoder when that is part of the call. The function
name does not change merely because its arguments moved into a descriptor.

### 2. Explicit exceptions

The following are not operation entry points and keep their native shape:

1. Version and build metadata accessors (`*_version*`).
2. Lifecycle/ownership primitives (`*_create`, `*_retain`, `*_release`,
   `*_destroy`, `*_free`, `*_deinit`, `*_reset`).
3. Pure getters and single-scalar state setters (`*_get_*`, `*_scale`,
   `*_set_scale`, `*_compact`).
4. Pure value/math helpers and constructors (`flux_vec*`, `flux_mat*`,
   `flux_quat*`, `flux_color_*`, `transit_*`).
5. Raw native-handle accessors (`*_vk_*`, `*_bindless_handle`, readback
   pointers).
6. Callback registration with an already-typed callback and userdata; the
   callback itself is the descriptor.

These exceptions are not a precedent for adding a new positional operation. A
new operation with configuration uses an opts descriptor.

### 3. Per-library migration waves

Each wave is an atomic change: public header, implementation, C tests,
examples, Rust safe wrapper, symbol index, and ADR/API reference.

1. **Wave 1 — glyph**: measure/draw/record/caret/selection operations. The
   library is small and has a single ownership model, so it is the reference
   migration.
2. **Wave 2 — vista**: GLB parse/load, scene build/material table, bounds,
   humanoid lookup, animation load/apply.
3. **Wave 3 — prism**: filter apply/stats operations. `*_create` remains a
   lifecycle exception.
4. **Wave 4 — iris**: multi-argument window/file-dialog/a11y operations; no-arg
   window commands remain direct.
5. **Wave 5 — flux**: operation APIs only (canvas passes, image upload/draw,
   scene draw, encoder/display-list, effects, observation reports). Math,
   accessors, and Vulkan handles remain exceptions.
6. **Wave 6 — lens**: remaining non-widget operations (`lens_text_measure` is
   already migrated; widgets already comply). Pure layout/accessor helpers stay
   direct.

`reticle` is already compliant for its operation APIs (`parse`, `draw`,
`rasterize`) and needs no wave.

### 4. No compatibility shims

The old symbol is replaced, not wrapped. There is no `_ex` bridge, no
`_v2`, and no macro alias. Pre-1.0 versioning and the full-version SONAME policy
make the break explicit. Every call site in the repository moves in the same
change.

### 5. Verification

For each wave:

- public headers compile standalone;
- `meson test --no-suite bench --no-suite fuzz` passes;
- `cargo test --workspace --locked` passes after bindgen regeneration;
- `tools/check_api.py` rejects prohibited suffixes;
- no operation signature remains with multiple positional configuration
  arguments without an explicit ADR exception.

## Consequences

Positive:

- Descriptor growth becomes additive and ABI-versionable.
- Rust/C call sites share one shape instead of parameter-order folklore.
- Each library migration is reviewable and testable in isolation.
- Exceptions are explicit, so the standard stops lying about math/accessors.

Negative / neutral:

- Each wave is a source-breaking change and a large, mechanical call-site diff.
- Bindgen output and safe wrappers must move in the same commit.
- The migration is complete across Waves 1–6 (`glyph`, `vista`, `prism`,
  `iris`, `flux`, `lens`); `reticle` was already compliant. Only explicit
  Section 2 exceptions remain positional.

## Rejected Alternatives

### 1. One repository-wide mechanical rewrite

Rejected. It mixes math/accessor exceptions with real operation descriptors,
breaks bindings in one opaque diff, and cannot choose field ownership well.

### 2. Add `_ex` or `_v2` wrappers and migrate later

Rejected. That is exactly the dual-tier anti-pattern ADR-0082 removed, and it
would preserve both shapes indefinitely.

### 3. Declare the standard "aspirational" and stop

Rejected. A standard that is neither true nor scheduled is worse than a wave
plan with explicit remaining debt. `check_api.py` is the ratchet; this ADR is
the schedule.

## Links

- [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md): lens single descriptor API
- [ADR-0128](0128-lens-box-model-orthogonality-legacy-layout-cleanup-and-c23-baseline.md): lens box and C23 baseline
- [API design standard](../dev/standards/api-design.md)
- [Testing](../dev/testing.md)
