# ADR-0044: Iris backend compile-time selection

- Status: Accepted
- Date: 2026-07-21
- Scope: iris (L3 application toolkit). Defines how a platform backend is
  chosen.

## Context

`iris_app_run` is the cross-platform entry point
([ADR-0043](0043-iris-foundations.md)), but the actual window/event-loop
implementation is platform-specific: Wayland on Linux today, Win32 and
Cocoa in the future. The question is how the active backend is chosen
and how the choice stays invisible to the public API.

Forces:

1. **One public signature.** `iris_app_run(const iris_app_config *)` must
   be the same on every platform; the backend is an implementation
   detail.
2. **No runtime loader.** No realistic iris consumer links two backends
   in one binary; a `dlopen` loader would double the init surface for no
   benefit.
3. **Linkable everywhere.** A platform-less build (CI, bindgen) must
   still produce a `libiris` that links, with `iris_app_run` returning a
   documented non-zero code.
4. **Capability-gated sources.** Some platform code depends on optional
   libraries (libsystemd for portal watch + AT-SPI; wayland-client /
   xkbcommon / vulkan for the Wayland backend). Missing dependencies
   must degrade, not fail the build.
5. **Room to grow.** Adding `app_win32.c` or `app_cocoa.m` later must be
   a new file selected at configure time, not a rewrite of the
   dispatcher.

## Decision

1. **Compile-time backend selection.** The active backend is chosen at
   meson configure time based on host platform and available
   dependencies. `iris_app_run` in `libs/iris/src/app.c` is a thin
   dispatcher that forwards to the selected backend (today:
   `iris_app_run_wayland`). Implemented in `libs/iris/src/app.c` and
   selected in `libs/iris/meson.build`.
2. **One backend per build.** Exactly one backend is compiled in. The
   dispatcher has a single forward declaration and call; there is no
   runtime branch over backends.
3. **`IRIS_BUILD_NO_BACKEND` fallback.** When the host platform is not
   Linux or the Wayland/xkbcommon/vulkan dependencies are missing,
   meson defines `IRIS_BUILD_NO_BACKEND`. In that build `iris_app_run`
   returns `2` and `iris_request_animation_frame` is a no-op, so the
   library still links for header / bindgen / platform-less CI use.
4. **Capability `#define`s for optional sources.**
   - `IRIS_HAVE_WAYLAND` — Wayland backend sources compiled in.
   - `IRIS_HAVE_PORTAL_WATCH` — live theme watching via sd-bus.
   - `IRIS_HAVE_ATSPI` — real AT-SPI bridge (else the a11y stub).
   Each gates a source file (`app_wayland.c`, `theme_watch_portal.c`,
   `a11y_atspi.c`) and falls back to a stub
   (`theme_watch_stub.c`, `a11y_stub.c`) so the public API is unchanged.
5. **Future backends are new files.** A Win32 backend will land as
   `src/app_win32.c` (selected when `host_machine.system() == 'windows'`),
   Cocoa as `src/app_cocoa.m`; both will satisfy the same internal
   `iris_app_run_<backend>` signature the dispatcher calls.
6. **`gnu_symbol_visibility: 'hidden'`.** Only `IRIS_API`-marked symbols
   are exported; backend entry points (`iris_app_run_wayland`, etc.) are
   internal to the build and never reachable from outside the library.

References: `libs/iris/src/app.c`, `libs/iris/meson.build`,
`libs/iris/include/iris/app.h`.

## Alternatives Considered

- **Runtime backend selection via dlopen.** Reject: no consumer links
   two backends; the loader doubles init/factory API for no gain and
   adds a dynamic-dependency failure mode.
- **Separate libraries per backend (`libiris-wayland`, `libiris-win32`).**
   Reject: fragments the install; the public surface is identical, so
   one `libiris` with a configure-time choice is simpler.
- **Hard failure when Wayland deps are missing.** Reject: breaks
   platform-less CI and bindgen; the `IRIS_BUILD_NO_BACKEND` fallback
   keeps the library linkable.
- **Backend selection by meson `<option>`.** A future refinement once a
   second backend exists; today there is exactly one and host-machine
   detection is sufficient.

## Consequences

Positive:

- The public API is identical across platforms; consumers never branch on
  the backend.
- The same source tree compiles from a full Wayland desktop down to a
  header-only bindgen target.
- Adding a backend is additive: a new source file and a meson branch,
  not a dispatcher rewrite.

Negative:

- A single binary cannot target multiple platforms; that is the deliberate
  cost of compile-time selection.
- The dispatcher's forward declaration is backend-specific
  (`iris_app_run_wayland`); a second backend will require either a
  renamed shared signature or a small `#if` in `app.c`. Planned and
  contained.

## References

- [ADR-0043](0043-iris-foundations.md) — the foundations this backend
  seam realises.
- [ADR-0023](0023-unified-monorepo-build.md) — the monorepo build that
  wires iris's dependencies.
- [ADR-0035](0035-lens-accessibility-tree.md) — the a11y seam whose
  AT-SPI backend is gated by `IRIS_HAVE_ATSPI`.
