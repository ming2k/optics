# Project Layout

## Source Tree

```text
.
├── bindings/           Unified Cargo workspace with facade crate (ADR-0159)
├── docs/               Tutorials, guides, reference, decisions, and dev docs
├── examples/           C examples per library, plus showcase/ effect demos
├── include/            Canonical umbrella C23 header <optics/optics.h> (ADR-0160)
├── libs/
│   ├── flux/           Vulkan RHI, 2D canvas, 3D draw core, compute, effects
│   ├── glyph/          Text shaping, layout metrics, glyph atlas
│   ├── vista/          3D scene graph, cameras, glTF loading
│   ├── prism/          Physical materials (Liquid Glass, Mica, Acrylic)
│   ├── transit/        Header-only spring physics and parametric easing (ADR-0158)
│   ├── svg/            SVG document compiler (device-less, links flux only)
│   ├── lens/           Headless UI engine
│   └── iris/           Application host and desktop integration
├── tests/              C tests grouped by library and engine facade
├── meson.build         Root build orchestration
└── meson_options.txt   Repository-wide feature switches
```

## Ownership Boundaries

| Area | Public surface | Implementation | Tests | Examples |
|------|----------------|----------------|-------|----------|
| `flux` | `libs/flux/include/flux/` | `libs/flux/src/` | `tests/flux/` | `examples/flux/` |
| `glyph` | `libs/glyph/include/glyph/` | `libs/glyph/src/` | `tests/flux/` (shared text suites) | `examples/glyph/` |
| `vista` | `libs/vista/include/vista/` | `libs/vista/src/` | `tests/vista/` | `examples/vista/` |
| `prism` | `libs/prism/include/prism/` | `libs/prism/src/` | `tests/prism/` | `examples/showcase/` |
| `transit` | `libs/transit/include/transit/` | Header-only (`export.h`) | `tests/transit/` | `examples/transit/` |
| `reticle` | `libs/reticle/include/reticle/` | `libs/reticle/src/` | `tests/reticle/` | — |
| `lens` | `libs/lens/include/lens/` | `libs/lens/src/` | `tests/lens/` | `examples/lens/` |
| `iris` | `libs/iris/include/iris/` | `libs/iris/src/` | `tests/iris/` | `examples/iris/` |
| `optics` | `include/optics/optics.h` | Umbrella facade (`optics_dep`) | `tests/optics/` | — |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Bindings

`bindings/` holds a unified Cargo workspace containing all `-sys` crates, safe
abstraction crates, and the root `optics` facade crate with layered feature flags
(ADR-0159). See [Rust bindings](../explanation/rust-bindings.md) for the crate map
and development commands.

## Where New Files Go

- Add public declarations under the owning library's `include/` tree and
  implementation under its `src/` tree.
- Add C regression coverage under `tests/<library>/`; `glyph` is
  exercised from the `tests/flux/` suites it was split out of, while `vista`
  has dedicated suites under `tests/vista/`. Use `unit/`
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
