# Getting started

You will build flux from source, run the simplest example, and confirm
your environment supports it. Total time: about five minutes.

## What you need before you start

- A Linux box with a Vulkan 1.3 driver. macOS and Windows are possible
  but not covered here.
- Root or sudo for one `apt`/`pacman`/`dnf` command.
- About 100 MB of disk space for the build directory.

## Step 1 — install the dependencies

On Debian/Ubuntu:

    sudo apt install meson ninja-build gcc-15 libvulkan-dev \
                     vulkan-validationlayers glslang-tools libglfw3-dev

On Arch:

    sudo pacman -S meson ninja gcc vulkan-headers vulkan-validation-layers \
                   glslang glfw

> **Compiler note:** flux is a C23 library. It uses `#embed` to inline
> SPIR-V shaders at compile time. You need **GCC ≥ 15** or **Clang ≥ 19**.
> Older compilers will fail on the `#embed` directives in the examples.

Confirm Vulkan 1.3 is available:

    vulkaninfo --summary

You should see at least one `GPU` with `apiVersion >= 1.3.*`.

## Step 2 — get and build flux

    git clone <flux repo> flux
    cd flux
    meson setup build -Dexamples=true -Dtests=true
    meson compile -C build

You will see one library built and four example binaries linked.

## Step 3 — run the tests

    meson test -C build

You should see all tests `OK`. If any fail, the full per-test stderr
goes to `build/meson-logs/testlog.txt`. The most common cause is a
missing or pre-1.3 Vulkan driver — see
[dev/testing.md](../dev/testing.md) for the headless lavapipe setup
CI uses.

## Step 4 — run the simplest example

    ./build/examples/hello_triangle

A window opens and draws a coloured triangle. Close it.

You just consumed `flux-core` directly without any higher-level module:
that program calls `flux_surface_begin_frame`, `flux_frame_begin_pass`
(clear + draw), `flux_frame_end_pass`, `flux_frame_submit`,
`flux_frame_present`. No 2D shapes or 3D scene. This is the smallest possible
flux application.

## Step 5 — run the 2D, 3D, and compute examples

    ./build/examples/canvas_hello
    ./build/examples/scene_cube
    ./build/examples/compute_fill

The first shows tiled rectangles, gradients, paths, stroked lines, and
rotated shapes. The second shows a tumbling cube with depth testing. The
third runs headless, dispatches a compute shader, and prints the first and
last values of the filled buffer.

## What's next

| Want to...                              | Read                                               |
|-----------------------------------------|----------------------------------------------------|
| Build your own 2D app                   | [02 — Your first 2D canvas application](02-first-2d-app.md) |
| Build your own 3D app                   | [03 — Your first 3D scene application](03-first-3d-app.md)  |
| Understand the architecture             | [Application architecture](../explanation/application-architecture.md) |
| Look up types and contracts             | [API reference](../reference/api.md), [Thread safety](../reference/thread-safety.md), [Glossary](../reference/glossary.md). The headers themselves (`include/flux/*.h`) are the canonical reference until a generated doc page lands. |
