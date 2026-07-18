# Contributing to Optics

Optics keeps the full graphics and UI stack in one repository so a change
across `flux`, `lens`, `iris`, examples, tests, documentation, and bindings can
land atomically.

## Dependency direction

```text
iris ──▶ lens ──▶ flux
  host           UI engine      rendering

flux-text ──▶ flux
flux-scene-graph ──▶ flux
```

Keep the ABI boundaries even though the sources share a checkout:

- `flux` owns Vulkan resources and rendering primitives.
- `flux-text` and `flux-scene-graph` translate content into `flux` draw calls.
- `lens` owns headless UI state, layout, input, and draw-list generation.
- `iris` owns the window, event loop, desktop integration, and accessibility
  bridge.

Do not reach into another library's `src/` tree. Use its public headers and
Meson dependency object.

## Building the stack

Configure once at the repository root. Meson builds libraries in dependency
order; no intermediate install or `PKG_CONFIG_PATH` is required.

```bash
meson setup build -Dexamples=true -Dtests=true
meson compile -C build
meson test -C build --no-suite bench
```

See [development setup](../dev/setup.md) for dependencies and
[testing](../dev/testing.md) for suite selection and sanitizer builds.

## Make a change

Place code and coverage together:

- Public C headers: `libs/<library>/include/`.
- Implementations: `libs/<library>/src/`.
- C tests: `tests/<library>/`.
- C examples: `examples/<library>/`.
- Rust workspaces: `bindings/{flux,lens,iris}-rs/`.
- User documentation: `docs/tutorials/`, `docs/how-to/`,
  `docs/reference/`, or `docs/explanation/`.
- Contributor documentation: `docs/dev/`.

When a public API changes, update its reference entry and bindings in the same
change. When a dependency or command changes, update the root README, setup
guide, and getting-started tutorial together.

## Documentation

Documentation ships in the same commit as the code it describes. Follow the
project-neutral [documentation governance](../dev/documentation/index.md) for
routing, writing style, the ADR workflow, and the update checklist.

The governance files themselves are maintainer-controlled policy: assistants
may read and suggest changes to them but must not modify them directly.
