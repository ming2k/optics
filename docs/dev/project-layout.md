# Project Layout

## Source Tree

```text
.
├── bindings/           Rust workspaces, one per library
├── docs/               Tutorials, guides, reference, decisions, and dev docs
├── examples/           C examples per library, plus showcase/ effect demos
├── libs/
│   ├── flux/           Vulkan RHI, 2D canvas, 3D draw core, compute, effects
│   ├── glyph/          Text shaping, layout metrics, glyph atlas
│   ├── vista/          3D scene graph, cameras, glTF loading
│   ├── prism/          Physical materials (Liquid Glass, Mica, Acrylic)
│   ├── transit/        Spring physics and parametric easing
│   ├── svg/            SVG document compiler (device-less, links flux only)
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
| `glyph` | `libs/glyph/include/glyph/` | `libs/glyph/src/` | `tests/flux/` (shared text suites) | `examples/glyph/` |
| `vista` | `libs/vista/include/vista/` | `libs/vista/src/` | `tests/flux/` (shared scene suites) | `examples/vista/` |
| `prism` | `libs/prism/include/prism/` | `libs/prism/src/` | `tests/prism/` | `examples/showcase/` |
| `transit` | `libs/transit/include/transit/` | `libs/transit/src/` | `tests/transit/` | `examples/transit/` |
| `svg` | `libs/svg/include/svg/` | `libs/svg/src/` | `tests/svg/` | — |
| `lens` | `libs/lens/include/lens/` | `libs/lens/src/` | `tests/lens/` | `examples/lens/` |
| `iris` | `libs/iris/include/iris/` | `libs/iris/src/` | `tests/iris/` | `examples/iris/` |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Bindings

`bindings/*-rs/` holds one Cargo workspace per library (`flux-rs`, `glyph-rs`,
`iris-rs`, `lens-rs`, `prism-rs`, `svg-rs`, `transit-rs`, `vista-rs`). Each
contains a raw `*-sys` crate and a safe wrapper crate.
See [Rust bindings](../explanation/rust-bindings.md) for the crate map and
development commands.

## Where New Files Go

- Add public declarations under the owning library's `include/` tree and
  implementation under its `src/` tree.
- Add C regression coverage under `tests/<library>/`; `glyph` and `vista` are
  exercised from the `tests/flux/` suites they were split out of. Use `unit/`
  versus `integration/` for `flux` based on whether Vulkan execution is
  required.
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
