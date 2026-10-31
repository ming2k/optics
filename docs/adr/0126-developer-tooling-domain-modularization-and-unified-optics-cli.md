---
id: ADR-0126
title: "Developer Tooling Domain Modularization and Unified Optics CLI"
status: accepted
date: 2026-11-01
scope: tools, guardrails, ci, build, docs
superseded_by: null
negative_knowledge: true
---

# ADR-0126: Developer Tooling Domain Modularization and Unified Optics CLI

- Status: Accepted
- Date: 2026-11-01
- Deciders: Optics Architecture Working Group, Developer Experience Team
- Consulted: Platform Architecture, Engine Core, Tools Maintainers
- Informed: Core Engineering
- Amends: [ADR-0098](0098-developer-tooling-and-architecture-guardrails.md), [ADR-0125](0125-docgov-decoupling-standards-consolidation-and-documentation-modernization.md)

---

## Context and Problem Statement

ADR-0098 previously consolidated scattered pre-commit checks into `./tools/check-all.sh` and established an architectural taxonomy. However, the root `tools/` directory retained substantial structural liabilities:

1. **Heterogeneous Root Clutter**: Build generators (`spv2h.py`, `shader_manifest.py`), shell lint wrappers (`check-*.sh`), sanitizer configs (`lsan.supp`), and CI smoke scripts (`zig-win32-check.sh`) sat flat in `tools/`, projecting tech debt and informal hackiness.
2. **Text-Level Grep Fragility (False Positives)**: Guardrails like `check-material-boundary.sh` and `check-no-library-stdout.sh` used crude shell `grep`. Comments or docstrings discussing architectural comparisons (e.g. `// Note: unlike liquid_glass in prism...`) triggered false rejections, forcing developers to distort comments to pass CI.
3. **Shell Fragility & Windows Incompatibility**: Multiple POSIX `.sh` scripts containing bespoke directory switching and piping failed to run in native Windows developer environments without MSYS2/WSL.
4. **Historical Wrapper Accumulation**: Redundant shell shims (`verify.sh`, `check-all.sh`) layered over underlying scripts rather than exposing a direct, structured toolchain.

---

## Architectural Decisions

1. **Domain-Driven Directory Partitioning**:
   Reorganize `tools/` into four orthogonal domain directories:
   - `tools/guardrails/`: Pure Python, syntax-aware architectural invariant enforcement (`boundary.py`, `lockstep.py`, `stdout_purity.py`, `formatting.py`, `symbols.py`, `doc_links.py`, `tidy.py`).
   - `tools/build/`: Build-time code generators invoked directly by Meson (`spv2h.py`, `shader_manifest.py`).
   - `tools/ci/`: Dedicated CI runners and platform cross-compilation harnesses (`zig-win32-check.sh`).
   - `tools/config/`: Toolchain suppressions and sanitizer definitions (`abi-supprs.abignore`, `lsan.supp`).

2. **Unified Cross-Platform CLI (`tools/optics`)**:
   Implement a single-entry Python CLI (`tools/optics.py` with executable launcher `tools/optics`) as the sole public gateway for developer verification:
   - `./tools/optics check [target]` (default: all core quality guardrails; supports granular targets like `boundary`, `lockstep`, `tidy`).
   - `./tools/optics fix` (automated code reformatting and symbol table synchronization).
   - `./tools/optics list` (self-documenting tool metadata and invariant mapping).

3. **Syntax-Aware Comment Stripping**:
   Guardrails parse tokens outside C/GLSL comments and string literals, preventing false-positive rejections on architectural prose.

4. **Zero Historical Baggage**:
   Prune all legacy flat shims (`tools/check-all.sh`, `check-material-boundary.sh`, `check-version-lockstep.sh`, `check-no-library-stdout.sh`, `check-format.sh`, `check-tidy.sh`, `verify.sh`, `check-doc-links.py`, `gen_symbols.py`). CI workflows and documentation update directly to `./tools/optics`.

---

## Negative Knowledge & Discarded Alternatives

- **Retaining Shell Shims for Backward Compatibility**: Discarded. Preserving 6+ shell forwarders perpetuates directory bloat and ambiguity over the canonical entrypoint. A clean break enforces structural hygiene across all documentation and CI pipelines.
- **Node.js / Rust-Based Tooling Runtime**: Discarded. Optics requires zero exotic package dependencies beyond the standard C23 toolchain and Python 3.

---

## Consequences

- The `tools/` root directory is clean, containing only `optics` CLI, `optics.py`, `README.md`, and four domain subdirectories.
- Developer verification runs in milliseconds across all platforms with clear terminal diagnostics and granular subcommands.
- Architectural invariant enforcement is resilient to comment prose and free of legacy wrappers.
