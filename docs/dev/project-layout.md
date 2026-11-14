# Project Layout

## Source Tree

```text
.
├── Cargo.toml          Repository-root Cargo workspace (ADR-0163)
├── Cargo.lock          Single Rust dependency graph lockfile
├── bindings/           Rust binding crates under crates/ (ADR-0159, ADR-0163)
├── docs/               Tutorials, guides, reference, decisions, and dev docs
├── examples/           C examples per library, plus showcase/ effect demos
├── include/            Canonical umbrella C23 header <optics/optics.h> (ADR-0160)
├── libs/
│   ├── flux/           Vulkan RHI, 2D canvas, 3D draw core, compute, effects
│   ├── glyph/          Text shaping, layout metrics, glyph atlas
│   ├── vista/          3D scene graph, cameras, glTF loading
│   ├── prism/          Physical materials (Liquid Glass, Mica, Acrylic)
│   ├── transit/        Header-only spring physics and parametric easing (ADR-0158)
│   ├── reticle/        SVG document compiler (device-less, links flux only)
│   ├── lens/           Headless UI engine
│   └── iris/           Application host and desktop integration
├── tests/              Three-layer C test pyramid (unit/integration/e2e)
├── meson.build         Root build orchestration
└── meson_options.txt   Repository-wide feature switches
```

## Ownership Boundaries

| Area | Public surface | Implementation | Tests | Examples |
|------|----------------|----------------|-------|----------|
| `flux` | `libs/flux/include/flux/` | `libs/flux/src/` | `tests/flux/` | `examples/flux/` |
| `glyph` | `libs/glyph/include/glyph/` | `libs/glyph/src/` | `tests/glyph/` | `examples/glyph/` |
| `vista` | `libs/vista/include/vista/` | `libs/vista/src/` | `tests/vista/` | `examples/vista/` |
| `prism` | `libs/prism/include/prism/` | `libs/prism/src/` | `tests/prism/` | `examples/showcase/` |
| `transit` | `libs/transit/include/transit/` | Header-only (`export.h`) | `tests/transit/` | `examples/transit/` |
| `reticle` | `libs/reticle/include/reticle/` | `libs/reticle/src/` | `tests/reticle/` | `examples/reticle/` |
| `lens` | `libs/lens/include/lens/` | `libs/lens/src/` | `tests/lens/` | `examples/lens/` |
| `iris` | `libs/iris/include/iris/` | `libs/iris/src/` | `tests/iris/` | `examples/iris/` |
| `optics` | `include/optics/optics.h` | Umbrella facade (`optics_dep`) | `tests/optics/` | — |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Bindings

The repository-root `Cargo.toml` is the single Cargo workspace. `bindings/crates/`
holds all `-sys` crates, safe abstraction crates, and the `optics` facade crate
with layered feature flags (ADR-0159, ADR-0163). Cargo commands run from the
repository root. See [Rust bindings](../explanation/rust-bindings.md) for the
crate map and development commands.

## Where New Files Go

- Add public declarations under the owning library's `include/` tree and
  implementation under its `src/` tree.
- Add C regression coverage under the layer that matches the boundary being
  tested: `tests/<library>/unit/` for CPU-only library contracts,
  `tests/<library>/integration/` for Vulkan/font/environment seams, and
  `tests/e2e/` for public-API scenarios. Every test must declare exactly one
  of `unit`, `integration`, or `e2e` (or a `bench`/`fuzz` lane); see
  [Testing](testing.md) and [ADR-0162](../adr/0162-layered-test-architecture-one-harness-three-layers-public-api-e2e.md).
- Add runnable demonstrations under `examples/<library>/`.
- Add FFI declarations and safe wrappers to the corresponding crate under
  `bindings/crates/` in the same change as a public C API update. Cargo workspace
  membership lives in the repository-root `Cargo.toml`.
- Route documentation through the [documentation index](../index.md). Keep
  contributor procedures in `docs/dev/` and user tasks outside it.
- Record durable architecture choices in a new numbered ADR; do not rewrite an
  accepted decision.

## Design References

- [Application architecture](../explanation/application-architecture.md)
- [ADR-0016: pure RHI and draw primitives](../adr/0016-pure-rhi-and-draw-primitives.md)
- [ADR-0023: unified monorepo build](../adr/0023-unified-monorepo-build.md)
