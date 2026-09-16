# Optics Developer Tooling & Guardrails

The `tools/` directory hosts the **machine-enforced architecture guardrails, build-time code generators, and toolchain configurations** for the Optics monorepo.

> **Policy (ADR-0098):** `tools/` is strictly governed. It is **not** a dumping ground for ad-hoc scratch scripts or temporary data files. Every utility here must be actively wired into CI pipelines (`.github/workflows/`) or the Meson build system (`meson.build`).

---

## Unified Verification Gate

Before pushing code or opening a pull request, run the single authoritative gatekeeper:

```bash
# Verify all guardrails in sequence
./tools/check-all.sh

# Automatically format code and refresh symbol documentation
./tools/check-all.sh --fix
```

---

## Tool Catalog & Taxonomy

### 1. Architecture & Quality Guardrails (CI Enforced)

| Tool | Enforced Policy | Description |
|------|-----------------|-------------|
| [`check-all.sh`](check-all.sh) | ADR-0098 | Single-entry unified runner across all 6 architecture and quality guardrails. |
| [`check-material-boundary.sh`](check-material-boundary.sh) | ADR-0063 | Asserts `flux` never references named materials (`prism_`, `liquid_glass`); prevents layer pollution. |
| [`check-version-lockstep.sh`](check-version-lockstep.sh) | Monorepo lockstep | Asserts all sibling libraries (`flux`, `lens`, `iris`, `prism`, `anim`) share the exact project version. |
| [`check-no-library-stdout.sh`](check-no-library-stdout.sh) | Library hygiene | Asserts shared library TUs never write to `stdout` (`printf`, `puts`), reserving stdout for IPC wires. |
| [`check-format.sh`](check-format.sh) | Clang-Format 22.x | Asserts every C source file is byte-identical to root `.clang-format` output (`--fix` to reformat). |
| [`check-tidy.sh`](check-tidy.sh) | Zero-warning policy | Runs `clang-tidy` against all shipped library translation units; zero-warning baseline. |

### 2. Build Infrastructure & Cross-Compilation

| Tool | Consumed By | Description |
|------|-------------|-------------|
| [`spv2h.py`](spv2h.py) | `meson.build` | Converts `.spv` SPIR-V binary files into `.h` C byte arrays for compilers lacking C23 `#embed` (MSVC, Apple Clang per ADR-0053). |
| [`shader_manifest.py`](shader_manifest.py) | `libs/flux/meson.build` | Generates shader byte-hash manifest headers so `ccache` reliably invalidates cache on shader edits. |
| [`zig-win32-check.sh`](zig-win32-check.sh) | `.github/workflows/ci-cross.yml` | Cross-compiles Win32 C sources using Zig (`x86_64-windows-gnu`) without requiring a Windows runner. |

### 3. Symbol Tracking & Documentation Governance

| Tool | Consumed By | Description |
|------|-------------|-------------|
| [`gen_symbols.py`](gen_symbols.py) | CI & Docs | Extracts all exported C symbols from public headers into `docs/reference/symbols.md` (`--check` fails on drift). |
| [`verify.sh`](verify.sh) | CI & Governance | Verifies documentation signatures against `.manifest.json` under Documentation Governance Protocol v5.1.0. |

### 4. Toolchain Configuration Files

| File | Consumer | Description |
|------|----------|-------------|
| [`abi-supprs.abignore`](abi-supprs.abignore) | `libabigail` (CI) | ABI change suppression rules ensuring semver-compliant shared object compatibility. |
| [`lsan.supp`](lsan.supp) | ASan / LSan | LeakSanitizer suppression list for known process-lifetime upstream allocations (e.g. fontconfig caches). |

---

## Governance Rules

1. **Deterministic Execution:** All shell scripts must run with `set -euo pipefail` (or `set -eu`), require no exotic dependencies beyond bash/sh and python3 standard library, and exit with code `0` on success and non-zero on failure.
2. **Fast Feedback:** Guardrail checks must execute in milliseconds to provide instant developer feedback.
3. **No Unwired Scripts:** Any script not actively executed by `check-all.sh`, `.github/workflows/`, or `meson.build` will be pruned.
