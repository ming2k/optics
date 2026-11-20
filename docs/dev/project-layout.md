# Project Layout

## Source Tree

```text
.
├── Cargo.toml          Repository-root Cargo workspace (ADR-0163, ADR-0169)
├── Cargo.lock          Single Rust dependency graph lockfile
├── crates/             Rust crates (`optics`, `optics-*`, `optics-*-sys`, `optics-build`) (ADR-0169)
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
| `flux` | `libs/gfx/include/flux/` | `libs/gfx/src/` | `tests/gfx/` | `examples/getting_started/` |
| `glyph` | `libs/text/include/glyph/` | `libs/text/src/` | `tests/text/` | `examples/showcase/` |
| `vista` | `libs/scene/include/vista/` | `libs/scene/src/` | `tests/scene/` | `examples/showcase/` |
| `prism` | `libs/material/include/prism/` | `libs/material/src/` | `tests/material/` | `examples/showcase/` |
| `transit` | `libs/motion/include/transit/` | Header-only (`export.h`) | `tests/motion/` | `examples/showcase/` |
| `reticle` | `libs/svg/include/reticle/` | `libs/svg/src/` | `tests/svg/` | `examples/getting_started/` |
| `lens` | `libs/ui/include/lens/` | `libs/ui/src/` | `tests/ui/` | `examples/showcase/` |
| `iris` | `libs/window/include/iris/` | `libs/window/src/` | `tests/window/` | `examples/getting_started/` |
| `optics` | `include/optics/optics.h` | Umbrella facade (`optics_dep`, ADR-0160/0164) | `tests/optics/` | — |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Crates

The repository-root `Cargo.toml` is the single Cargo workspace. `crates/`
stands symmetric with `libs/` and holds all `optics-*-sys` crates, safe
`optics-*` wrapper crates, the shared `optics-build` helper, and the canonical
`optics` facade crate with layered feature flags (ADR-0159, ADR-0163, ADR-0169).
Cargo commands run from the repository root. See
[Rust bindings](../explanation/rust-bindings.md) for the crate map and
development commands.

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
- Add FFI declarations and safe wrappers to the corresponding `optics-*` crate
  under `crates/` in the same change as a public C API update. Cargo workspace
  membership lives in the repository-root `Cargo.toml`.
- Route documentation through the [documentation index](../index.md). Keep
  contributor procedures in `docs/dev/` and user tasks outside it.
- Record durable architecture choices in a new numbered ADR; do not rewrite an
  accepted decision.

## Design References

- [Application architecture](../explanation/application-architecture.md)
- [ADR-0016: pure RHI and draw primitives](../adr/0016-pure-rhi-and-draw-primitives.md)
- [ADR-0023: unified monorepo build](../adr/0023-unified-monorepo-build.md)
