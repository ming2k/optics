# ADR-0001: Meson subprojects for the flux + lens + iris stack

- Status: Accepted
- Date: 2026-06-23

## Context

flux, lens, and iris are three independent repositories with a strict
build order — `flux → lens → iris` — discovered exclusively through
pkg-config. The polyrepo contract is documented in each repo (lens's
`libs/lens/meson.build`; iris's [setup](../dev/setup.md) and
[release](../dev/release.md) docs; the README build sequence). Today a
developer must `meson install` each layer into `$PREFIX` before the next can
configure against it.

Three forces make this friction acute enough to revisit.

### 1. Cross-cutting changes have a breakage window

The flux [ADR-0016](https://github.com/ming2k/flux/blob/main/docs/adr/0016-pure-rhi-and-draw-primitives.md)
text extraction moved `flux_text_*` out of `libflux` into a `flux-text`
sibling. That change required coordinated edits across **two** repos: flux
(remove text, add the sibling) and lens (`#include <flux-text/text.h>`,
add the `flux-text` pkg-config dependency). In a polyrepo workflow those are
two commits in two repos, and between them lens is broken against the new
flux. A third layer (iris) is one more cascade step. The edits are
mechanical and bounded, but they cannot be atomic.

### 2. The inter-repo install dance

Every rebuild of flux requires `meson install` before lens sees it; every
rebuild of lens requires `meson install` before iris sees it. During
iterative cross-layer work this is the dominant friction. It also forces a
single shared `$PREFIX` and careful cleanup of stale installed headers (the
text extraction left a stale `<flux/text.h>` in `$PREFIX` that had to be
removed by hand, because `meson install` does not delete files that are no
longer installed).

### 3. Runtime loader paths got stricter

Before ADR-0016, lens needed only `libflux.so` (text lived inside it). After
the split, lens and iris binaries need `libflux_text.so` too, and that
library's own transitive dependency on `libflux.so` is not resolved by a
consumer's `DT_RUNPATH` (RUNPATH, unlike RPATH, is not inherited by
transitive deps). With `$PREFIX/lib` absent from the `ld.so` cache, this
surfaced as `libflux_text.so => not found` at runtime. The dev environment
now needs `LD_LIBRARY_PATH=$PREFIX/lib` or an `ld.so.conf.d` entry that the
polyrepo install flow does not provide.

### What must be preserved

The three-repo separation is deliberate, recorded in each repo's
foundational ADR, and mirrors GSK/GTK and Flutter Engine/Framework:

- flux is a general Vulkan rendering library, reusable outside any UI.
- lens's defining contract is "input arrives as data, no windowing linked"
  — the property that makes it headless-testable and embeddable.
- iris absorbs windowing / event loop / a11y so lens stays pure.

The pkg-config ABI boundary (`flux.pc`, `lens.pc`) is the contract that
enforces the layering. Any solution must keep each repo independently
cloneable, buildable, versionable, and consumable; the layers must not
reach into one another's internals.

## Decision

Adopt **meson subprojects** to compose the stack at build time, without
merging the repositories.

```
iris/subprojects/lens.wrap   →  lens source  (local-path or git wrap)
lens/subprojects/flux.wrap    →  flux source
```

A single `meson setup build && meson compile -C build` at the iris root
builds flux, lens, and iris from source, in dependency order, with no
`meson install` between layers. The build-tree `.pc` files Meson emits wire
the layers together automatically; the pkg-config ABI contract is unchanged.

- **Local-path wraps for development** (`directory = ../flux`), so an
  in-place checkout of all three repos edits and rebuilds atomically.
- **Git wraps for releases / CI** (pinned commits), so the exact sibling
  versions are reproducible and a fresh clone of iris alone builds the
  whole stack.
- **The ABI boundary stays.** iris consumes lens via `lens_dep`; lens
  consumes flux via `flux_dep` / `flux_text_dep`; no layer reaches into
  another's `src/`. Each repo still builds standalone from its own root
  against an installed flux/lens.

## Scope boundaries

In scope:

- A build-time composition layer (subprojects). It changes how the three
  repos are built together, not what each contains or exports.

Out of scope, deliberately:

- **Merging the repositories.** Rejected — see Alternatives.
- **Changing release or versioning independence.** flux, lens, and iris
  continue to version and release separately. A subproject wrap pins a
  sibling *revision*; it does not unify version numbers.
- **Removing the pkg-config contract.** Subprojects produce the same `.pc`
  files an install would; consumers that are not iris (editors, render
  farms, tests) still link flux/lens the same way.

## Consequences

Positive:

- No inter-repo `meson install`. One `meson setup` builds the stack.
- Cross-cutting changes are atomic in the workspace: edit flux + lens +
  iris, rebuild once, run the whole stack's tests in one invocation. The
  ADR-0016 cascade would have been one commit's worth of coordinated edits.
- Reproducible dev and CI environments: the wrap pins the exact sibling
  commit, so "works on my machine" stops depending on what is installed in
  `$PREFIX`.
- Stale-header drift disappears: there is no installed `$PREFIX` to go
  stale, because nothing is installed during development.

Negative:

- Wrap maintenance: each release of flux or lens requires updating the
  downstream wrap's pinned commit. This is the cost of pinning.
- Nested subproject resolution: building iris pulls lens, which pulls flux.
  Meson handles nested subprojects, but the wraps must agree on a single
  flux revision (a iris-level flux wrap and a lens-level flux wrap that
  disagree is a real failure mode). Mitigation: the top layer's wrap wins;
  document the resolution rule.
- A new concept to teach contributors. Mitigation: the build command gets
  *simpler* (one setup), so the net cognitive load drops.

## Alternatives considered

- **Merge flux, lens, and iris into one monorepo.** Rejected. It removes
  the install dance and makes changes atomic, but at the cost of the
  deliberate layering: the pkg-config ABI boundary would erode (the
  temptation to reach across into another layer's internals), each layer
  would lose independent consumability and release cadence, and the
  recorded rationale in each repo's ADR-0001 would be reversed. Subprojects
  deliver the same workflow benefit without that cost.
- **Umbrella repository with git submodules.** A variant of the merge: one
  checkout of all three repos, but each submodule still installs into
  `$PREFIX` to satisfy the next, so the install dance and the stale-header
  drift remain. Submodules compose well *with* subprojects (umbrella for
  the checkout, subprojects for the build), but subprojects alone already
  achieve the goal.
- **Status quo (polyrepo + install dance + `LD_LIBRARY_PATH`).** Works, and
  is what the stack does today, but the friction grows with every
  cross-layer change and with every sibling split like flux-text. The
  ADR-0016 extraction made the cost concrete.

## See also

- flux
  [ADR-0016](https://github.com/ming2k/flux/blob/main/docs/adr/0016-pure-rhi-and-draw-primitives.md)
  — the text / scene-graph sibling split that motivated this ADR.
- [iris ADR-0001](../../libs/iris/docs/adr/0001-why-iris-exists.md) —
  why iris is a separate repo from lens.
- [Development setup](../dev/setup.md) — the current `flux → lens → iris`
  install sequence this ADR proposes to replace.
- [Release](../dev/release.md) — the stack build order at release time.
