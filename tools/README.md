# Optics Developer Tooling

The `tools/` directory hosts developer convenience scripts, CI runners, and toolchain configurations for the Optics repository.

---

## 1. Developer Workflows

### Code Formatting
Format all C sources in-place using standard `clang-format`:
```bash
./tools/format.sh          # format in place
./tools/format.sh --check  # check without modifying
```

### Running Tests & Quality Verification
Optics uses the native Meson test runner. Quality checks are integrated directly as test suites:
```bash
meson test -C build --suite unit          # CPU-only unit layer
meson test -C build --suite integration   # Vulkan/font/environment seams
meson test -C build --suite e2e           # public-API end-to-end layer
meson test -C build --suite lint          # tree-level checks, including test layering
```

### Tree-Level Gates
These run in the Meson `lint` suite and can also be invoked directly:

```bash
python3 tools/check_versions.py        # Meson/C headers/Cargo lockstep
python3 tools/check_library_purity.py  # no stdout in libs, no materials in gfx
python3 tools/check_doc_links.py       # relative links + public/dev firewall
python3 tools/check_c23.py             # no NULL/{0}, typed enums
python3 tools/check_api.py             # public API parameter / suffix rules
python3 tools/check_naming.py          # ADR-0170 names, plus live content: no codenames,
                                       # no stale optics_* names, no dead repo paths
python3 tools/check_adr.py             # ADR frontmatter schema + index agreement
python3 tools/check_leaks.py           # leak lane per public kind, raw alloc, attribution
python3 tools/gen_symbols.py --check   # docs/reference/symbols.md matches the headers
python3 tools/check_test_layers.py build  # test-pyramid taxonomy (needs a build dir)
```

Regenerate the symbol reference after changing a public declaration:
```bash
python3 tools/gen_symbols.py
```

### Wayland Listener Slot Contract (ADR-0145)
Run the hermetic Wayland listener verification directly:
```bash
python3 tools/check_listeners.py
```

---

## 2. Tooling Directory Structure

```text
tools/
├── format.sh               # clang-format over libs/, examples/, tests/, tools/ C sources
├── check_listeners.py      # Wayland listener-slot safety check (ADR-0145)
├── check_test_layers.py    # Test-pyramid taxonomy guardrail (ADR-0162)
├── check_versions.py       # Meson/C/Cargo version lockstep
├── check_library_purity.py # stdout + material-boundary checks
├── check_doc_links.py      # relative links + public/dev firewall
├── check_c23.py            # C23 baseline (NULL/{0}/untyped enums)
├── check_api.py            # public API parameter and suffix ratchet
├── check_naming.py         # semantic naming + live-content drift (ADR-0170, ADR-0173)
├── check_adr.py            # ADR registry consistency
├── check_leaks.py          # leak-coverage guardrail (ADR-0130)
├── gen_symbols.py          # generates docs/reference/symbols.md
├── README.md               # Tooling documentation
│
├── ci/                     # CI helpers
│   └── zig-win32-check.sh  # Win32 cross-compilation smoke runner via Zig
├── probes/                 # Frame-pacing probes, built with -Dexamples=true
│
└── config/                 # Toolchain configuration & suppression lists
    ├── api_baseline.txt    # check_api.py ratchet (empty)
    ├── leak-lanes.toml     # leak lane per public resource kind (ADR-0130)
    ├── leak-exempt.toml    # reasoned exemptions from a leak lane
    ├── lsan.supp           # LeakSanitizer suppression list
    └── wayland-listeners.toml # Pinned Wayland event contract (ADR-0145)
```
