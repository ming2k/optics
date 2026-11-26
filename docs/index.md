# Optics Documentation

Optics is a pure, high-performance C23 2D graphics and UI stack powered by Vulkan 1.3. The six core C libraries (`gfx`, `text`, `motion`, `window`, `ui`, `app`), interactive examples, tests, and Rust bindings live in this repository.

---

## Documentation Directory

Choose a section based on what you are trying to do:

- **[Tutorials](tutorials/index.md)** — Step-by-step learning lessons: bootstrap a development environment and build your first 2D canvas application.
- **[How-To Guides](how-to/index.md)** — Task-oriented problem-solving recipes: frame presentation, image blur effects, CPU headless rendering, and color management.
- **[Interactive Examples Showcase](how-to/run-and-explore-examples.md)** — Run and explore the widget gallery and minimal starters.
- **[Reference Specifications](reference/index.md)** — Authoritative lookup tables: C API contracts, public symbol catalog, effect operators, and thread safety.
- **[Explanation & Architecture](explanation/index.md)** — Deep conceptual foundations: Value-Semantic Rendering (VSR), analytic curve coverage, scene snapshots, and Vulkan backend design.
- **[Architecture Decision Records](adr/index.md)** — Ratified architectural ledger detailing structural contracts and negative knowledge.
- **[Developer & Contributor Documentation](dev/index.md)** — Build configuration, automated testing runbooks, manual verification suites, and release hygiene.
- **[Engineering Standards](dev/standards/README.md)** — Repository architecture topology, system invariants, C23 API guidelines, and PR verification gates.

The current source tree and root `meson.build` are authoritative when a historical ADR describes an older layout.
