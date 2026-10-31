# Documentation Taxonomy & Information Architecture

This document defines the organization, scope boundaries, and classification of all documentation assets in Optics.

---

## 1. The Four-Quadrant System (Diátaxis)

Public documentation in `docs/` strictly follows the Diátaxis information architecture:

```text
               PRACTICAL                   THEORETICAL
          ┌─────────────────────────┬─────────────────────────┐
          │  Tutorials              │  How-To Guides          │
LEARNING  │  (Learning-Oriented)    │  (Task-Oriented)        │
          │  `docs/tutorials/`      │  `docs/how-to/`         │
          ├─────────────────────────┼─────────────────────────┤
          │  Reference              │  Explanation            │
WORKING   │  (Information-Oriented) │  (Understanding-Orient) │
          │  `docs/reference/`      │  `docs/explanation/`    │
          └─────────────────────────┴─────────────────────────┘
```

- **`docs/tutorials/`**: Guided, hands-on lessons taking a newcomer through building their first 2D/3D applications. Focuses on learning and onboarding confidence.
- **`docs/how-to/`**: Step-by-step problem-solving recipes for specific graphics or UI tasks (e.g. applying a blur effect, recording and presenting a frame, CPU fallback). Focuses on accomplishing a practical outcome.
- **`docs/reference/`**: Precise technical descriptions of C APIs, exported symbols, composition graphs, shader effects, and threading contracts without tutorial narrative.
- **`docs/explanation/`**: Conceptual deep dives, physical optics models (Liquid Glass), value-semantic rendering, and Vulkan architecture. Focuses on understanding the system.

---

## 2. Documentation Spaces & Audiences

Documentation is structured into two distinct surfaces separated by a one-way visibility boundary:

```text
┌─────────────────────────────────────────────────────────────┐
│ Public Surface (Users, Integrators, Application Developers) │
│ `docs/tutorials/`, `how-to/`, `reference/`, `explanation/`  │
└──────────────────────────────┬──────────────────────────────┘
                               │
                               │ Scope Boundary: public docs must
                               │ never link into internal space.
                               ▼
┌─────────────────────────────────────────────────────────────┐
│ Engineering Surface (Contributors, Maintainers, Agents)     │
│ `docs/dev/` (internals, standards, testing runbooks)        │
│ `docs/adr/` (canonical architectural decision records)      │
└─────────────────────────────────────────────────────────────┘
```

1. **User Surface**: Addressed to application developers and library consumers. Emphasizes API usage, rendering examples, and high-level principles.
2. **Engineering Surface**: Addressed to engine contributors, maintainers, and development agents. Emphasizes internal subsystem blueprints, standards, and verification runbooks.
3. **Audience Firewall (`[INV-DOC-FIREWALL]`)**:
   - Public documentation surfaces (`docs/{tutorials,how-to,reference,explanation}/`) **must never contain relative links pointing into the internal maintainer space (`docs/dev/**`)**.
   - Links into `docs/adr/` remain permitted: a public reader may follow an architectural decision record, but not an internal contributor checklist or runbook.
4. **Reverse Linking Permitted**: Internal developer documents may freely link outward to public reference and how-to guides.

Enforcement is mechanical via `tools/optics check docs` (`tools/guardrails/doc_links.py`).

---

## 3. Repository Root Whitelist (`[INV-DOC-ROOT]`)

To keep the repository root clean, professional, and clutter-free, only canonical top-level files are permitted at the root:
- `README.md`
- `CHANGELOG.md`
- `CONTRIBUTING.md`
- `AGENTS.md`
- `LICENSE`
- `SECURITY.md`

All other documentation must reside within organized directories under `docs/`.
