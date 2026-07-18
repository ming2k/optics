# Setup

Use this guide to prepare a checkout for changes across the Optics monorepo.
The [root README](../../README.md) contains the shorter build-only path.

## Prerequisites

| Tool | Requirement | Why |
|------|-------------|-----|
| Meson | `>= 1.0` | Required by the root `meson.build`. |
| Ninja | Any Meson-supported version | Default build backend. |
| C compiler | GCC `>= 15` or Clang `>= 19` | The C23 sources use `#embed`. |
| `pkg-config` | Any current version | Finds system libraries. |
| `glslangValidator` | Vulkan 1.3 SPIR-V support | Compiles bundled shaders. |

The default feature set requires Vulkan headers and loader plus FreeType,
HarfBuzz, Fontconfig, and FriBidi. GLFW is needed for the windowed `flux`
examples. On Linux, the `iris` Wayland backend additionally uses
`wayland-client`, Wayland protocols, and XKB Common. `libsystemd` is optional;
without it, live theme watching and AT-SPI use their stub implementations.

On Debian or Ubuntu, install the development packages with:

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config glslang-tools vulkan-tools \
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

The root build links `flux`, `lens`, and `iris` directly. Do not install one
library before building the next, and do not configure nested Meson
subprojects.

Useful feature options are defined in `meson_options.txt`:

| Option | Default | Effect |
|--------|---------|--------|
| `canvas` | `true` | Build the 2D canvas and CPU backend. |
| `scene` | `true` | Build 3D mesh and material primitives. |
| `compute` | `true` | Build compute pipelines. |
| `effect` | `true` | Build image effects; requires canvas and compute. |
| `text` | `true` | Build the `flux-text` sibling. |
| `scene-graph` | `true` | Build the `flux-scene-graph` sibling. |
| `examples` | `false` | Build all available C examples. |
| `tests` | `false` | Build C tests and benchmarks. |

After changing options or installing a missing dependency, reconfigure:

```bash
meson setup --reconfigure build -Dexamples=true -Dtests=true
```

## Build

```bash
meson compile -C build
meson test -C build --no-suite bench
```

Run representative binaries from their module directories:

```bash
./build/examples/flux/scene_cube
./build/examples/lens/headless_demo
./build/examples/iris/widgets
```

The `iris` examples need a Wayland session. The `lens` examples are headless.
Most `flux` integration tests can use either a physical Vulkan device or
Lavapipe.

## Editor Setup

Meson writes `build/compile_commands.json`. Point `clangd` at the build tree,
or link the database into the repository root if that path is unused:

```bash
ln -s build/compile_commands.json compile_commands.json
```

The root `.clang-format` defines the C formatting style.

## Troubleshooting

| Symptom | First check |
|---------|-------------|
| Compiler rejects `#embed` | Use GCC 15+ or Clang 19+. |
| `glslangValidator` not found | Install `glslang-tools` and reconfigure. |
| FreeType, HarfBuzz, Fontconfig, or FriBidi missing | Install the text development packages, or configure with `-Dtext=false`. |
| GLFW warning and missing windowed `flux` examples | Install GLFW development files and reconfigure. |
| `iris` reports that its backend was skipped | Install Wayland, XKB Common, and Vulkan development files, then reconfigure. |
| Vulkan integration tests skip | Confirm `vulkaninfo --summary` finds a Vulkan 1.3 device or install Lavapipe. |

## See Also

- [Testing](testing.md)
- [Project layout](project-layout.md)
- [Getting started](../tutorials/01-getting-started.md)
