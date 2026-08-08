# Optics 

Optics is a unified C23 graphics and UI stack. It is structured as a monolithic repository (Monorepo) to ensure atomic commits and a seamless developer experience.

## Architecture

The stack consists of three closely integrated libraries located in `libs/`:

1. **flux**: The foundational Vulkan rendering engine. Provides core 2D/3D drawing primitives (canvas, scene-graph) and compute pipelines. The 2D `canvas` additionally ships a headless **software (CPU) backend** (`<flux/canvas_cpu.h>`) for GPU-free rendering — see [ADR-0019](docs/adr/0019-canvas-backend-seam-and-cpu-backend.md).
2. **lens**: An immediate-mode UI engine built on top of `flux`. It is headless by design and purely focuses on layout, state (retained trees), and emitting draw calls.
3. **iris**: The application L3 toolkit. It handles OS integration, window management, event loops, and feeds OS inputs into `lens`. Backends: Wayland (Linux), Win32 (Windows), Cocoa (macOS).

Rust bindings for these libraries are located in `bindings/`.

## Platforms

Linux/Wayland, Windows (Win32 + Vulkan), and macOS (Cocoa + MoltenVK >= 1.3)
share one source tree; the backend is chosen at configure time. See
[docs/dev/cross-platform.md](docs/dev/cross-platform.md) for requirements,
verification tooling, and consistency invariants.

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

Effect showcases — demos where a visual or mathematical effect is the point
(liquid glass, particle fields, a GPU-computed Julia set) — live in
`examples/showcase/`:
```bash
./build/examples/showcase/liquid_glass
```
