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
python3 tools/check_library_purity.py  # no stdout in libs, no materials in flux
python3 tools/check_doc_links.py       # relative links + public/dev firewall
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
├── format.sh               # Fast in-place code formatting wrapper
├── check_listeners.py      # Wayland listener-slot safety check (ADR-0145)
├── check_test_layers.py    # Test-pyramid taxonomy guardrail (ADR-0162)
├── check_versions.py       # Meson/C/Cargo version lockstep
├── check_library_purity.py # stdout + material-boundary checks
├── check_doc_links.py      # relative links + public/dev firewall
├── README.md               # Tooling documentation
│
├── ci/                     # CI Runners & Platform Emulation
│   └── zig-win32-check.sh  # Win32 cross-compilation smoke runner via Zig
│
└── config/                 # Toolchain Configuration & Suppression Lists
    ├── lsan.supp           # LeakSanitizer suppression list
    └── wayland-listeners.toml # Pinned Wayland event contract (ADR-0145)
```
