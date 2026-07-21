# ADR-0031: Lens symbol namespaces — public `lens_*`, internal `lensi_*`

- Status: Accepted
- Date: 2026-07-21
- Scope: lens (L2 toolkit). Defines the symbol-prefix contract.

## Context

Lens ships one shared library and one umbrella header
([ADR-0024](0024-lens-foundations.md)). Like flux, lens needs a stable,
discoverable boundary between public API and private internals, and a
way to keep internals out of the exported symbol table.

Forces:

1. **Public API stability.** Consumers include `<lens/lens.h>` and link
   `liblens.so`; the public symbols must be namespaced and versioned.
2. **Internal freedom.** Cross-module private helpers need to be callable
   from any translation unit in the library without becoming public API.
3. **Monorepo boundary.** In the unified monorepo
   ([ADR-0023](0023-unified-monorepo-build.md)), reviewers must enforce
   public API boundaries because source-directory proximity no longer
   prevents private cross-layer includes.

## Decision

1. **Public symbols use the `lens_*` prefix.** Every entry point in
   `<lens/lens.h>` (and subset headers) is `lens_<thing>` and is
   exported with `LENS_API` (visibility `default` on GCC/Clang,
   `__declspec(dllexport)` / `dllimport` on Windows).
2. **Internal symbols use the `lensi_*` prefix.** Any function, type, or
   macro that is cross-module-private lives in `src/internal.h` (not
   installed) and is named `lensi_*`. The prefix marks "internal to the
   library, not exported" — the convention is enforced by review and by
   the absence of `LENS_API` on these symbols.
3. **The umbrella header is the only public header.** `internal.h` and
   every `src/**/*.c` are private; consumers never include them.
4. **Terse and descriptor widget forms** (`lens_button` vs
   `lens_button_ex`) both remain `lens_*`; the `_ex` suffix is a naming
   convention for the descriptor-taking variant, not a visibility marker.

References: `libs/lens/include/lens/lens.h` (Visibility section, header
comment), `libs/lens/src/internal.h`.

## Alternatives Considered

- **A second public header for internals.** Reject: the whole point of
  the `lensi_*` prefix is that internals are not API and must not be
  installed.
- **C++ namespaces / a C++ wrapper.** Reject: lens is C23; the prefix
  convention is the idiomatic C answer.
- **Anonymous-namespace / hidden visibility only, no prefix.** Reject:
  the prefix is documentation as much as it is a linker rule; it tells a
  reader "do not call this from outside the library" at the call site.

## Consequences

Positive:

- The public surface is grep-able (`lens_*` in installed headers) and
  the internal surface is grep-able (`lensi_*` in `src/`).
- Reviewers can enforce the boundary mechanically: any `lensi_*`
  reference outside `libs/lens/src/` is a leak.

Negative:

- The `lensi_*` prefix is a convention; a missing `static` on a file
  -local helper that does not use the prefix could leak a plain-named
  symbol. Mitigated by `-fvisibility=hidden` as the default and
  `LENS_API` opt-in.

## References

- [ADR-0024](0024-lens-foundations.md) — foundations.
- [ADR-0023](0023-unified-monorepo-build.md) — monorepo boundary
  enforcement.
