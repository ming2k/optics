# Optics Documentation

Optics is a C23 graphics and UI stack built as one repository. The C libraries
(`flux`, `lens`, and `iris`), their examples and tests, and the Rust binding
workspaces all live in this checkout.

Choose a section based on what you are trying to do:

- [Tutorials](tutorials/01-getting-started.md) — build the stack and learn the
  2D and 3D APIs.
- [How-to guides](how-to/record-and-present-a-frame.md) — complete a focused
  rendering task.
- [Explanation](explanation/application-architecture.md) — understand the
  stack boundaries, Vulkan backend, and Rust bindings.
- [Reference](reference/api.md) — look up API contracts, symbols, effects,
  the [composition graph](reference/composition-graph.md), threading rules,
  and terminology.
- [Architecture decisions](adr/index.md) — review accepted and superseded
  technical decisions.
- [Developer documentation](dev/index.md) — set up a checkout, run tests,
  navigate the tree, and prepare a release.
- [Contributing](contributing/index.md) — follow the monorepo change workflow
  and documentation rules.

The current source tree and root `meson.build` are authoritative when a
historical ADR describes an older layout.
