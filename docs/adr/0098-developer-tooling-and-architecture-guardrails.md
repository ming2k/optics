# ADR-0098: Developer tooling taxonomy, architecture guardrails, and unified verification

- Status: Accepted
- Date: 2026-09-15
- Scope: `tools/` directory, CI workflow invocations, and developer pre-commit verification.
- Depends on: [ADR-0023](0023-unified-monorepo-build.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0097](0097-canonical-example-taxonomy-and-governance.md).

## Context

The `tools/` directory contains utilities critical to Optics' development lifecycle:
Vulkan SPIR-V code generators (`spv2h.py`), shader cache tracking (`shader_manifest.py`),
material boundary enforcement (`check-material-boundary.sh`), ABI suppression rules (`abi-supprs.abignore`),
and documentation governance (`verify.sh`).

While each utility served an essential architectural function, the directory suffered from organizational drift:
1. **Scattered verification commands:** Verifying tree compliance required manually invoking six separate
   scripts with differing flags and arguments. Developers frequently pushed changes that failed CI on
   mechanical errors (e.g. forgotten `gen_symbols.py` updates or formatting drift).
2. **Wrapper script redundancy:** A 5-line wrapper script (`check-docs-governance.sh`) merely forwarded to
   `verify.sh`, creating an unnecessary layer of indirection.
3. **Lack of a tooling charter:** Without clear documentation, developers questioned whether `tools/`
   was an ad-hoc scratchpad or core infrastructure, risking the accumulation of unmaintained one-off scripts.

## Decision

1. **Implement `tools/check-all.sh` as the single authoritative verification gate:**
   Sequentially executes all machine guardrails with fast-fail semantics and millisecond execution time:
   - `[1/6] Code formatting (clang-format 22.x)`
   - `[2/6] Material boundary separation (ADR-0063)`
   - `[3/6] Version lockstep across monorepo libraries`
   - `[4/6] Library stdout purity (stderr only)`
   - `[5/6] Public symbols reference freshness (docs/reference/symbols.md)`
   - `[6/6] Documentation governance protocol verification`
   Supports `--fix` to automatically reformat code and refresh the symbol reference in one command.

2. **Establish a strict four-category taxonomy documented in `tools/README.md`:**
   - **Architecture & Quality Guardrails:** `check-all.sh`, `check-material-boundary.sh`,
     `check-version-lockstep.sh`, `check-no-library-stdout.sh`, `check-format.sh`, `check-tidy.sh`.
   - **Build Infrastructure & Cross-Compilation:** `spv2h.py`, `shader_manifest.py`, `zig-win32-check.sh`.
   - **Symbol & Documentation Governance:** `gen_symbols.py`, `verify.sh`.
   - **Toolchain Configurations:** `abi-supprs.abignore`, `lsan.supp`.

3. **Prune redundant wrappers:**
   Remove `tools/check-docs-governance.sh`. CI and scripts invoke `tools/verify.sh .` directly.

4. **Enforce the "No Unwired Scripts" policy:**
   Every file in `tools/` must be actively consumed by `meson.build`, `.github/workflows/`, or
   `tools/check-all.sh`. Ad-hoc, temporary, or unlinked scripts are strictly prohibited.

## Consequences

- Pre-commit verification is consolidated into a single memorable command (`./tools/check-all.sh`).
- CI pipelines and local developer environments share the exact same gatekeeper logic.
- The `tools/` directory is clean, self-documenting, and free of legacy wrappers.
