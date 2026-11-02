/*
 * flux/profile.h — execution profiler telemetry facade (ADR-0138).
 *
 * Provides a zero-overhead, pure C23 profiling and telemetry facade for
 * real-time frame timeline tracking, multi-thread execution boundaries,
 * and memory dynamics.
 *
 * Design contract:
 *   - Pure C23 baseline. C++ runtimes or headers NEVER leak into callers.
 *   - When OPTICS_PROFILER_TRACY is disabled (release/default build), all
 *     profiling macros expand to ((void)0) or empty statements, resulting in
 *     strictly 0.00 ns runtime overhead and 0 bytes binary bloat.
 *   - When OPTICS_PROFILER_TRACY is enabled (-Dtracy=true), the facade
 *     routes transparently to Tracy Profiler (ADR-0138) via its standard C API.
 *   - Hot-path frame walk memory is untouched: profiling telemetry does not
 *     invoke heap allocation on the UI or canvas render paths (INV-ARCH-ARENA).
 */

#ifndef FLUX_PROFILE_H
#define FLUX_PROFILE_H

#include <flux/core.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Macros (zero-overhead in release builds)                           */
/* ================================================================== */

#if defined(OPTICS_PROFILER_TRACY)

#include <tracy/TracyC.h>

typedef TracyCZoneCtx flux_profile_zone;

#define FLUX_PROFILE_ZONE(ctx, name) TracyCZoneN(ctx, name, 1)
#define FLUX_PROFILE_ZONE_COLOR(ctx, name, color) TracyCZoneNC(ctx, name, color, 1)
#define FLUX_PROFILE_ZONE_END(ctx) TracyCZoneEnd(ctx)

#define FLUX_PROFILE_FRAME(name) TracyCFrameMarkNamed(name)
#define FLUX_PROFILE_FRAME_MARK() TracyCFrameMark
#define FLUX_PROFILE_FRAME_START(name) TracyCFrameMarkStart(name)
#define FLUX_PROFILE_FRAME_END(name) TracyCFrameMarkEnd(name)

#define FLUX_PROFILE_PLOT(name, val) TracyCPlot(name, (double)(val))
#define FLUX_PROFILE_PLOT_INT(name, val) TracyCPlotI(name, (int64_t)(val))

#define FLUX_PROFILE_ALLOC(ptr, size) TracyCAlloc(ptr, size)
#define FLUX_PROFILE_FREE(ptr) TracyCFree(ptr)
#define FLUX_PROFILE_ALLOC_NAMED(ptr, size, name) TracyCAllocN(ptr, size, name)
#define FLUX_PROFILE_FREE_NAMED(ptr, name) TracyCFreeN(ptr, name)

#define FLUX_PROFILE_SET_THREAD_NAME(name) TracyCSetThreadName(name)
#define FLUX_PROFILE_MESSAGE(txt, len) TracyCMessage(txt, len)
#define FLUX_PROFILE_APP_INFO(txt, len) TracyCAppInfo(txt, len)

#else

typedef const void *flux_profile_zone;

#define FLUX_PROFILE_ZONE(ctx, name)
#define FLUX_PROFILE_ZONE_COLOR(ctx, name, color)
#define FLUX_PROFILE_ZONE_END(ctx)

#define FLUX_PROFILE_FRAME(name) ((void)0)
#define FLUX_PROFILE_FRAME_MARK() ((void)0)
#define FLUX_PROFILE_FRAME_START(name) ((void)0)
#define FLUX_PROFILE_FRAME_END(name) ((void)0)

#define FLUX_PROFILE_PLOT(name, val) ((void)0)
#define FLUX_PROFILE_PLOT_INT(name, val) ((void)0)

#define FLUX_PROFILE_ALLOC(ptr, size) ((void)0)
#define FLUX_PROFILE_FREE(ptr) ((void)0)
#define FLUX_PROFILE_ALLOC_NAMED(ptr, size, name) ((void)0)
#define FLUX_PROFILE_FREE_NAMED(ptr, name) ((void)0)

#define FLUX_PROFILE_SET_THREAD_NAME(name) ((void)0)
#define FLUX_PROFILE_MESSAGE(txt, len) ((void)0)
#define FLUX_PROFILE_APP_INFO(txt, len) ((void)0)

#endif

/* ================================================================== */
/*  Public API (Single Opts Descriptor Paradigm, ADR-0082)            */
/* ================================================================== */

/* Options descriptor for flux_profile_query. */
typedef struct flux_profile_opts {
    const char *app_info; /* Optional application metadata broadcast */
} flux_profile_opts;

/* Structured response describing the active profiler state. */
typedef struct flux_profile_status {
    bool enabled;   /* Compiled with active profiler instrumentation */
    bool connected; /* Profiler server is currently connected and capturing */
} flux_profile_status;

/* Query profiler runtime state. */
FLUX_NODISCARD FLUX_API flux_profile_status flux_profile_query(const flux_profile_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_PROFILE_H */
