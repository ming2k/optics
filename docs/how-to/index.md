# How-To Guides

Task-oriented problem-solving recipes for common rendering, layout, and visual computing workflows in Optics. Each guide assumes you have built the repository and completed [Getting Started](../tutorials/index.md).

---

## Guides Directory

| Guide | Solves |
| :--- | :--- |
| **[Record and Present a Frame](record-and-present-a-frame.md)** | Execute the standard 5-call Vulkan frame lifecycle: acquire, begin, draw, end, present. |
| **[Run and Explore Examples](run-and-explore-examples.md)** | Build and interact with the complete showcase gallery: physical glass materials, particles, and UI apps. |
| **[Use Optics from a Rust Project](use-optics-from-rust.md)** | Add the `optics` crate to an external Cargo project: build the C libraries at the matching version, point the build at them, and run the result. |
| **[Choose a Static or Shared Link](choose-a-static-or-shared-link.md)** | Link the shared objects (the default) or embed the archives with the `static` feature: precedence, how the probe decides, and link-failure triage. |
| **[Apply a Blur Effect](apply-a-blur-effect.md)** | Run an exact separable Gaussian or dual-pass blur on a `optics_gfx_image` and manage transient targets. |
| **[Render on the CPU (No GPU)](render-on-cpu.md)** | Execute headless 2D canvas drawing on the CPU via `<optics/gfx/canvas_cpu.h>` without a Vulkan ICD. |
| **[Render Offscreen (No Window)](render-offscreen.md)** | Render vector or 3D scene content into an offscreen image target and read back pixels for tests. |
| **[Light a Mesh](light-a-mesh.md)** | Configure directional lighting and Blinn-Phong transient uniform blocks for 3D meshes. |
| **[Texture a Mesh](texture-a-mesh.md)** | Bind diffuse/normal textures and samplers to 3D materials via descriptor chains. |
| **[Manage Color](manage-color.md)** | Work with linear RGBA16F intermediate buffers, ICC color profiles, and calibrated presentation. |

For deep architectural rationale and physical models, consult the [Explanation Articles](../explanation/index.md).
For technical API and symbol signatures, consult the [Reference Specifications](../reference/index.md).
