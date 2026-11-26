# Getting Started

You will build the Optics monorepo, run its correctness tests, and launch the
smallest windowed application. Allow about five minutes after the dependencies
are installed.

## Step 1 — Prepare the Environment

You need a C23 compiler with `#embed` support, Meson, Ninja, `slangc` (from a
recent Vulkan SDK or the pinned Slang release), and the development libraries
used by the default feature set. On Debian or Ubuntu, install the system
packages with:

```bash
sudo apt-get install -y --no-install-recommends \
  build-essential meson ninja-build pkg-config vulkan-tools \
  libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers \
  libfreetype-dev libharfbuzz-dev libfontconfig1-dev libfribidi-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev
```

Ubuntu's archive does not ship `slangc`; install the official release from the
Slang project (or a recent Vulkan SDK) and ensure it is on `PATH`.

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

The one root build compiles `gfx`, `ui`, `window`, the enabled sibling
libraries, and their examples and tests. No intermediate install is needed.

## Step 3 — Run the Tests

```bash
meson test -C build --no-suite bench --no-suite fuzz
```

The CPU suites run everywhere. GPU integration cases use the available Vulkan
device and skip cleanly when a required capability is absent. Full failure
output is stored in `build/meson-logs/testlog.txt`; pass `-v` to
`meson test` for live verbose output.

## Step 4 — Run the Smallest Windowed Application

```bash
./build/examples/getting_started/hello_app
```

A window opens with a button, a click counter, and a dark/light theme toggle.
This is the smallest complete application: `optics_app` runs an `optics_ui`
frame loop inside a native `optics_window`. Press `Esc` or close the window to
exit.

If the binary was not built, confirm that the UI stack is enabled (`text` and
`svg` are both `true`) and that the Wayland and Vulkan development packages are
installed, then reconfigure the build. If no display is available, run the
headless compute example instead:

```bash
./build/examples/getting_started/compute_pass
```

## Step 5 — Explore 2D and UI

```bash
./build/examples/getting_started/canvas_window
./build/examples/showcase/widget_gallery
```

`canvas_window` draws animated 2D vector geometry through `window` + `gfx`
without a UI engine, and `widget_gallery` exercises the multi-page UI shell.

## What's Next

| Goal | Read |
|------|------|
| Build a 2D app | [Your first 2D canvas application](02-first-2d-app.md) |
| Explore the examples | [Interactive examples showcase](../how-to/run-and-explore-examples.md) |
| Understand the stack | [Application architecture](../explanation/application-architecture.md) |
| Look up contracts | [API reference](../reference/api.md), [thread safety](../reference/thread-safety.md), and [glossary](../reference/glossary.md) |
