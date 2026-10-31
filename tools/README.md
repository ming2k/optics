# Optics Developer Tooling & Guardrails

The `tools/` directory hosts the **machine-enforced architecture guardrails, build-time code generators, CI runners, and toolchain configurations** for the Optics monorepo.

> **Architecture & Quality Policy:** `tools/` is strictly governed into structured domains. Every utility here is actively consumed by the unified developer CLI (`tools/optics`), GitHub Actions workflows (`.github/workflows/`), or the Meson build system (`meson.build`).

---

## 1. Unified Developer CLI (`tools/optics`)

All local verification and automatic remediation workflow is consolidated under the unified cross-platform CLI:

```bash
# Run all core architecture & quality guardrails in sequence
./tools/optics check

# Run a specific guardrail check (format, boundary, lockstep, stdout, symbols, docs, tidy)
./tools/optics check boundary
./tools/optics check lockstep

# Run clang-tidy static analysis against library translation units
./tools/optics check tidy

# Automatically format C sources and synchronize public symbol documentation
./tools/optics fix

# List available guardrails, domains, and policy mapping
./tools/optics list
```

---

## 2. Tooling Directory Structure & Taxonomy

```text
tools/
├── optics                      # Executable CLI launcher
├── optics.py                   # Pure Python 3 CLI runner (check, fix, list)
├── README.md                   # Tooling system documentation
│
├── guardrails/                 # Domain: Architectural Invariants & Quality Enforcement
│   ├── boundary.py             # Layer 0 flux isolation check (syntax-aware, ADR-0063)
│   ├── doc_links.py            # Hyperlinks, Audience Firewall & ADR lineage check (ADR-0125)
│   ├── formatting.py           # Pinned clang-format 22.x verification and in-place fix
│   ├── lockstep.py             # Monorepo version lockstep verification (ADR-0098)
│   ├── stdout_purity.py        # Library diagnostics purity check (syntax-aware)
│   ├── symbols.py              # Exported public symbols tracker & docs generator
│   └── tidy.py                 # Clang-tidy static analysis runner (zero-warning policy)
│
├── build/                      # Domain: Build System Code Generators (consumed by Meson)
│   ├── spv2h.py                # SPIR-V to C header byte array generator (ADR-0053)
│   └── shader_manifest.py      # Shader content-hash manifest generator (ccache tracking)
│
├── ci/                         # Domain: CI Runners & Platform Emulation
│   └── zig-win32-check.sh      # Win32 cross-compilation smoke runner via Zig
│
└── config/                     # Domain: Toolchain Configuration & Suppression Lists
    ├── abi-supprs.abignore     # Libabigail ABI suppression rules
    └── lsan.supp               # LeakSanitizer suppression list
```

---

## 3. Domain Catalog

### A. Architecture & Quality Guardrails (`guardrails/`)

| Tool | Enforced Policy | Description |
|------|-----------------|-------------|
| [`guardrails/boundary.py`](guardrails/boundary.py) | ADR-0063 | Syntax-aware check asserting `flux` never references named materials (`prism_`, `liquid_glass`). |
| [`guardrails/lockstep.py`](guardrails/lockstep.py) | Monorepo lockstep | Asserts all sibling libraries and bindings share the exact project version from `meson.build`. |
| [`guardrails/stdout_purity.py`](guardrails/stdout_purity.py) | Library hygiene | Asserts shared library TUs never write to `stdout` (`printf`), reserving stdout for IPC wires. |
| [`guardrails/formatting.py`](guardrails/formatting.py) | Clang-Format 22.x | Asserts every C source file is byte-identical to root `.clang-format` output (`--fix` to reformat). |
| [`guardrails/symbols.py`](guardrails/symbols.py) | ADR-0098 | Extracts exported C symbols from public headers into `docs/reference/symbols.md`. |
| [`guardrails/doc_links.py`](guardrails/doc_links.py) | ADR-0125 | Verifies documentation hyperlinks, anchor resolution, Audience Firewall, and ADR lineage. |
| [`guardrails/tidy.py`](guardrails/tidy.py) | Zero-warning policy | Runs `clang-tidy` against all shipped library translation units; zero-warning baseline. |

### B. Build Infrastructure (`build/`)

| Tool | Consumed By | Description |
|------|-------------|-------------|
| [`build/spv2h.py`](build/spv2h.py) | `meson.build` | Converts `.spv` SPIR-V binary files into `.h` C byte arrays for compilers lacking C23 `#embed`. |
| [`build/shader_manifest.py`](build/shader_manifest.py) | `libs/flux/meson.build` | Generates shader byte-hash manifest headers so `ccache` reliably invalidates cache on shader edits. |

### C. CI Infrastructure (`ci/`)

| Tool | Consumed By | Description |
|------|-------------|-------------|
| [`ci/zig-win32-check.sh`](ci/zig-win32-check.sh) | `.github/workflows/ci-cross.yml` | Cross-compiles Win32 C sources using Zig (`x86_64-windows-gnu`) without requiring a Windows runner. |

### D. Toolchain Configuration Files (`config/`)

| File | Consumer | Description |
|------|----------|-------------|
| [`config/abi-supprs.abignore`](config/abi-supprs.abignore) | `libabigail` (CI) | ABI change suppression rules ensuring semver-compliant shared object compatibility. |
| [`config/lsan.supp`](config/lsan.supp) | ASan / LSan | LeakSanitizer suppression list for known process-lifetime upstream allocations (e.g. fontconfig caches). |

---

## 4. Governance Rules

1. **Single CLI Entrypoint:** All developer-facing verification and fixes run through `./tools/optics`. No detached wrapper scripts are allowed.
2. **Domain Encapsulation:** Every file belongs to a domain subdirectory (`guardrails/`, `build/`, `ci/`, `config/`).
3. **Deterministic & Fast:** Guardrails execute in milliseconds using Python 3 standard library and native compilers, with zero exotic package dependencies.
