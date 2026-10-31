---
id: ADR-0125
title: "DocGov Decoupling, Standards Consolidation, and Documentation Modernization"
status: accepted
date: 2026-10-30
superseded_by: null
scope: docs, dev/standards, agent/directives, tools
negative_knowledge: true
---

# ADR-0125: DocGov Decoupling, Standards Consolidation, and Documentation Modernization

- Status: Accepted
- Date: 2026-10-30
- Deciders: Optics Architecture Working Group
- Consulted: Platform Architecture, Engine Core, Lens UI, and Tools Teams
- Informed: Core Engineering, Developer Experience
- Amends: [ADR-0098](0098-developer-tooling-and-architecture-guardrails.md)

---

## Context and Problem Statement

Optics historically adopted an external documentation governance framework (`docgov`) to enforce repository invariants. Over time, this created compounding friction and architectural drag across the codebase:

1. **Invasive Upstream Tooling (`docgov`)**:
   An external binary (`docgov`) was introduced alongside a deep mirror directory (`docs/governance/documentation/{core,profiles}/`). This imported rigid linter restrictions, external YAML templates, and artificial delimiter comment tags (`<!-- BEGIN/END DOCGOV DIRECTIVES -->`) in `AGENTS.md`.
2. **The "Junk Drawer" Hierarchy**:
   The multi-layered `docs/governance/documentation/core/` and `profiles/` directory structure acted as an over-engineered junk drawer, adding navigation friction while offering zero value to Optics' C23 graphics runtime or public API consumers.
3. **Siloed Governance vs. Contributor Docs**:
   Technical specifications were split between `docs/governance/` (`components.md`, `materials.md`, `backends.md`, `effects.md`, `api-design-guidelines.md`) and `docs/dev/` (`setup.md`, `testing.md`, `release.md`), obscuring discovery and leaving `docs/dev/` without unified architectural ownership.
4. **Toothless AI Directives**:
   `AGENTS.md` contained purely auto-generated `docgov` protocol boilerplate. It contained zero knowledge of Optics' core C23 baseline, Vulkan 1.3 RHI purity, 7-library microkernel boundaries, or the Single Opts Descriptor paradigm.
5. **Missing Diátaxis Quadrant Gateways**:
   The four public quadrants (`tutorials/`, `how-to/`, `reference/`, `explanation/`) lacked dedicated `index.md` landing pages, causing the root `docs/index.md` to link unpredictably to arbitrary individual files.

---

## Decision Drivers

1. **Decouple from External Governance Overhead**: Eliminate `docgov`, `.docgov.yml`, `.docgov.lock`, and the nested `docs/governance/documentation/` mirror.
2. **Flatten & Consolidate Standards into `docs/dev/standards/`**: Establish a unified, flat, high-cohesion engineering standards suite directly inside `docs/dev/standards/`.
3. **Mechanisms Over Exhortations**: Invariants must be enforced by concrete mechanisms: compiler attributes (`[[nodiscard]]`, `static_assert`), library dependency firewalls, or deterministic verification scripts (`tools/check-all.sh`).
4. **High-Signal `AGENTS.md`**: Provide AI assistants with precise architectural red-lines, subsystem topologies, and on-demand standards routing.
5. **Complete Diátaxis Navigation**: Introduce dedicated `index.md` matrices for all public documentation quadrants with strict Audience Firewall enforcement.

---

## Architectural Decisions

### 1. Complete Decoupling from `docgov` and Unification into `docs/dev/standards/`
- Delete `.docgov.yml` and `.docgov.lock`.
- Completely abolish the external and over-nested `docs/governance/` directory.
- Re-home all engineering and architectural specifications directly into `docs/dev/standards/`:
  - `docs/dev/standards/README.md`: Central catalog and governance charter.
  - `docs/dev/standards/architecture.md`: 7-library topology, 5-Gate Review Matrix, and system red-lines.
  - `docs/dev/standards/invariants.md`: Admissibility matrix and core invariants catalog.
  - `docs/dev/standards/api-design.md`: Single Opts Descriptor paradigm and C23 standards.
  - `docs/dev/standards/adr.md`: ADR 3-question qualification test and template.
  - `docs/dev/standards/rfc.md`: RFC cross-boundary deliberation protocol.
  - `docs/dev/standards/taxonomy.md`: Diátaxis layout, audience firewall, and root whitelist.
  - `docs/dev/standards/style.md`: Hyperlink conventions and Markdown formatting.
  - `docs/dev/standards/workflow.md`: PR review gates and Code-to-Doc synchronization.
  - `docs/dev/standards/postmortems.md`: Blameless incident retrospective guidelines.

### 2. Streamlining & Re-architecting `AGENTS.md`
- Strip the artificial `<!-- BEGIN/END DOCGOV DIRECTIVES -->` delimiters.
- Re-architect `AGENTS.md` into four focused sections:
  1. Operational Guardrails (destructive git prohibition, finite command execution);
  2. Core Mental Model & Subsystem Boundaries (C23, 7 libraries, Vulkan RHI purity, material isolation, Single Opts, zero-alloc frame loop);
  3. Engineering Discipline (mechanisms over exhortations, invariant realism, atomic verification);
  4. On-Demand Standards Routing (direct pointers to `docs/dev/standards/*`).

### 3. Diátaxis Quadrant Index Gateways
- Author structured `index.md` navigation tables in `docs/tutorials/`, `docs/how-to/`, `docs/reference/`, and `docs/explanation/`.
- Update `docs/index.md` to route cleanly to each quadrant index.
- Enforce the **Audience Firewall (`[INV-DOC-FIREWALL]`)**: public documentation (`tutorials/`, `how-to/`, `reference/`, `explanation/`) must never link to internal developer docs (`docs/dev/**`).

### 4. Native Verification Gate
- Replace the manifest-hash verifier (`verify.sh`) with native documentation hyperlink and audience firewall verification (`tools/check-doc-links.py`), wired directly into `tools/check-all.sh`.

---

## Consequences

### Positive
- **Zero External Governance Drag**: Complete elimination of `docgov` tooling and fragile delimiter comments.
- **Flat, Readable Documentation**: No more nested "junk drawer" hierarchies under `docs/governance/`.
- **High-Leverage AI Agent Assistance**: Agents operating on the repository immediately understand Optics' C23 contracts and subsystem red-lines.
- **Audience Clarity**: Public consumers have clear, structured learning and task paths; contributors have a unified standards suite.
- **Self-Contained Epistemic Integrity**: Zero dead links, zero external protocol dependencies.

### Negative & Trade-offs
- Verification of documentation standards is now guided by repository-native tooling and engineering review rather than an external AST linter.

---

## Rejected Alternatives & Negative Knowledge

### 1. Retaining `docgov` While Pruning Rules
- **Why considered**: Attempting to keep the external binary while disabling rules in `.docgov.yml`.
- **Why rejected**: Perpetuates dependency on an external binary that mandates invasive boundary comments in `AGENTS.md` and generates redundant mirror directories. Native repo checks provide cleaner, faster verification without external drag.

### 2. Keeping `docs/governance/` as a Sibling to `docs/dev/`
- **Why considered**: Preserving the top-level directory name to avoid updating links.
- **Why rejected**: Creates a false dichotomy between "governance" and "development". Engineering standards belong directly in `docs/dev/standards/` where developers work and consult runbooks.

---

## Links

- Related Standards: [Architecture](../dev/standards/architecture.md), [Invariants](../dev/standards/invariants.md), [Taxonomy](../dev/standards/taxonomy.md)
- Related Agent Directives: [AGENTS.md](../../AGENTS.md)
- ADR Index: [ADR Index](index.md)
