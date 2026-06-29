# Optics Development Setup

This guide covers how to set up your environment to develop the Optics stack (`flux`, `lens`, and `iris`) from this unified monorepo.

For a quick build overview, refer to the [root README](../../README.md).

## Prerequisites

Optics requires a C23-capable compiler and modern build tools. Since the stack spans from low-level GPU rendering to display server integration, several system dependencies are required.

### Build Tools

| Tool | Version | Notes |
|------|---------|-------|
| Meson | `>= 1.8` | Required for stable Wayland module support. |
| Ninja | any | Meson's default backend. |
| C compiler | GCC `>= 15` or Clang `>= 19` | C23 (`c2x`) support is strictly required. |
| `pkg-config` | any | Used to locate system libraries. |

### System Libraries (Linux / Wayland)

You will need Vulkan for the `flux` rendering engine and Wayland development libraries for `iris`.

*   **Vulkan**: `libvulkan-dev` (loader and headers)
*   **Wayland**: `libwayland-dev`, `wayland-protocols`
*   **Input / XKB**: `libxkbcommon-dev`
*   **Optional Integration**: `libsystemd-dev` (for theme watching and AT-SPI bridge)

**Debian / Ubuntu one-liner (matches CI):**
```sh
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config \
  libvulkan-dev libwayland-dev wayland-protocols \
  libxkbcommon-dev libsystemd-dev
```

## Building the Monorepo

The Optics monorepo is built using Meson. Building at the root level will compile all libraries (`flux`, `lens`, `iris`) and link them together automatically.

1. **Configure the build directory:**
   ```bash
   meson setup build -Dexamples=true
   ```
   *Tip: Use `--buildtype=debug` for development or `--buildtype=release` for profiling.*

2. **Compile the stack:**
   ```bash
   meson compile -C build
   ```

## Editor Configuration

### clangd (LSP)

If you use an editor with LSP support (VSCode, Neovim, Emacs), `clangd` is highly recommended. Meson generates a `compile_commands.json` file in the build directory.

Symlink it to the project root so `clangd` can provide accurate completions and diagnostics:
```bash
ln -s build/compile_commands.json .
```

### Formatting

The project includes a `.clang-format` configuration. It is highly recommended to configure your editor to run `clang-format` on save. 

You can also format the codebase manually if a formatting target is configured, or use integration scripts.

## Running Tests and Examples

After building, you can verify your environment by running the test suite and interactive examples.

**Run the full test suite:**
```bash
meson test -C build
```

**Run the low-level rendering demo (`flux`):**
```bash
./build/examples/flux/scene_cube
```

**Run the high-level UI toolkit demo (`iris` + `lens`):**
```bash
./build/examples/iris/widgets
```
