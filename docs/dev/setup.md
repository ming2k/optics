# Setup

Use this guide to prepare a checkout for changes across the Optics monorepo.
The [root README](../../README.md) contains the shorter build-only path.

## Prerequisites

| Tool | Requirement | Why |
|------|-------------|-----|
| Meson | `>= 1.0` | Required by the root `meson.build`. |
| Ninja | Any Meson-supported version | Default build backend. |
| C compiler | GCC `>= 15` or Clang `>= 19` | Shaders are baked into the libraries with C23 `#embed`; configure fails with a clear message on a compiler without it. MSVC `cl` cannot build the tree. |
| `pkg-config` | Any current version | Finds system libraries. |
| `slangc` | Slang 2026.17+ (CI pins 2026.21; recent Vulkan SDKs bundle it) | Compiles bundled shaders to SPIR-V ([ADR-0156](../adr/0156-slang-first-shader-architecture.md)). |

**Linux**: the default feature set requires Vulkan headers and loader plus FreeType,
HarfBuzz, Fontconfig, and FriBidi. `slangc` is provided by recent LunarG Vulkan
SDKs or the standalone Slang release. The `window` Wayland backend additionally
uses `wayland-client`, Wayland protocols, and XKB Common. `libsystemd` is
optional; without it, live theme watching and AT-SPI use their stub
implementations.

**Windows**: the Vulkan SDK 1.4+ (headers, loader, slangc) plus MinGW-w64
GCC 15+ (CI uses MSYS2 UCRT64) or Clang 19+. FreeType/HarfBuzz/FriBidi resolve automatically through the
bundled `subprojects/*.wrap` fallbacks; font discovery uses DirectWrite, so
fontconfig is not needed. When forcing the fallbacks, disable two optional
dependency cycles: `-Dfreetype2:harfbuzz=disabled -Dharfbuzz:cairo=disabled`.
The window backend is Win32 (`app_win32.c`).

**macOS**: MoltenVK >= 1.3 (older releases advertise only Vulkan 1.2 and are
rejected), a Vulkan loader (`vulkan-loader` via Homebrew), the pinned Slang
release from the official tarball (Homebrew has no `slang` formula), and the
same three text libraries (wraps also work). Font discovery uses CoreText.
The window backend is Cocoa (`app_cocoa.m`); Objective-C is enabled
automatically by `libs/window/meson.build`. See
[Cross-platform](cross-platform.md) for the full matrix and invariants.

On Debian or Ubuntu, install the development packages with:

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config vulkan-tools \
  libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers \
  libfreetype-dev libharfbuzz-dev libfontconfig1-dev libfribidi-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libsystemd-dev
```

If the distribution compiler is older than the requirement above, install a
newer GCC or Clang separately and select it with `CC` on the first
`meson setup` invocation.

Runtime file dialogs use an xdg-desktop-portal implementation. It is not a
build dependency.

## Configure

From the repository root, create one build tree for all C libraries, tests,
and examples:

```bash
meson setup build -Dexamples=true -Dtests=true
```

The default root build links `gfx`, `window`, `ui`, and `app` directly; a reduced
feature build may omit the UI stack. Do not install one library before building
the next, and do not configure nested Meson subprojects.

Useful feature options are defined in `meson_options.txt`:

| Option | Default | Effect |
|--------|---------|--------|
| `text` | `true` | Build the `text` (`liboptics_text`) shaping library. |
| `motion` | `true` | Install the header-only `<optics/motion.h>` motion vocabulary. |
| `examples` | `false` | Build all available C examples. |
| `tests` | `false` | Build C tests and benchmarks. |
| `fuzz` | `false` | Build fuzz harnesses (libFuzzer when available). |

`gfx` itself has no module switches: every `liboptics_gfx` carries the canvas,
compute and effect modules, so its ABI is identified by its version alone. The
options above only select which sibling libraries are built. The `ui`/`app`
stack is built only when `text` is enabled. A `gfx`+`window`-only build is
`-Dtext=false -Dmotion=false`.

After changing options or installing a missing dependency, reconfigure:

```bash
meson setup --reconfigure build -Dexamples=true -Dtests=true
```

## Build

```bash
meson compile -C build
meson test -C build --no-suite bench --no-suite fuzz
```

Run representative binaries from their module directories:

```bash
./build/examples/showcase/widget_gallery
./build/examples/getting_started/headless_canvas /tmp/out.ppm
```

The interactive examples need a Wayland session; the headless starters need no
display. Most `gfx` integration tests can use either a physical Vulkan device or
Lavapipe. For a complete catalog of visual effect demos and UI examples,
see [Interactive examples showcase](../how-to/run-and-explore-examples.md).

## Build Directory Conventions & Hygiene

Meson enforces out-of-tree builds. Adhere to these conventions to keep the repository root orderly:

- `build/`: The **canonical local development directory** (Debug + tests + examples). Rust crates under `crates/` probe this directory automatically via `optics-build`; the Cargo workspace manifest is at the repository root.
- **Ephemeral matrix builds (`build-fuzz*`, `build-release*`, `build-api*`, etc.)**: Temporary directories created when reproducing CI matrices locally. Treat them as disposable and clean them up after testing:
  ```bash
  rm -rf build-*
  ```

## Editor Setup

Meson writes `build/compile_commands.json`. Point `clangd` at the build tree,
or link the database into the repository root if that path is unused:

```bash
ln -s build/compile_commands.json compile_commands.json
```

The root `.clang-format` defines the C formatting style, enforced by
`./tools/format.sh --check` (CI runs it on every push) and
`./tools/format.sh` reformats the tree in place. Set `CLANG_FORMAT` to pick a
specific clang-format binary.

## Troubleshooting

| Symptom | First check |
|---------|-------------|
| Compiler rejects `#embed` | Nothing to do — the generated-header fallback builds automatically. To force it (or verify it), add `-DFLUX_SHADER_NO_EMBED=1` to `c_args`. |
| `slangc` not found | Install Vulkan SDK 1.4+ or the pinned Slang release from the official tarball, then ensure `slangc` is in PATH. |
| FreeType, HarfBuzz, Fontconfig, or FriBidi missing | Install the text development packages for a UI build. A deliberately gfx-only build passes `-Dtext=false`, which also disables `ui` and `app`; `window` needs only `gfx`. |
| `window` reports that its backend was skipped | Install Wayland, XKB Common, and Vulkan development files, then reconfigure. |
| Vulkan integration tests skip | Confirm `vulkaninfo --summary` finds a Vulkan 1.3 device or install Lavapipe. |

## See Also

- [Testing](testing.md)
- [Project layout](project-layout.md)
- [Getting started](../tutorials/01-getting-started.md)
