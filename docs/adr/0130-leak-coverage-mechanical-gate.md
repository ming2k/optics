---
id: ADR-0130
title: "Leak coverage is a mechanical gate: no resource kind ships without a drain assertion"
status: accepted
date: 2026-11-02
scope: [tools, tests, flux, glyph, lens, vista, prism, iris, bindings]
superseded_by: null
negative_knowledge: true
---

# ADR-0130: Leak coverage is a mechanical gate

- Status: Accepted
- Date: 2026-11-02
- Deciders: Optics Architecture Working Group
- Consulted: CI, `tools/optics`
- Informed: Core Engineering
- Depends on: [ADR-0129](0129-observation-plane-and-resource-ledger.md)
- Amends: [ADR-0098](0098-developer-tooling-and-architecture-guardrails.md)
  (adds a guardrail domain), [ADR-0126](0126-developer-tooling-domain-modularization-and-unified-optics-cli.md)
  (adds a CLI target)

---

## Context and Problem Statement

The project's own doctrine is mechanism over exhortation
(`docs/dev/standards/invariants.md`): if a rule has neither a compiler proof
nor an automated assertion, it is an aspiration. Leak coverage today is an
aspiration. The evidence:

- Seven test files reference `live_allocations`. All seven live under
  `tests/flux/`. `tests/lens/` has sixty test files and zero allocation
  assertions; `tests/iris/` has twelve and zero.
- Leak tests that do exist are written from memory and drift toward tolerance
  constants (`<= baseline + 2`).
- `tools/config/abi-supprs.abignore` already demonstrates the correct pattern
  for "a rule with reviewed exceptions": every suppression carries a written
  justification and is diffed in CI. There is no equivalent for leak
  coverage, so adding a new resource kind silently adds an uncovered kind.

The failure mode is not "nobody writes leak tests". It is "nobody can *tell*
whether a kind is covered", so coverage is re-derived by reading test files
during review, and the reader is the same person who is short of time.

## Decision Drivers

1. Coverage must be *enumerable* from the tree, not from a wiki page.
2. Adding a resource kind must be possible only by also declaring its drain
   point, exactly as adding a struct type requires appending to
   `flux_struct_type` (`static_assert` guards, append-only).
3. Exemptions must be written down with a reason, auditable, and rare.

## Architectural Decisions

### 1. `tools/optics check leaks` is a guardrail domain

New guardrail `leaks` in `tools/guardrails/leaks.py`, registered in
`tools/optics.py` alongside `boundary`/`lockstep`/`stdout`. It performs three
independent checks:

**(a) Inventory.** Enumerate every public `*_create`, `*_retain`, `*_acquire`
and every `*_release`, `*_destroy`, `*_free` across `libs/*/include/`. The
inventory is derived from the headers, so it cannot go stale. Each retained
kind must map to a declared leak lane in
`tools/config/leak-lanes.toml`:

```toml
[kind.flux_image]
scope    = "device"
lifetime = "context"
lane     = "tests/flux/integration/test_image_leak.c"
```

A kind with no lane is a hard failure. A lane path that does not exist is a
hard failure. A kind mapped to a lane whose binary is not registered in meson
is a hard failure, because a test that never runs is not coverage.

**(b) Exemption ledger.** `tools/config/leak-exempt.toml` follows
`abi-supprs.abignore` in shape and philosophy: every exemption states *why*
the kind cannot leak and *what* replaces the assertion. An exemption without a
`reason` field is a hard failure.

**(c) Symbol absence.** Given a configured release build directory, verify
that the observer capability gates actually gate: a build configured with
`FLUX_OBSERVE_COUNTS` only must not export `flux_observe_name*`,
`flux_observe_site*`, or pass-label symbols. This check is what makes "debug
only" mean something after the third release.

### 2. New invariants, registered in the catalog

Two invariants are added to `docs/dev/standards/invariants.md`, each with a
mechanical enforcement path:

| Invariant | Policy | Enforcement |
|---|---|---|
| `[INV-ARCH-LEDGER]` | Every public resource kind has a declared ledger scope and a leak lane (or a reasoned exemption). | `tools/optics check leaks` |
| `[INV-ARCH-SCOPEDRAIN]` | A scope's live count returns to zero at its declared drain point; `FLUX_LIFETIME_FRAME` kinds are enforced every frame in debug configurations. | `test_observe_scope` + frame-boundary assertion |

