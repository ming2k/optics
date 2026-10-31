# Architectural Decision Records (ADR) Standards

This document establishes the qualification criteria, lifecycle progression, frontmatter schema, and canonical template for Architectural Decision Records (ADRs) in Optics.

---

## 1. Intent & Qualification (The 3-Question Test)

To prevent ADR inflation and maintain a high-signal architectural registry, author an ADR only if a proposal meets **at least two** of these criteria:

1. **High Reversal Cost**: Would reversing this decision require multi-week refactoring or breaking public C ABI / Rust binding migration paths?
2. **Cross-Boundary Blast Radius**: Does it alter library boundaries under `libs/`, public descriptor conventions, shader pipelines, or frame memory models?
3. **Generates Binding Red-Lines**: Does it establish hard structural boundaries that developers, linters, and AI assistants must follow?

### Anti-Patterns to Reject
- **The Triviality Smear**: Authoring ADRs for routine implementation choices (e.g. adding an internal helper function or renaming a local variable).
- **The Ephemeral Workaround**: Recording temporary bug fixes, transient compiler flags, or local driver quirks.
- **The Toothless Declaration**: Vague prose lacking enforceable constraints or clear behavioral boundaries.
- **The Post-Hoc Justification**: Authoring an ADR after code is merged with zero genuine consideration of discarded alternatives.

---

## 2. In-Place Lifecycle & Frontmatter Schema

ADRs evolve strictly in-place through YAML frontmatter transitions. Files are never physically moved to an `archive/` directory:

```text
       [ draft ] ───► [ accepted ] ───► [ superseded ]
           │
           └───► [ rejected ]
```

- **draft**: Proposal under active design deliberation.
- **accepted**: Ratified invariant or architectural contract binding on the repository.
- **superseded**: Formally replaced by a subsequent ADR (`superseded_by: ADR-NNNN`).
- **rejected**: Discarded alternative preserved in-place for negative knowledge audit.

### Frontmatter Schema
```yaml
---
id: ADR-NNNN
title: "[Title of Decision]"
status: accepted # [draft | accepted | superseded | rejected]
date: YYYY-MM-DD
scope: [libraries or subsystems affected, e.g. flux, lens, iris]
superseded_by: null # e.g., ADR-0016 (mandatory when status is superseded)
negative_knowledge: true
---
```

---

## 3. Authoritative ADR Template

```markdown
---
id: ADR-NNNN
title: "[Title of Decision]"
status: draft
date: YYYY-MM-DD
scope: [modules/libraries affected]
superseded_by: null
negative_knowledge: true
---

# NNNN. [Title of Decision]

- Status: Draft | Accepted | Superseded | Rejected
- Date: YYYY-MM-DD
- Deciders: Optics Architecture Working Group
- Consulted: [Libraries / Subsystems Consulted]
- Informed: Core Engineering
- Amends / Supersedes: [ADR-XXXX](XXXX-title.md)

---

## Context and Problem Statement

[Describe the context, technical challenges, and why existing abstractions are insufficient.]

---

## Decision Drivers

1. [Driver 1]
2. [Driver 2]

---

## Architectural Decisions

### 1. [Decision Component 1]
[Detailed architectural specification, struct definitions, or memory layouts.]

### 2. [Decision Component 2]
[Behavioral rules and invariants.]

---

## Consequences

### Positive
- [Benefit 1]
- [Benefit 2]

### Negative & Trade-offs
- [Trade-off 1]

---

## Rejected Alternatives & Negative Knowledge

### 1. [Alternative Name]
- **Why considered**: [Rationale]
- **Why rejected**: [Failure mode / flaw]

---

## Links

- Related Standards: [Architecture](architecture.md), [Invariants](invariants.md)
- ADR Index: [ADR Index](index.md)
```
