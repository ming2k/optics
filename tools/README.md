# Optics Developer Tooling

The `tools/` directory keeps only essential developer scripts, CI helpers, and toolchain configurations.

---

## 1. Developer Workflows

### Code Formatting
Format all C sources in-place using `clang-format`:
```bash
./tools/format.sh          # format in place
./tools/format.sh --check  # check without modifying
```

### Running Tests & Lint Checks
```bash
meson test -C build --suite unit          # CPU-only unit layer
meson test -C build --suite integration   # Vulkan / font / cross-library seams
meson test -C build --suite e2e           # public-API end-to-end scenarios
meson test -C build --suite lint          # essential tree-level checks
```

### Tree-Level Checks
Registered in the Meson `lint` suite and runnable directly:

```bash
python3 tools/check_versions.py        # Meson / C headers / Cargo version lockstep
python3 tools/check_listeners.py       # Wayland listener-slot safety (ADR-0145)
python3 tools/check_library_purity.py  # no stdout in libs, no named materials in gfx
python3 tools/gen_symbols.py --check   # docs/reference/symbols.md matches public headers
```

Regenerate `docs/reference/symbols.md` after changing a public declaration:
```bash
python3 tools/gen_symbols.py
```

Regenerate the pinned Wayland listener contract after upgrading `wayland-protocols`:
```bash
python3 tools/check_listeners.py --fix
```

---

## 2. Directory Structure

```text
tools/
├── format.sh                  # clang-format over C sources
├── check_versions.py          # Meson / C / Cargo version lockstep
├── check_listeners.py         # Wayland listener-slot safety check (ADR-0145)
├── check_library_purity.py    # stdout purity + gfx material boundary check
├── gen_symbols.py             # generates docs/reference/symbols.md
├── README.md                  # Tooling documentation
│
├── ci/                        # CI helpers
│   └── zig-win32-check.sh     # Win32 cross-compilation smoke check via Zig
├── probes/                    # Manual frame-pacing probes (built with -Dexamples=true)
│
└── config/                    # Toolchain configuration
    ├── lsan.supp              # LeakSanitizer suppression list (fontconfig cache)
    └── wayland-listeners.toml # Pinned Wayland event contract (ADR-0145)
```
