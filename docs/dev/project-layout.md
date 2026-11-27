# Project Layout

## Source Tree

```text
.
├── Cargo.toml          Repository-root Cargo workspace (ADR-0163, ADR-0169)
├── Cargo.lock          Single Rust dependency graph lockfile
├── crates/             Rust crates (`optics-*`, `optics-*-sys`, `optics-build`) (ADR-0169, ADR-0176)
├── docs/               Tutorials, guides, reference, decisions, and dev docs
├── examples/           getting_started/ starters and showcase/ applications (ADR-0171)
├── include/            Canonical umbrella C23 header <optics/optics.h> (ADR-0160)
├── libs/
│   ├── gfx/            Vulkan RHI, 2D canvas, compute, effects
│   ├── text/           Text shaping, layout metrics, glyph atlas
│   ├── window/         Native window, event pump, frame cadence, platform services
│   ├── ui/             Headless UI engine (incl. header-only <optics/ui/motion.h>, ADR-0177)
│   └── app/            Application shell joining ui to window
├── tools/              Tree-level gates, CI helpers, developer probes
├── tests/              Three-layer C test pyramid (unit/integration/e2e)
├── meson.build         Root build orchestration
└── meson_options.txt   Repository-wide feature switches
```

## Ownership Boundaries

| Area | Public surface | Implementation | Tests | Examples |
|------|----------------|----------------|-------|----------|
| `gfx` | `libs/gfx/include/optics/gfx/` | `libs/gfx/src/` | `tests/gfx/` | `examples/getting_started/` |
| `text` | `libs/text/include/optics/text/` | `libs/text/src/` | `tests/text/` | `examples/showcase/` |
| `ui` | `libs/ui/include/optics/ui/` | `libs/ui/src/` | `tests/ui/` | `examples/showcase/` |
| `window` | `libs/window/include/optics/window/` | `libs/window/src/` | `tests/window/` | `examples/getting_started/` |
| `app` | `libs/app/include/optics/app.h` | `libs/app/src/` | `tests/app/` | `examples/getting_started/` |
| `optics` | `include/optics/optics.h` | Umbrella facade (`optics_dep`, ADR-0160/0164) | `tests/optics/` | — |

Each library exposes a Meson dependency object to the next layer. Code in one
library must not include another library's private `src/` headers.

## Rust Crates

The repository-root `Cargo.toml` is the single Cargo workspace. `crates/`
stands symmetric 1:1 with `libs/` and holds all `optics-*-sys` crates, safe
`optics-*` wrapper crates (with upper crates re-exporting their public
dependencies), and the shared `optics-build` helper (ADR-0163, ADR-0169,
ADR-0176). Cargo commands run from the repository root. See
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
- Add runnable demonstrations under `examples/getting_started/` for a
  single-concept starter, or `examples/showcase/` for a multi-layer flagship
  application ([ADR-0171](../adr/0171-curated-examples-taxonomy-and-showcase-design-system.md)).
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
