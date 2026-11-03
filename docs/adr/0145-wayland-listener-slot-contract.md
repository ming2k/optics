---
id: ADR-0145
title: "Wayland listener slots are a pinned, mechanically verified contract"
status: accepted
date: 2026-11-04
scope: [iris, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0145: Wayland listener slots are a pinned, mechanically verified contract

- Status: Accepted
- Date: 2026-11-04
- Deciders: Optics Architecture Working Group
- Consulted: `libs/iris` (Wayland backend, tablet input, presentation pacing), Developer Experience
- Informed: Core Engineering
- Amends: [ADR-0126](0126-developer-tooling-domain-modularization-and-unified-optics-cli.md), [ADR-0129](0129-observation-plane-and-resource-ledger.md)

---

## Context and Problem Statement

`libs/iris` is the default Linux host, and every Wayland protocol object it
binds is driven through a `wl_proxy` listener table: 24 interfaces across
`app_wayland.c` and `tablet_wayland.c`. libwayland treats a NULL entry in that
table as unrecoverable. `wl_closure_invoke` calls
`wl_abort("listener function for opcode %u of %s is NULL")`, which `abort()`s
the process from inside `wl_display_dispatch`. There is no error return, no
handle a consumer could catch, and the message names the protocol object rather
than the translation unit that omitted the field:

```text
listener function for opcode 0 of wp_presentation_feedback is NULL
Aborted (core dumped)
```

Optics 0.0.72 shipped exactly that. `feedback_listener` implemented `presented`
and `discarded` but not `sync_output`; compositors send `sync_output` before
every `presented`, so the window died on its first presented frame for every
user, on the first session, with no stack trace that named the mistake.

No part of the toolchain can state this rule. Both listener tables are
designated-initializer aggregates, and GCC and Clang deliberately suppress the
aggregate-arity diagnostic for designated init — `-Wmissing-field-initializers`
(gcc 16.1) and `-Wmissing-designated-field-initializers` (clang 22.1) are silent
on `{.enter = f}` against an eleven-field `wl_pointer_listener`. `sizeof`
cannot express "every event an interface declares in someone else's header", so
`static_assert` is not available either. The invariant is unenforceable by the
compiler, which leaves two options: a mechanical check, or nothing.

A mechanical check over the installed protocol corpus, however, is the wrong
mechanism. It makes the verdict a function of the host: it fails where
`wayland-protocols` is absent (macOS, Windows, minimal container images — the
tree is unchanged and correct), and it goes red on unrelated pull requests when
a dependency bump adds an event upstream, because the corpus lists all events
rather than those below the version this tree binds. A gate that fails for
reasons the author did not cause is a gate developers learn to route around.

---

## Decision Drivers

1. The failure mode is a process abort in the default host backend, with no
   recovery path and no diagnostic that names the defect.
2. The invariant must hold for events that are currently undeliverable, because
   a raised `wl_registry_bind` version makes them deliverable without touching
   the listener table.
3. A verification verdict must be a pure function of the repository.
4. An upstream protocol addition is a real decision — a slot must be written —
   and belongs in review, not in a red build.

---

## Architectural Decisions

### 1. The expected event set is a pinned repository artifact

`tools/config/wayland-listeners.toml` lists, per interface, every event the
interface declares. It is checked in, reviewed like code, and its `corpus`
key records which `wayland-protocols` version produced it. The guardrail reads
only this file; the installed protocol corpus is consulted exclusively by
`./tools/optics fix listeners`, which regenerates the contract and surfaces the
change as a diff.

This is the `symbols.md` pattern from
[ADR-0098](0098-developer-tooling-and-architecture-guardrails.md) applied to a
protocol surface rather than a C header: a generated, tracked artifact whose
freshness is enforced, so drift is a reviewable one-line change instead of a
host-dependent verdict.

### 2. `./tools/optics check listeners` is hermetic

The check requires no Wayland installation, no network, and no build directory.
It strips C comments (so a documented-but-absent handler still reads as absent),
extracts every `const struct <iface>_listener <var> = {...}` in the tree, and
compares slots against the contract in both directions:

- A contract event with no slot is a violation — the abort.
- A slot the contract does not list is a violation — the contract is stale.

A table for an interface with no contract entry is a violation, so adopting a
new protocol cannot escape verification by not being pinned yet. There is no
grandfathering and no suppression list.

### 3. Unprovable shapes fail closed

The scanner reports an error rather than a pass for anything it cannot prove:
a listener table that is not `const` (a writable table can have a slot nulled at
runtime), a table declared without an initializer (slots assigned at runtime are
invisible to a static scan), an array of tables (nested aggregate bodies defeat
the flat scanner), and a tree with a Wayland backend but zero tables found —
the shape a broken parser would otherwise present as compliance forever.

### 4. Sunset is bound to the backend, not to the calendar

`libs/iris` is the sole consumer. The guardrail, its contract, and its test are
deleted in the same commit that removes the Wayland backend. The contract exists
to protect a backend; it must not outlive it.

---

## Consequences

### Positive

- The 0.0.72 abort class cannot reach a release: a missing or extra slot fails
  `./tools/optics check` in under 40 ms with the file, line, and variable.
- The verdict is identical on every developer machine and every CI runner,
  including hosts with no Wayland toolchain at all.
- An upstream protocol addition arrives as a reviewed diff with a written
  `corpus` provenance line, not as an unexplained red build.
- Raising a `wl_registry_bind` version can no longer convert a latent NULL slot
  into a user-visible abort.

### Negative & Trade-offs

- The contract must be regenerated after a `wayland-protocols` upgrade; a stale
  contract still catches missing slots, but cannot name events added upstream
  since it was pinned.
- Roughly 230 lines of tooling plus a test for one backend's protocol surface.
  Justified only because no compiler diagnostic covers the invariant; if one
  ever does, this ADR is superseded and the tooling deleted.
- The scanner is syntax-aware, not a C parser. Shapes it cannot see are refused
  by design (Decision 3) rather than approximated.

---

## Rejected Alternatives & Negative Knowledge

### 1. Read the event list from the installed protocol XML at check time

- **Why considered**: No pinned file to maintain; always current with the
  developer's toolchain.
- **Why rejected**: The verdict stops being a function of the repository. It
  fails where `wayland-protocols` is absent — a correct tree marked broken on
  macOS, Windows, and minimal images — and turns every dependency bump into red
  CI on unrelated pull requests, because the corpus lists all events regardless
  of the version bound. Reproduced before rejection: with no XML present the
  check emitted a "NULL slot … libwayland calls abort()" headline for a table
  that was complete.

### 2. Rely on a compiler warning

- **Why considered**: Zero tooling, zero maintenance.
- **Why rejected**: Designated initializers suppress the aggregate-arity
  diagnostic in both toolchains, and `sizeof` cannot express the rule. Verified
  directly: with the table made non-static and externally visible, gcc 16.1
  `-Wextra` / `-Wmissing-field-initializers` and clang 22.1 `-Wextra` /
  `-Wmissing-designated-field-initializers` all stayed silent.

### 3. Verify coverage only up to the bound protocol version

- **Why considered**: Fewer forced stubs for events this tree cannot receive.
- **Why rejected**: Correct today, wrong the instant a bind version rises — the
  exact latent-`abort()` trap this ADR exists to close. An undeliverable event
  costs three lines to stub, so there is no case where a NULL slot is the better
  trade.

### 4. Exempt an interface with a reviewed reason

- **Why considered**: Symmetry with `leak-exempt.toml` ([ADR-0130](0130-leak-coverage-mechanical-gate.md)).
- **Why rejected**: An exemption here permits a NULL slot, i.e. an abort. There
  is no legitimate reason to leave one, so there is no escape hatch to reason
  about — only a stub to write.

---

## Links

- Related Standards: [Architecture](../dev/standards/architecture.md), [Invariants](../dev/standards/invariants.md)
- ADR Index: [ADR Index](index.md)
- Implementation: `tools/guardrails/listeners.py`, `tools/config/wayland-listeners.toml`
- Negative test: `tests/optics/test_listeners_guardrail.py`
