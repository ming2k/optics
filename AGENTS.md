# AGENTS.md

Instructions for AI coding assistants working in the `optics` repository.

---

## 1. Repository Identity & Mission

Optics is a unified C23 graphics and UI monorepo.
The core stack consists of five closely integrated libraries in `libs/`:
1. **`flux`**: Foundational Vulkan rendering engine (2D canvas, 3D scene-graph, compute pipelines, headless software CPU backend).
2. **`lens`**: Immediate-mode UI engine (headless layout, retained reactive tree, widget state, draw-list emission).
3. **`iris`**: Application L3 toolkit (OS integration, window management, cross-platform event loops for Wayland, Win32, Cocoa).
4. **`prism`**: Physical material library (dielectric liquid glass, acrylic, mica atop flux runtime).
5. **`anim`**: Motion vocabulary library (closed-form non-divergent analytic springs, easing curves, motion-adaptive smoothing).

---

## 2. Policy & Modification Guardrails

- Adhere to the System Invariants defined in `docs/governance/documentation/core/invariants.md`.
- Public documentation (`docs/{tutorials,how-to,reference,explanation}/`) must never link into internal developer docs (`docs/dev/`).
- Architectural decisions must be recorded under `docs/adr/` with valid Frontmatter schema.

---

## 3. Toolchain & Verification

- **Automated Verification**: Before submitting changes, run:
  ```bash
  meson compile -C build
  meson test -C build
  docgov check
  ```

<!-- BEGIN DOCGOV DIRECTIVES -->
## Documentation Governance Directives

You are bound by repository invariants. Violations will fail CI (`docgov check`).

### 1. Machine Invariants (Pre-Submit Checklist)
- `[INV-LINT-01] Location Sanitization`: Never create arbitrary Markdown files at the repository root.
- `[INV-LINT-02] Contributor Firewall`: Public docs (`docs/{tutorials,how-to,reference,explanation}/`) must NEVER link into internal docs (`docs/dev/`).
- `[INV-LINT-03] Frontmatter Schema`: ADRs must contain valid Frontmatter with standardized status enum.
- `[INV-LINT-04] Code-Doc Sync`: Modifying monitored paths in `src/` requires updating `docs/` in the same change.
- `[INV-LINT-05] Agent Directives Binding`: Ensure this docgov directives block is retained in agent configuration.

### 2. Cognitive & Architecture Protocols (Thinking Framework)
- `[INV-AGENT-01] Negative Knowledge`: Every new ADR MUST contain a 'Rejected Alternatives' section explaining why discarded options were not chosen.
- `[INV-AGENT-02] Context Routing & Chesterton's Fence`:
  - In feature generation: NEVER use docs marked `status: superseded` or `status: rejected` as active designs (prevents resurrecting dead patterns).
  - In refactoring/investigation: MUST retrieve `superseded` docs as negative constraints (learn from historical failure modes).
- `[INV-AGENT-03] Blameless Postmortem`: Postmortems MUST analyze system defense failures and detection gaps. Attribution of personal human blame is strictly prohibited.

### 3. Canonical Governance Knowledge & Context
Before drafting or restructuring documentation, inspect the local governance specifications:
- 4D Coordinate Tensor: `docs/governance/documentation/core/taxonomy.md`
- System Invariants Constitution: `docs/governance/documentation/core/invariants.md`
- Technical Voice & Link Contracts: `docs/governance/documentation/core/style.md`
- ADR & Architecture RFC Standard: `docs/governance/documentation/profiles/architecture/adr.md`
- Quality & Verification Guides: `docs/governance/documentation/profiles/validation/testing.md`

### 4. Fast Verification
Before completing any task, run:
```bash
docgov check
```
<!-- END DOCGOV DIRECTIVES -->
