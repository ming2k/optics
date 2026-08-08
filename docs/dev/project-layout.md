# Project Layout

## Source Tree

```text
.
├── bindings/           Rust workspaces for flux, lens, and iris
├── docs/               Tutorials, guides, reference, decisions, and dev docs
├── examples/           C examples per library, plus showcase/ effect demos
├── libs/
│   ├── flux/           Vulkan rendering, text, and scene-graph siblings
│   ├── lens/           Headless UI engine
│   └── iris/           Application host and desktop integration
├── tests/              C tests grouped by library
├── meson.build         Root build orchestration
└── meson_options.txt   Repository-wide feature switches
```

## Ownership Boundaries

| Area | Public surface | Implementation | Tests | Examples |
|------|----------------|----------------|-------|----------|
| `flux` | `libs/flux/include/flux/` | `libs/flux/src/` | `tests/flux/` | `examples/flux/` |
| `flux-text` | `libs/flux/text/include/` | `libs/flux/text/src/` | Flux unit/integration tests and Rust workspace | `examples/flux-text/` |
| `flux-scene-graph` | `libs/flux/scene_graph/include/` | `libs/flux/scene_graph/src/` | No dedicated C suite; Rust workspace build | `examples/flux-scene-graph/` |
| `lens` | `libs/lens/include/lens/` | `libs/lens/src/` | `tests/lens/` | `examples/lens/` |
| `iris` | `libs/iris/include/iris/` | `libs/iris/src/` | `tests/iris/` | `examples/iris/` |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Bindings

`bindings/flux-rs/`, `bindings/lens-rs/`, and `bindings/iris-rs/` are separate
Cargo workspaces inside the monorepo. Each contains a raw `*-sys` crate and a
safe wrapper; `flux-rs` also contains the text, layout, and scene-graph crates.
See [Rust bindings](../explanation/rust-bindings.md) for the crate map and
development commands.

## Where New Files Go

- Add public declarations under the owning library's `include/` tree and
  implementation under its `src/` tree.
- Add C regression coverage under `tests/<library>/`; use `unit/` versus
  `integration/` for `flux` based on whether Vulkan execution is required.
- Add runnable demonstrations under `examples/<library>/`.
- Add FFI declarations and safe wrappers to the corresponding workspace under
  `bindings/` in the same change as a public C API update.
- Route documentation through the [documentation index](../index.md). Keep
  contributor procedures in `docs/dev/` and user tasks outside it.
- Record durable architecture choices in a new numbered ADR; do not rewrite an
  accepted decision.

## Design References

- [Application architecture](../explanation/application-architecture.md)
- [ADR-0016: pure RHI and draw primitives](../adr/0016-pure-rhi-and-draw-primitives.md)
- [ADR-0023: unified monorepo build](../adr/0023-unified-monorepo-build.md)
