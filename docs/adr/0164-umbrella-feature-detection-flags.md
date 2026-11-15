---
id: ADR-0164
title: "Umbrella Feature Detection Uses Distribution Flags, Not Host Header Probing"
status: accepted
date: 2026-11-15
scope: include/optics/optics.h, meson.build, optics.pc, tests/optics/
superseded_by: null
negative_knowledge: true
---

# ADR-0164: Umbrella Feature Detection Uses Distribution Flags, Not Host Header Probing

- Status: Accepted
- Date: 2026-11-15
- Scope: `include/optics/optics.h`, `meson.build`, `optics.pc`, `tests/optics/`
- Amends: [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md)

## Context

ADR-0160 chose C23 `__has_include` to expose optional subsystems from
`<optics/optics.h>` without requiring consumer preprocessor flags. The intent
was good: one include, no manual configuration.

The implementation has a structural failure mode that is not hypothetical:

1. **The host include path is not the distribution.** `__has_include` searches
   every include directory the compiler sees, including `/usr/include` and
   `/usr/local/include`. A stale system copy of `<prism/prism.h>` from an older
   Optics install satisfies the probe even when the local build deliberately
   omitted prism.
2. **Header presence is not version compatibility.** The compiled descriptor
   layouts are versioned by the full pre-1.0 version string. A stale header can
   compile and then disagree with the library the consumer actually links.
3. **A disabled feature becomes visible again.** The umbrella re-exports
   declarations for a module that `optics.pc` does not require and the build
   does not link. The first call becomes an undefined reference at link time,
   far from the configuration error.

This is exactly the class of failure ADR-0160's dual-track distribution is
supposed to prevent: granular packages must remain granular, and a consumer
must not inherit a module it did not ask for.

## Decision

The umbrella exposes optional modules through **explicit distribution flags**,
never host header probing.

1. **`optics_dep` and `optics.pc` carry `OPTICS_HAVE_<MODULE>`.** The root Meson
   build adds `-DOPTICS_HAVE_GLYPH=1`, `-DOPTICS_HAVE_LENS=1`,
   `-DOPTICS_HAVE_IRIS=1`, `-DOPTICS_HAVE_PRISM=1`, `-DOPTICS_HAVE_VISTA=1`,
   `-DOPTICS_HAVE_TRANSIT=1`, and `-DOPTICS_HAVE_RETICLE=1` only for modules
   that are part of the build. The flags are propagated to consumers through
   both the Meson dependency object and `pkg-config --cflags optics`.
2. **The header guards include the flag, not the filesystem.** Each optional
   include is wrapped in `#if defined(OPTICS_HAVE_<MODULE>)`. A stale header in
   a system directory is irrelevant.
3. **`flux` remains unconditional; every other layer is flagged.** `flux` is
   the foundation and has no feature probe. `glyph`, `lens`, `iris`, `prism`,
   `vista`, `transit`, and `reticle` each use the same `OPTICS_HAVE_<MODULE>`
   mechanism when they are part of the build.
4. **The flags are a distribution contract, not a user preference.** A consumer
   that includes `<optics/optics.h>` without the `optics.pc` flags gets only
   `flux` (the one unconditional foundation). That is deterministic; guessing
   from the host is not.

## Consequences

Positive:

- A stale system install cannot resurrect a module omitted by the current
  distribution.
- Header and link surface agree: every optional declaration is present only
  when `optics.pc` also requires and links that module.
- Feature detection is testable from the build graph; no environment-dependent
  `__has_include` result.

Negative / neutral:

- A consumer that bypasses `pkg-config` and manually includes
  `<optics/optics.h>` must pass the same `-DOPTICS_HAVE_*` flags to see optional
  layers. The header documents this.
- The flags are an internal distribution detail, not an API for third-party
  feature selection.

## Rejected Alternatives

### 1. Keep `__has_include` and ask users to clean stale prefixes

Rejected. A build system cannot assume a clean host, and the failure is a
versioned-ABI mismatch rather than a helpful missing-header error.

### 2. Always include every module and require every library

Rejected. It destroys the granular/headless distribution ADR-0160 explicitly
preserves and turns optional modules into mandatory link dependencies.

### 3. Generate `optics/config.h` per install

Rejected for now. It adds a second generated header and a second lookup path;
`optics.pc` already carries the exact flags and Meson already propagates them.
The compile-definition contract is smaller and works for both build-tree and
installed consumption.

## Links

- [ADR-0160](0160-unified-optics-c-facade-and-umbrella-distribution.md): unified C umbrella distribution
- [ADR-0163](0163-repository-root-cargo-workspace.md): repository-root Cargo workspace
- [API Reference](../reference/api.md)
