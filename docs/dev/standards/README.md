# Optics Engineering Standards

This directory defines the authoritative engineering, architectural, and quality standards for the Optics monorepo (`flux`, `glyph`, `vista`, `prism`, `transit`, `lens`, `iris`).

Optics maintains a **flat, high-cohesion, practical, and code-aligned standards suite directly inside `docs/dev/standards/`**, decoupled from external governance meta-frameworks and nested junk-drawer directories.

---

## Standards Directory Structure

All engineering standards reside flatly in `docs/dev/standards/`:

| Document | Purpose & Authority |
| :--- | :--- |
| **[`architecture.md`](architecture.md)** | **Monorepo Topology & Boundaries**: Defines the 7-library microkernel architecture, 5-gate review matrix, layer purity, and core system red-lines. |
| **[`invariants.md`](invariants.md)** | **Invariant Standards & Admissibility**: Codifies true system invariants (mechanisms over exhortations), establishes the constraint classification matrix, and degazettes pseudo-tags. |
| **[`api-design.md`](api-design.md)** | **API Conventions & C23 Contracts**: Enforces the Single Opts Descriptor paradigm, modern C23 language baseline (`nullptr`, `{}`), and zero-alloc frame loop contracts. |
| **[`adr.md`](adr.md)** | **Architectural Decision Records**: The 3-Question Test for ADR qualification, in-place lifecycle (`draft`, `accepted`, `superseded`, `rejected`), frontmatter schema, and template. |
| **[`rfc.md`](rfc.md)** | **Request for Comments (RFC)**: Deliberation and consensus protocol for cross-cutting, high-reversal-cost subsystem redesigns prior to ADR ratification. |
| **[`taxonomy.md`](taxonomy.md)** | **Documentation Taxonomy**: Four-quadrant Diátaxis layout, audience boundaries (public user vs contributor internal), and knowledge lifecycle. |
| **[`style.md`](style.md)** | **Documentation & Markdown Style**: Canonical hyperlink conventions, relative routing, formatting consistency, and prose guidelines. |
| **[`workflow.md`](workflow.md)** | **Engineering Workflow & PR Gates**: Review gates, `tools/optics check`, code-to-doc synchronization, and pre-release hygiene. |
| **[`postmortems.md`](postmortems.md)** | **Blameless Incident Postmortems**: Root-cause analysis, timeline reconstruction, detection gap mitigation, and defense-in-depth principles. |

For hands-on testing execution and QA runbooks, see [`docs/dev/testing.md`](../testing.md) and [`docs/dev/manual-testing.md`](../manual-testing.md).

---

## Core Governance Philosophy

1. **Mechanisms Over Exhortations**:
   Rules and system boundaries must be enforced by concrete mechanisms: compiler attributes (`[[nodiscard]]`, C23 static assertions), link-time library separation, or deterministic automated test assertions (`tools/optics check`). Natural-language "swearing" in comments is forbidden.
2. **Anti-Inflation & Invariant Realism**:
   A sharp line separates **System Invariants** (non-negotiable structural red-lines with high blast radius), **Domain Contracts** (internal state machines and enums), **Design Specifications** (visual layout constants and tokens), and **Implementation Notes**.
3. **Flat & Pragmatic**:
   Zero arbitrary nesting or nested "core" junk drawers. Every governance document directly addresses an active engineering concern in Optics.
4. **Single Opts Descriptor Paradigm**:
   Every public function accepts exactly one typed options struct pointer (`const module_<item>_opts *opts`) and returns a structured response. Suffix variations (`_ex`, `_wrapped`) and floating state modifiers are strictly prohibited.
5. **Self-Contained Epistemic Lineage**:
   All architectural decisions and references must resolve directly within `docs/adr/` (ADR-0001 through current).