Both satisfy the three admissibility criteria in `invariants.md`: high blast
radius (unbounded growth and descriptor exhaustion), durable (it is the
ownership model itself), and mechanically enforced (a CI script and an
assertion, not a comment).

### 3. Coverage is per *kind*, not per *file*

The inventory unit is a resource kind (`flux_image`, `wl_buffer`,
`VkSurfaceKHR`), not a source file and not a library. Per-file coverage
metrics invite tests that touch files without touching lifetimes; per-library
metrics hide the one uncovered kind behind nine covered ones. Kind-level
granularity is the only unit at which "did we test this thing's release?" is
well-posed.

### 4. The gate is additive, not blocking-on-legacy

The inventory lands with every currently-covered kind mapped, and every
currently-uncovered kind either given a lane in the same change or entered in
`leak-exempt.toml` with a reason. Asterisking the whole tree into a blanket
exemption at introduction would defeat the purpose, so `leak-exempt.toml`
opens with at most the kinds that are structurally unownable (e.g.
`flux_icc_profile` is process-scoped immutable data with no device
affinity) — each with its reason written down.

## Alternatives Considered

### 1. Convention: "write a leak test when you add a resource"
- **Why considered**: zero tooling cost.
- **Rejected**: this is the current state, and the current state is the
  problem. Discipline that cannot be verified is not discipline.

### 2. LSan-only coverage
- **Why considered**: `-Db_sanitize=leak` already works and is documented
  (`docs/dev/testing.md:78`).
- **Rejected as sufficient**: LSan cannot see `VkDeviceMemory`, descriptors,
  or `wl_proxy` handles — none of them are libc allocations. It also does not
  catch "released too late" or "released into the wrong pool", both of which
  the ledger sees. LSan stays as a complementary lane, not the gate.

### 3. Coverage matrix in `docs/dev/architecture-implementation-status.md`
- **Why considered**: the project already tracks implementation status there.
- **Rejected as the gate**: a hand-maintained matrix is exactly as current as
  the last person who remembered to update it. The status document records
  *progress*; the guardrail establishes *fact*.

## Consequences

### Positive
- "Is `glyph_atlas_page` leak-covered?" becomes a machine answer.
- The exemption ledger makes debt explicit and diff-visible, so review sees a
  decision instead of an absence.
- Symbol-absence checking retires a recurring class of "debug path leaked into
  release" regressions.
- The guardrail reuses the existing `tools/optics` machinery (domain modules,
  `--fix` plumbing, exit-code conventions), so there is one tool, not two.

### Negative & Trade-offs
- Header scanning must be syntax-aware enough not to match comments; the
  `boundary.py` / `stdout_purity.py` precedent already solves this and the
  same stripping helpers are reused rather than reimplemented.
- The gate will fail on the first run after introduction, by design. Landing
  it requires writing the initial inventory and the initial exemption ledger
  in the same change — real work, not a tooling formality.
- A legitimate new resource kind now costs a lane. That friction is the
  point.

## Acceptance Criteria

1. `./tools/optics check leaks` fails when a public `*_create`/`*_retain`
   kind has no lane and no exemption, demonstrated by a negative test
   (`tests/optics/test_leaks_guardrail.py`).
2. The guardrail passes on the tree at the end of this change.
3. `[INV-ARCH-LEDGER]` and `[INV-ARCH-SCOPEDRAIN]` appear in
   `docs/dev/standards/invariants.md` with their enforcement mechanism.
4. `./tools/optics check` (all domains) is green.

## Links

- Related Standards: [Invariants](../dev/standards/invariants.md),
  [Workflow](../dev/standards/workflow.md),
  [Architecture](../dev/standards/architecture.md)
- ADR Index: [ADR Index](index.md)
- Predecessors: [ADR-0098](0098-developer-tooling-and-architecture-guardrails.md),
  [ADR-0126](0126-developer-tooling-domain-modularization-and-unified-optics-cli.md),
  [ADR-0129](0129-observation-plane-and-resource-ledger.md)
