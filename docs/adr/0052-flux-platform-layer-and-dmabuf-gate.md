# ADR-0052: flux platform abstraction layer + Linux-only dma-buf gate

- Status: Accepted
- Date: 2026-08-08
- Scope: flux core. How OS facilities are reached, and how the Linux-only
  dma-buf subsystem degrades on Windows/macOS.

## Context

flux core grew up Linux-only and accumulated four kinds of POSIX/GNU
coupling: `pthread_mutex_t` device-level locks (~10 locks, ~130 call
sites), `__builtin_mul_overflow` (no MSVC equivalent), C11 `<threads.h>`
for `thread_local` (absent from MinGW/MSVC), and the dma-buf subsystem
(`VK_EXT_external_memory_dma_buf`, sync_file fds, `<unistd.h>` fd
ownership) which has no Windows/macOS counterpart. The goal of running
flux on Windows (MSVC/clang-cl/MinGW) and macOS needs these funnelled
behind a seam without changing Linux behaviour.

## Decision

1. **`src/core/platform.h`** (internal, never installed) is the single
   home for OS shims:
   - `flux_platform_mutex` — `SRWLOCK` on `_WIN32` (no kernel object, no
     destroy), `pthread_mutex_t` elsewhere. Call sites keep the existing
     `lock` + `lock_initialized` field pattern.
   - `flux_platform_mul_size` — overflow-checked `size_t` multiply;
     `__has_builtin(__builtin_mul_overflow)` where available, a portable
     `a > SIZE_MAX / b` check otherwise.
   - `flux_platform_strdup` — malloc+memcpy (MSVC spells it `_strdup`).
2. **dma-buf is compile-time gated**: `FLUX_HAVE_DMABUF` is defined (in
   both private and public compile args) only when
   `host_machine.system() == 'linux'`. Other platforms compile
   `src/core/dmabuf_stub.c`, which implements every public
   `flux/dmabuf.h` entry point returning `FLUX_ERROR_UNSUPPORTED` — the
   stub replicates the real implementation's argument-validation order so
   `test_dmabuf` behaves identically on dmabuf-less systems. The stub
   also provides empty internal acquire-semaphore pool functions so
   `device.c`/`frame.c` carry no `#ifdef` at call sites. Public headers
   and the installed header set are identical on all platforms.

   Amendment (2026-08-10 audit fix): the dmabuf sources moved from
   `src/canvas/` to `src/core/` and compile unconditionally — the
   core call sites (`device.c` / `surface.c` / `frame.c`) reference the
   pool helpers and `flux_dmabuf_supported` whether or not the canvas
   module is enabled, so gating the sources on `-Dcanvas` broke the
   `-Dcanvas=false` link. The one entry point that genuinely needs
   canvas internals, `flux_canvas_wait_dmabuf_acquire`, lives in
   `src/canvas/dmabuf_acquire.c` inside the canvas gate; its platform
   fork goes through `flux_dmabuf_import_acquire_semaphore` /
   `flux_dmabuf_close_fd` in the core pair, so it carries no `#ifdef`
   either.
3. **`-D_GNU_SOURCE` is Linux-only** in the root `meson.build`; the
   `-Wno-*` project arguments are probed via `cc.get_supported_arguments`
   (MSVC cl errors on unknown options).
4. **`<threads.h>` is not used**: `thread_local` is a C23 keyword; the
   include was dropped (MinGW/MSVC do not ship the header).

## Alternatives Considered

- **C11 `threads.h` `mtx_t` as the mutex abstraction.** Rejected: Apple
  libc ships no `<threads.h>` at all; it would solve one platform by
  breaking another.
- **winpthreads on Windows.** Rejected: a runtime dependency for ten
  mutexes; SRWLOCK is lighter, always present, and destroy-free.
- **Hiding the dma-buf API on non-Linux headers.** Rejected: identical
  headers keep bindings and host code branch-free; a documented
  `FLUX_ERROR_UNSUPPORTED` return is the cleaner degradation, and the
  feature is already opt-in via `FLUX_DEVICE_FEATURE_DMABUF*`.
- **`<threads.h>` fallback macros.** Rejected: deleting a needless
  include beats shimming it.

## Consequences

- New flux code must use the platform.h shims; pthread/strdup/builtin
  overflow must not reappear (CI's zig-cc Windows compile gate checks
  every flux TU).
- dma-buf consumers must handle `FLUX_ERROR_UNSUPPORTED` — they already
  do by contract (`flux_dmabuf_supported()` is false there).
- The Windows compile gate proved all 31 flux TUs build for
  `x86_64-windows-gnu`; runtime behaviour on real Windows/macOS drivers
  remains to be validated on hardware.

## References

- [ADR-0006](0006-no-runtime-rhi.md) — the rendering API stays Vulkan;
  this ADR changes OS shims, not the RHI.
- [ADR-0049](0049-strict-drm-vulkan-device-selection.md) — the DRM
  device-selection extension stays Linux-only and optional.
- `libs/flux/src/core/platform.h`, `libs/flux/src/core/dmabuf_stub.c`.
