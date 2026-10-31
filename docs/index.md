# Optics Documentation

Optics is a modern, high-performance C23 graphics and UI stack powered by Vulkan 1.3. The C libraries (`flux`, `glyph`, `vista`, `prism`, `transit`, `lens`, `iris`), interactive examples, tests, and Rust bindings live in this repository.

---

## Documentation Directory

Choose a section based on what you are trying to do:

- **[Tutorials](tutorials/index.md)** — Step-by-step learning lessons: bootstrap a development environment and build your first 2D canvas and 3D scene applications.
- **[How-To Guides](how-to/index.md)** — Task-oriented problem-solving recipes: frame presentation, image blur effects, CPU headless rendering, mesh texturing, and color management.
- **[Interactive Examples Showcase](how-to/run-and-explore-examples.md)** — Run and explore the interactive material gallery, Liquid Glass refraction, UI widget demos, and particle dynamics.
- **[Reference Specifications](reference/index.md)** — Authoritative lookup tables: C API contracts, public symbol catalog, physical material parameters, effect operators, and thread safety.
- **[Explanation & Architecture](explanation/index.md)** — Deep conceptual foundations: Value-Semantic Rendering (VSR), Liquid Glass optics, analytic curve coverage, and Vulkan backend design.
- **[Architecture Decision Records](adr/index.md)** — Ratified architectural ledger (ADR-0001 through ADR-0125) detailing structural contracts and negative knowledge.
- **[Developer & Contributor Documentation](dev/index.md)** — Build configuration, automated testing runbooks, manual verification suites, and release hygiene.
- **[Engineering Standards](dev/standards/README.md)** — System invariants, C23 API guidelines, UI component purity, material boundaries, and PR verification gates.

The current source tree and root `meson.build` are authoritative when a historical ADR describes an older layout.
