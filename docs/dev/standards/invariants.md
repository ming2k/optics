# Invariant & Constraint Governance

This document codifies the definition, categorization, and lifecycle of invariants in Optics. It establishes a strict policy against pseudo-invariant tag proliferation, ensuring that invariants remain high-value architectural anchors rather than ceremonial comments.

---

## 1. Core Principle: Mechanisms Over Exhortations

In software engineering, an invariant is a condition that must unconditionally hold across all valid states of a system. Projects suffer from "invariant inflation" when mundane UI styles, layout heuristics, or ephemeral flags are labeled as invariants without verification mechanics.

**The Golden Rule**:
> **If a rule has neither a compiler-level proof nor an automated test assertion, it is NOT an invariant — it is merely an aspiration.**

---

## 2. Constraint Admissibility Matrix

Every architectural and operational rule maps deterministically into one of four categories:

| Category | Definition & Scope | Identifier Policy | Mandatory Enforcement Mechanism |
| :--- | :--- | :--- | :--- |
| **System Invariant** | Non-negotiable structural red-lines with cross-boundary blast radius (e.g., library boundaries, material isolation, zero-alloc loop, C23 purity). Max ~15 across the repo. | Formally ratified in ADRs (`[INV-ARCH-*]`, `[INV-CORE-*]`) | 1. Linker/library dependency firewalls<br>2. Dedicated check command (`tools/optics check <target>`)<br>3. Compiler attributes (`[[nodiscard]]`, `static_assert`) |
| **Domain Contract** | State-machine transitions, function signatures, and memory lifecycles within a single library or module. | **NO global `INV-` tag**.<br>Expressed as named structs / enums. | 1. Typed enums & struct layouts<br>2. Assertion guards (`assert`)<br>3. Unit/integration tests |
| **Design Specification** | Layout padding, font scales, spring stiffness constants, color palette tokens. | **NO `INV-` tag**.<br>Expressed as design tokens / constants. | 1. Constant definitions (e.g. style tokens / metrics)<br>2. Snapshot or golden pixel tests |
| **Implementation Note** | Transient workarounds, compiler bug workarounds, localized edge-case guards. | **NO `INV-` tag**.<br>Standard code comments. | Direct regression unit test |

---

## 3. The 3-Condition Gate for System Invariants

A rule may be elevated to a **System Invariant** if and only if it satisfies all three criteria:

1. **High Blast Radius**: Violation results in memory corruption, frame stutter, cross-library dependency inversion, or ABI breakages.
2. **Durable Architecture**: It represents a foundational pillar that will not become obsolete due to routine feature iterations or localized redesigns.
3. **Mechanically Enforced**: It specifies an automated enforcement mechanism (compiler type check, static assertion, or deterministic CI script).

---

## 4. Optics Core Invariants Catalog

The following canonical invariants are mechanically enforced in Optics:

| Invariant | Name | Policy | Enforcement Mechanism |
| :--- | :--- | :--- | :--- |
| `[INV-ARCH-MATERIAL]` | Material Isolation (ADR-0063) | `flux` must never reference named materials or `prism_` symbols. | `tools/optics check boundary` (`tools/guardrails/boundary.py`) |
| `[INV-ARCH-STDOUT]` | Stdout Purity | Shared library TUs must never write to `stdout` (`printf`, `puts`). | `tools/optics check stdout` (`tools/guardrails/stdout_purity.py`) |
| `[INV-ARCH-VERSION]` | Monorepo Version Lockstep | All sibling libraries (`flux`, `lens`, `iris`, etc.) share the exact version. | `tools/optics check lockstep` (`tools/guardrails/lockstep.py`) |
| `[INV-ARCH-FORMAT]` | Pure C23 Formatting | Every C source file conforms strictly to `.clang-format` (Clang-Format 22.x). | `tools/optics check format` (`tools/guardrails/formatting.py`) |
| `[INV-ARCH-ARENA]` | Zero Heap Alloc Frame Walk | Hot frame traversal, layout passes, and render walks use only per-frame arena memory. | Arena memory tracking & integration tests |
| `[INV-ARCH-OPTS]` | Single Opts Descriptor | Every public entry point accepts a single `const *opts` descriptor struct. | Static analysis & symbol generator `tools/guardrails/symbols.py` |
| `[INV-ARCH-SYMBOLS]` | Public Symbol Parity | `docs/reference/symbols.md` is strictly synchronized with header exports. | `tools/optics check symbols` (`tools/guardrails/symbols.py`) |
| `[INV-DOC-FIREWALL]` | Audience Firewall | Public documentation must never link to internal developer docs (`docs/dev/**`). | `tools/optics check docs` (`tools/guardrails/doc_links.py`) |
| `[INV-DOC-ROOT]` | Clean Root Whitelist | Only canonical markdown files exist at repository root. | `tools/optics check docs` (`tools/guardrails/doc_links.py`) |

---

## 5. Degazetting & Retirement Policy

1. **Pseudo-Invariants**: All pseudo-tags previously minted for visual design or local state transitions are formally degazetted. Use named types, design constants, and doc-comments.
2. **True Invariants**: When a system invariant evolves, it follows the **In-Place Lifecycle**:
   - `Active`: Verified on current `main`.
   - `Superseded`: Formally replaced by a successor invariant via an ADR (`superseded_by: ADR-NNNN`).
   - `Deprecated`: Retired due to subsystem removal, with context preserved in ADR history.
