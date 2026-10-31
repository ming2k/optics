# Agent Directives

## 1. Operational Guardrails (Hard Constraints)
- **Prohibited Git Commands**: NEVER execute `git stash`, `git branch`, or `git reset`.
- **Command Termination**: All commands MUST be finite and self-terminating. Never start background servers or unbounded loops without a strict timeout.
- **Clean Git State**: Do not leave scratch files, temporary binaries, or untracked test logs in the working tree.

## 2. Core Mental Model & Subsystem Boundaries
- **Micro-Kernel Topology**: Optics is a high-performance C23 graphics and UI stack organized into seven orthogonal libraries under `libs/`:
  - `flux`: Vulkan 1.3 RHI, 2D vector canvas (GPU & headless CPU fallback `<flux/canvas_cpu.h>`), 3D draw core, compute pipelines, analytic curve kernel.
  - `glyph`: HarfBuzz-backed text shaping, layout metrics, and dynamic texture atlas caching.
  - `vista`: Scene graph orchestration, cameras, hierarchical transforms, and spatial bounds.
  - `prism`: Physical surface materials: dielectric Liquid Glass (refraction, dispersion, contact AO), Frosted Glass, Acrylic, and Mica.
  - `transit`: Provable motion physics, closed-form analytic spring integration, and parametric easing.
  - `lens`: Immediate-mode UI engine, layout (Grid/Flex/Flow), retained store state machines, and accessibility tree export (A11y).
  - `iris`: Native OS windowing, event pump, display pacing (Wayland, Win32, Cocoa), pre-lens event seam (ADR-0107), and dual-thread pipeline (ADR-0124).
- **Pure C23 Baseline**: Idiomatic modern C (`-std=c23`) across all modules:
  - Pointers: Always use `nullptr` (never `NULL` or `0`).
  - Zero-initialization: Always use `{}` (never `{0}`).
  - Enums: Explicit underlying types (`typedef enum name : uint32_t { ... }`).
  - Attributes: Standard C23 `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
  - Assertions: Standard C23 `static_assert(cond, msg)`.
- **Single Opts Descriptor Paradigm**: Every public functional entry point accepts **exactly one** typed descriptor pointer (`const module_<item>_opts *opts`) and returns a structured response. Dual API tiers (`_ex`), parameter suffixes (`_wrapped`, `_primary`), and floating state modifiers are strictly forbidden.
- **Material Boundary Separation (ADR-0063)**: `flux` (Layer 0) must NEVER reference named materials (`prism_`, `liquid_glass`) or higher-level shaders. Enforced mechanically by `tools/optics check boundary`.
- **Zero-Alloc Frame Walk**: Render walk and widget layout passes (`lens_begin` ➔ `lens_end`) must never invoke libc `malloc`/`calloc`/`realloc`/`free`. Memory borrows strictly from the per-frame arena (`ui->arena`).
- **Library Stdout Purity**: Shared library TUs must never write to `stdout` (`printf`, `puts`), reserving stdout for IPC. Use `stderr` for diagnostics.

## 3. Engineering Discipline
- **Mechanisms Over Exhortations**: Rely on C23 compiler attributes (`[[nodiscard]]`, `static_assert`), link-time library separation, and automated tests to guarantee invariants. Never rely on natural-language comments alone.
- **Anti-Inflation**: Do not mint pseudo-invariant tags. Model states via typed enums, and layout metrics via design constants (`docs/dev/standards/invariants.md`).
- **Audience Firewall**: Public documentation (`docs/{tutorials,how-to,reference,explanation}/`) MUST NEVER link to internal contributor docs (`docs/dev/**`). Links into `docs/adr/` remain permitted.
- **Atomic Verification**: Always verify changes through the unified gatekeeper `./tools/optics check` before declaring tasks complete.
- **Clean Epistemic Lineage**: References must resolve to canonical in-repo ADRs (`docs/adr/0001`+).

## 4. On-Demand Standards Routing
Do not guess repository rules or rely on stale assumptions. Consult authoritative specifications as needed:
- Standards Suite Catalog: `docs/dev/standards/README.md`
  - System Architecture & 5-Gate Review Matrix: `docs/dev/standards/architecture.md`
  - Invariant & Constraint Policy: `docs/dev/standards/invariants.md`
  - C23 API Design & Opts Conventions: `docs/dev/standards/api-design.md`
  - UI Component Intake & 4-Layer Purity: `docs/dev/standards/components.md`
  - Surface Materials Governance (Prism): `docs/dev/standards/materials.md`
  - Image Effects Governance (Flux / Prism): `docs/dev/standards/effects.md`
  - Platform Windowing & Event Seams (Iris): `docs/dev/standards/backends.md`
  - ADR Standards & Template: `docs/dev/standards/adr.md`
  - Documentation Taxonomy & Audience Firewall: `docs/dev/standards/taxonomy.md`
  - Markdown & Hyperlink Style: `docs/dev/standards/style.md`
  - Engineering Workflow & Gates: `docs/dev/standards/workflow.md`
  - Blameless Incident Postmortems: `docs/dev/standards/postmortems.md`
- Testing Strategy & Execution: `docs/dev/testing.md`
- Manual Testing Runbooks: `docs/dev/manual-testing.md`
