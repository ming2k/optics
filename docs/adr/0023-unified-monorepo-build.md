# ADR-0023: Unified Monorepo Build

- Status: Accepted
- Date: 2026-07-18
- Supersedes: [ADR-0018](0018-obsolete-iris-meson-subprojects.md)

## Context

`flux`, `lens`, and `iris` were originally maintained as separate repositories.
ADR-0018 introduced nested Meson subprojects to reduce the install and
pkg-config friction of cross-layer work while keeping that polyrepo layout.

The current Optics source tree instead contains all three C libraries, the
`flux-text` and `flux-scene-graph` siblings, examples, tests, documentation,
and Rust binding workspaces. The historical subproject instructions now point
at directories and release workflows that do not exist in this repository.

The architectural layers still matter: `flux` is independently consumable,
`lens` is headless, and `iris` owns application and desktop integration. A
shared checkout must not turn source-directory proximity into private API
coupling.

## Decision

Build the complete C stack from the repository root as one Meson project:

```bash
meson setup build
meson compile -C build
```

The root `meson.build` enters each library in dependency order and passes
Meson dependency objects between them. Development builds do not use nested
wraps, intermediate installs, or a shared development prefix.

Keep the public library boundaries:

- Every C library owns its public include tree and private source tree.
- Cross-library calls use public headers and declared Meson dependencies.
- Each installable C library continues to produce its own shared library and
  pkg-config metadata.
- Rust bindings live in separate Cargo workspaces under `bindings/`, but in
  the same repository so C and Rust API changes can be atomic.
- Tests and examples live in top-level module directories and are orchestrated
  by the root build.

## Consequences

Positive:

- Cross-layer changes, tests, bindings, and documentation land in one commit.
- One configure and compile command builds the complete C dependency graph.
- Development no longer depends on installed headers, stale prefixes, nested
  wrap resolution, or inter-repository commit ordering.
- A single source revision describes the complete stack.

Negative:

- The checkout and default build include more components than a single-library
  repository.
- Component release and version boundaries must be stated explicitly inside a
  repository that also has a root project version.
- Reviewers must enforce public API boundaries because repository layout alone
  no longer prevents private cross-layer includes.

## Alternatives Considered

- **Keep the polyrepo with Meson subprojects.** This preserves repository-level
  separation but restores wrap maintenance, cross-repository coordination, and
  multiple release histories without providing a benefit to the current tree.
- **Use an umbrella repository with git submodules.** This keeps separate Git
  histories but makes atomic changes and onboarding harder.
- **Collapse the libraries into one ABI.** This simplifies linking but removes
  the independently consumable renderer and headless UI boundaries.

## See Also

- [Project layout](../dev/project-layout.md)
- [Development setup](../dev/setup.md)
- [Application architecture](../explanation/application-architecture.md)
