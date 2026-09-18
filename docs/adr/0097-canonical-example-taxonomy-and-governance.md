# ADR-0097: Canonical example taxonomy, legacy pruning, and lifecycle governance

- Status: Accepted
- Date: 2026-09-15
- Scope: `examples/` across the entire Optics monorepo, Meson targets, and CI verification.
- Depends on: [ADR-0023](0023-unified-monorepo-build.md), [ADR-0077](0077-anim-motion-vocabulary-library.md), [ADR-0095](0095-flux-public-contract-convergence.md), [ADR-0096](0096-prism-mica-foundation-material.md).

## Context

In an infrastructure monorepo, example programs are not optional accessories or informal drafts;
they serve three critical architectural functions:
1. **Zero-privilege consumer verification:** Unlike unit tests (which often use mock harnesses, internal
   headers, or test-only seams), examples are the only code that strictly consumes the public, installed API.
   They prove that the public interface is sufficient and ergonomic for external developers.
2. **Living documentation:** Examples participate in the build system and compile against the current tree,
   ensuring that idiomatic usage patterns remain fresh and do not drift like static prose.
3. **Interactive & visual acceptance:** Motion damping, drag-and-drop response, text rendering fidelity,
   and optical material composites require running programs for human sensory validation.

Over time, `examples/` accumulated 20+ programs with several architectural problems:
- **Redundancy and overlap:** `iris/hello_app.c` duplicated `iris/minimal.c`; `iris/forms.c` duplicated
  widgets already exercised in `iris/widgets.c`; `showcase/liquid_glass.c` was made redundant by the
  unified `material_gallery.c`.
- **Framework bypass:** `flux/hello_triangle.c` contained ~800 lines of raw Vulkan swapchain and pipeline
  boilerplate, directly contradicting Flux's design goal of abstracting raw Vulkan plumbing.
- **Maintenance drag from isolated shader experiments:** Four standalone particle/math demos
  (`filament_plume`, `particles_terrain`, `ripple_field`, `julia_morph`) lacked UI relevance and repeatedly
  churned during public contract migrations (e.g. ADR-0089, ADR-0095), acting as unmaintainable debt.
- **Missing first-class coverage:** `libs/anim` (ADR-0077) lacked any standalone example demonstrating
  closed-form springs, motion-adaptive smoothers, or hysteresis latches.

## Decision

1. **Establish a strict 3-tier canonical taxonomy for `examples/`:**
   - **Tier 1: Canonical Starters (极简起手式):** Single-concept entry points (<80 lines, zero boilerplate)
     demonstrating how to initialize and drive each core library:
     - `examples/iris/hello_window.c` (Iris: OS window + event loop + minimal button/label)
     - `examples/flux/canvas_hello.c` (Flux: 2D drawing primitives, paths, gradients)
     - `examples/flux-text/text_hello.c` (Flux-Text: HarfBuzz shaping and measurement)
     - `examples/lens/headless_demo.c` (Lens: headless tree layout, zero-GPU Flexbox)
     - `examples/anim/motion_spring.c` (Anim: closed-form spring, adaptive smoother, hysteresis)
   - **Tier 2: Subsystem Seams & Capability Scenarios (专项能力与多文件场景):** Dedicated, focused programs proving complex subsystems and idiomatic application architectures:
     - `examples/scenarios/overlays/` (Multi-file scenario: modals, popups, dropdown menus, backdrop scrims)
     - `examples/iris/dnd_demo.c` (Drag-and-Drop cross-window exchange per ADR-0086)
     - `examples/iris/paint_static_demo.c` (Static damage tracking & zero-damage frame verification per ADR-0030)
     - `examples/iris/overlay_demo.c` (Modals, popups, and floating overlays)
     - `examples/iris/fonts.c` (Font fallback, emoji, and system font discovery)
     - `examples/flux/scene_cube.c` (3D scene graph, mesh, camera, unlit/phong shaders)
     - `examples/flux/compute_fill.c` (GPU compute pipeline and storage image dispatch)
     - `examples/flux/image_animation.c` (Time-driven sprite and image transform animation)
     - `examples/flux-scene-graph/gltf_viewer.c` (glTF 3D model loading and bone animation playback)
     - `examples/lens/a11y_tree_demo.c` (Accessibility tree generation and inspection)
     - `examples/lens/state_demo.c` (Retained node state and store GC)
   - **Tier 3: Flagship Integration Galleries (全栈大画廊):** Comprehensive visual and interactive acceptance:
     - `examples/iris/widgets.c` (Unified UI catalog: buttons, inputs, sliders, checks, tabs, form controls)
     - `examples/showcase/material_gallery/` (Comparative showcase of all 4 Prism materials: Liquid Glass, Frosted, Acrylic, Mica)

2. **Prune redundant and obsolete targets:**
   - Remove `examples/flux/hello_triangle.c` (and its shaders).
   - Consolidate `examples/iris/minimal.c` and `examples/iris/hello_app.c` into `examples/iris/hello_window.c`.
   - Consolidate `examples/iris/forms.c` into `examples/iris/widgets.c` as a dedicated form section.
   - Remove `examples/showcase/liquid_glass.c` and `examples/showcase/liquid_glass_study.c` (superseded by `material_gallery.c`).
   - Remove the four legacy shader toys (`filament_plume`, `particles_terrain`, `ripple_field`, `julia_morph`) and their SPIR-V build rules.

3. **Add `examples/anim/motion_spring.c`:**
   - Introduces a clean C23 console/visual starter for `libs/anim` demonstrating closed-form spring integration,
     motion-adaptive smoothing, and the hysteresis latch.

4. **Lifecycle Governance Rules:**
   - **Zero Privilege:** Examples must strictly consume public installed headers (`<flux/...>`, `<lens/...>`,
     `<iris/...>`, `<prism/...>`, `<anim/...>`). No `*_internal.h` or private headers permitted.
   - **CI Smoke Validation:** Every example must support a headless `--smoke` flag or compile-and-link CI gate
     to guarantee that examples never suffer from bit-rot or hidden link failures.

## Consequences

- The example suite is lean, cohesive, and directly aligned with Optics' product mission (UI, windowing, 2D/3D graphics, and modern materials).
- Build times for `examples` are reduced by eliminating redundant shader compilation passes.
- Downstream developers have clear, unambiguous "Hello World" starters for every layer in the stack.
