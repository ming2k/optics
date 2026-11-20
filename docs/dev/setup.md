# Setup

Use this guide to prepare a checkout for changes across the Optics monorepo.
The [root README](../../README.md) contains the shorter build-only path.

## Prerequisites

| Tool | Requirement | Why |
|------|-------------|-----|
| Meson | `>= 1.0` | Required by the root `meson.build`. |
| Ninja | Any Meson-supported version | Default build backend. |
| C compiler | GCC `>= 15` or Clang `>= 19`; MSVC cl / Apple Clang also work | The C23 sources prefer `#embed`; toolchains without it automatically use generated shader headers (`tools/spv2h.py`). |
| `pkg-config` | Any current version | Finds system libraries. |
| `slangc` | Slang 2026.17+ (CI pins 2026.21; recent Vulkan SDKs bundle it) | Compiles bundled shaders to SPIR-V ([ADR-0156](../adr/0156-slang-first-shader-architecture.md)). |

**Linux**: the default feature set requires Vulkan headers and loader plus FreeType,
HarfBuzz, Fontconfig, and FriBidi. `slangc` is provided by recent LunarG Vulkan
SDKs or the standalone Slang release. GLFW is needed for the windowed `flux`
examples. The `iris` Wayland backend additionally uses
`wayland-client`, Wayland protocols, and XKB Common. `libsystemd` is optional;
without it, live theme watching and AT-SPI use their stub implementations.

**Windows**: the Vulkan SDK 1.4+ (headers, loader, slangc) plus clang-cl,
MSVC, or MinGW. FreeType/HarfBuzz/FriBidi resolve automatically through the
bundled `subprojects/*.wrap` fallbacks; font discovery uses DirectWrite, so
fontconfig is not needed. When forcing the fallbacks, disable two optional
dependency cycles: `-Dfreetype2:harfbuzz=disabled -Dharfbuzz:cairo=disabled`.
The iris backend is Win32 (`app_win32.c`).

**macOS**: MoltenVK >= 1.3 (older releases advertise only Vulkan 1.2 and are
rejected), a Vulkan loader (`vulkan-loader` via Homebrew), the pinned Slang
release from the official tarball (Homebrew has no `slang` formula), and the
same three text libraries (wraps also work). Font discovery uses CoreText.
The iris backend is Cocoa (`app_cocoa.m`); Objective-C is enabled
automatically by `libs/window/meson.build`. See
[Cross-platform](cross-platform.md) for the full matrix and invariants.

On Debian or Ubuntu, install the development packages with:

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config vulkan-tools \
  libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers \
  libfreetype-dev libharfbuzz-dev libfontconfig1-dev libfribidi-dev \
  libglfw3-dev libwayland-dev wayland-protocols libxkbcommon-dev \
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
| `scene` | `true` | Build the `scene` (`liboptics_scene`) 3D scene-graph library. |
| `material` | `true` | Build the `material` (`liboptics_material`) surface-material library. |
| `motion` | `true` | Install the header-only `<optics/motion.h>` motion vocabulary. |
| `svg` | `true` | Build the `svg` (`liboptics_svg`) SVG document compiler. |
| `examples` | `false` | Build all available C examples. |
| `tests` | `false` | Build C tests and benchmarks. |
| `fuzz` | `false` | Build fuzz harnesses (libFuzzer when available). |

`gfx` itself has no module switches: every `liboptics_gfx` carries the canvas,
scene, compute and effect modules, so its ABI is identified by its version
alone. The options above only select which sibling libraries are built. The
`ui`/`app` stack is built only when `text` and `svg` are both enabled. A
`gfx`+`window`-only build is `-Dtext=false -Dsvg=false -Dscene=false
-Dmaterial=false -Dmotion=false`.

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
./build/examples/showcase/scene_studio
./build/examples/showcase/headless_demo
./build/examples/showcase/widget_gallery
```

The `iris` examples need a Wayland session. The `lens` examples are headless.
Most `flux` integration tests can use either a physical Vulkan device or
Lavapipe. For a complete catalog of visual effect demos and UI examples,
see [Interactive examples showcase](../how-to/run-and-explore-examples.md).

## Build Directory Conventions & Hygiene

Meson enforces out-of-tree builds. Adhere to these conventions to keep the repository root orderly:

- `build/`: The **canonical local development directory** (Debug + tests + examples). Rust crates under `crates/` probe this directory automatically via `optics-build`; the Cargo workspace manifest is at the repository root.
- `build-tidy/`: Dedicated build tree for `tools/check-tidy.sh` (created automatically on demand).
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
`tools/check-format.sh` (CI runs it on every push; it pins clang-format 22,
so a different local major version will refuse rather than produce noise).
`tools/check-format.sh --fix` reformats the tree in place.

## Troubleshooting

| Symptom | First check |
|---------|-------------|
| Compiler rejects `#embed` | Nothing to do — the generated-header fallback builds automatically. To force it (or verify it), add `-DFLUX_SHADER_NO_EMBED=1` to `c_args`. |
| `slangc` not found | Install Vulkan SDK 1.4+ or the pinned Slang release from the official tarball, then ensure `slangc` is in PATH. |
| FreeType, HarfBuzz, Fontconfig, or FriBidi missing | Install the text development packages for a UI build. A deliberately flux-only build disables `canvas`, `glyph`, and `reticle` as a group, which also disables `lens`/`iris`. |
| GLFW warning and missing windowed `flux` examples | Install GLFW development files and reconfigure. |
| `iris` reports that its backend was skipped | Install Wayland, XKB Common, and Vulkan development files, then reconfigure. |
| Vulkan integration tests skip | Confirm `vulkaninfo --summary` finds a Vulkan 1.3 device or install Lavapipe. |

## See Also

- [Testing](testing.md)
- [Project layout](project-layout.md)
- [Getting started](../tutorials/01-getting-started.md)
