# Getting Started

You will build the Optics monorepo, run its correctness tests, and launch the
smallest `flux` example. Allow about five minutes after the dependencies are
installed.

## Step 1 — Prepare the Environment

You need a C23 compiler with `#embed` support, Meson, Ninja,
`glslangValidator`, and the development libraries used by the default feature
set. On Debian or Ubuntu, install them with:

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config glslang-tools vulkan-tools \
  libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers \
  libfreetype-dev libharfbuzz-dev libfontconfig1-dev libfribidi-dev \
  libglfw3-dev libwayland-dev wayland-protocols libxkbcommon-dev
```

If the distribution compiler is older than GCC 15 or Clang 19, install a
newer compiler and select it with `CC` when configuring Meson.

Confirm that a Vulkan 1.3 implementation is visible:

```bash
vulkaninfo --summary
```

At least one device should report `apiVersion` 1.3 or newer. A physical GPU is
not required; Lavapipe is sufficient for headless development.

## Step 2 — Get and Build Optics

```bash
git clone <optics-repository-url> optics
cd optics
meson setup build -Dexamples=true -Dtests=true
meson compile -C build
```

The one root build compiles `flux`, `lens`, `iris`, the enabled sibling
libraries, and their examples and tests. No intermediate install is needed.

## Step 3 — Run the Tests

```bash
meson test -C build --no-suite bench
```

The CPU suites run everywhere. GPU integration cases use the available Vulkan
device and skip cleanly when a required capability is absent. Full failure
output is stored in `build/meson-logs/testlog.txt`; pass `-v` to
`meson test` for live verbose output.

## Step 4 — Run the Smallest Windowed Example

```bash
./build/examples/flux/hello_triangle
```

A window opens with a coloured triangle. This program uses `flux-core`
directly: it creates a device and surface, records a dynamic-rendering pass,
submits the frame, and presents it. Close the window to exit.

If the binary was not built, install the GLFW development package and
reconfigure the build. If no display is available, run the headless compute
example instead:

```bash
./build/examples/flux/compute_fill
```

## Step 5 — Explore 2D, 3D, and UI

```bash
./build/examples/flux/canvas_hello
./build/examples/flux/scene_cube
./build/examples/lens/headless_demo
./build/examples/iris/widgets
```

The `flux` examples show direct rendering, the `lens` example exercises the
headless UI engine, and the `iris` example supplies the Wayland window and
event loop for a complete UI application.

## What's Next

| Goal | Read |
|------|------|
| Build a 2D app | [Your first 2D canvas application](02-first-2d-app.md) |
| Build a 3D app | [Your first 3D scene application](03-first-3d-app.md) |
| Understand the stack | [Application architecture](../explanation/application-architecture.md) |
| Look up contracts | [API reference](../reference/api.md), [thread safety](../reference/thread-safety.md), and [glossary](../reference/glossary.md) |
