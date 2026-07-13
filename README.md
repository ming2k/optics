# Optics 

Optics is a unified C23 graphics and UI stack. It is structured as a monolithic repository (Monorepo) to ensure atomic commits and a seamless developer experience.

## Architecture

The stack consists of three closely integrated libraries located in `libs/`:

1. **flux**: The foundational Vulkan rendering engine. Provides core 2D/3D drawing primitives (canvas, scene-graph) and compute pipelines. The 2D `canvas` additionally ships a headless **software (CPU) backend** (`<flux/canvas_cpu.h>`) for GPU-free rendering — see [ADR-0019](docs/adr/0019-canvas-backend-seam-and-cpu-backend.md).
2. **lens**: An immediate-mode UI engine built on top of `flux`. It is headless by design and purely focuses on layout, state (retained trees), and emitting draw calls.
3. **iris**: The application L3 toolkit. It handles Wayland/OS integration, window management, event loops, and feeds OS inputs into `lens`.

Rust bindings for these libraries are located in `bindings/`.

## Building

We use `meson` to build the entire stack at once.

```bash
meson setup build -Dexamples=true
meson compile -C build
```

To run the unified widget demo:
```bash
./build/examples/iris/widgets
```

To run the Vulkan rendering demo:
```bash
./build/examples/flux/scene_cube
```

To run the image-animation demo (bounce, squash/stretch, spin/pulse,
cross-fade, and sprite-sheet playback):
```bash
./build/examples/flux/image_animation
```
