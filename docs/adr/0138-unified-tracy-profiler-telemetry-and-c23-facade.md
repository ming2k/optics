---
id: ADR-0138
title: "Unified Tracy profiler telemetry, dual-thread lifecycle markers, and zero-overhead C23 instrumentation facade"
status: accepted
date: 2026-11-02
scope: [flux, lens, iris, build, tools]
superseded_by: null
negative_knowledge: true
---

# ADR-0138: Unified Tracy profiler telemetry and zero-overhead C23 instrumentation facade

- Status: Accepted
- Date: 2026-11-02
- Scope: `libs/flux/include/flux/profile.h`, `libs/flux/src/core/profile.c`, `libs/lens/src/core/context.c`, `libs/lens/src/layout/solve.c`, `libs/lens/src/store/store.c`, `libs/iris/src/frame_mailbox.c`, `libs/glyph/src/layout.c`, `libs/prism/src/liquid_glass.c`, `meson.build`, `meson_options.txt`.
- Implementation: Landed. Zero-overhead C23 telemetry facade in `<flux/profile.h>` routing to Tracy Profiler via its standard C API when `-Dtracy=true` (`OPTICS_PROFILER_TRACY`), compiling to strict no-ops (`((void)0)`) in release/default builds. Instrumented across dual-thread UI/Presenter boundaries (ADR-0124), mailbox handoffs, lens frame solves, glyph layout, and prism compute shaders. Verified by `tests/flux/unit/test_profile.c`.
- Depends on: [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md) (single descriptor paradigm), [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md) (dual-thread pipeline), [ADR-0129](0129-observation-plane-and-resource-ledger.md) (observation plane), [ADR-0133](0133-pass-labels-on-the-frame-pass-seam.md) (frame pass seam).

---

## Context and Problem Statement

Optics is a high-performance C23 graphics and UI stack operating under strict real-time frame budgets (60Hz–240Hz, 4.16ms–16.6ms). Under [ADR-0124](0124-dual-thread-ui-and-presentation-pipeline.md), execution is partitioned into a decoupled dual-thread topology:
1. **Thread 0 (UI Thread)**: OS event pump, pointer smoothing, AT-SPI accessibility, immediate-mode widget build, text shaping (`glyph`), layout solving (`lens`), and display list serialization.
2. **Thread 1 (Presenter / Compositor Thread)**: Hardware display pacing synchronization ([ADR-0122](0122-compositor-driven-presentation-pacing.md)), wait-free mailbox snapshot consumption (`iris_frame_mailbox`), Vulkan command buffer compilation, GPU submission, and hardware VBlank queue presentation.

While the stack possesses rigorous preventive memory accounting via the Observation Plane ledger ([ADR-0129](0129-observation-plane-and-resource-ledger.md)) and isolated CPU microbenchmarks (`tests/flux/bench/`), it lacked a real-time, nanosecond-resolution temporal profiler. When micro-stutter or frame drops occur in live application sessions, developers faced severe diagnostic blind spots:
1. **Inter-Thread Synchronization & Pipeline Bubbles**: Stutters were indistinguishable between UI thread layout spikes, mailbox handoff starvation, compositor synchronization blocks on `vkAcquireNextImageKHR`, and GPU command completion delays.
2. **Platform Tool Fragmentation**: Linux (`perf`/Hotspot), Windows (ETW/PIX), and macOS (Instruments) use completely disparate tooling workflows with no unified annotation format.
3. **Absence of Correlated Timeline Tracking**: Discrete GPU query timestamps ([ADR-0133](0133-pass-labels-on-the-frame-pass-seam.md)) were returned as raw integer counters without synchronized alignment against CPU UI layout passes on a continuous visual timeline.

## Decision Drivers

1. **Pure C23 Baseline**: Shared library translation units under `libs/` must remain pure C23 (`-std=c23`). C++ runtimes, headers, or compiler requirements must NEVER leak into callers, public headers, or default release builds.
2. **Zero-Overhead in Release**: When profiling is inactive, telemetry points must compile out completely into `((void)0)` or empty statements, guaranteeing 0.00 ns runtime penalty and 0 bytes binary bloat.
3. **Zero Heap Allocation in Frame Walk ([INV-ARCH-ARENA])**: Telemetry must not invoke libc `malloc`/`free` during hot-path frame walk execution (`lens_begin` ➔ `lens_end`).
4. **Single Opts Descriptor Compliance ([INV-ARCH-OPTS])**: Public programmatic entry points must adhere to the single descriptor paradigm (`flux_profile_query`).

## Architectural Decisions

### 1. Opt-in Build Feature with Isolated C++ Compilation (`-Dtracy=true`)

Tracy Profiler is integrated as an optional developer feature in `meson_options.txt`:
```meson
option('tracy', type : 'boolean', value : false,
       description : 'Compile Tracy profiler instrumentation into the libraries (ADR-0138)')
```

- **When `-Dtracy=false` (default)**: No C++ compiler is invoked. `tracy_dep` is an empty `declare_dependency()`. No Tracy symbols or C++ runtimes are linked.
- **When `-Dtracy=true`**: Meson pulls the Tracy client via `subprojects/tracy.wrap` and `subprojects/packagefiles/tracy/meson.build`. C++ is activated strictly for compiling `TracyClient.cpp` as a position-independent shared library (`libtracy.so` / `tracy.dll`) with `-DTRACY_ENABLE` and `-DTRACY_EXPORTS`. All Optics libraries and test executables dynamically link `libtracy.so` with `-DOPTICS_PROFILER_TRACY=1` and `-DTRACY_IMPORTS`, ensuring exactly one single Tracy client profiler instance exists across all shared objects.

### 2. The Pure C23 Telemetry Facade (`<flux/profile.h>`)

Optics defines a first-party facade header `<flux/profile.h>` installed under `flux/`. All internal libraries (`iris`, `lens`, `flux`, `glyph`, `prism`) interact exclusively through standard C macros wrapping Tracy's C API (`<tracy/TracyC.h>`):

```c
#if defined(OPTICS_PROFILER_TRACY)
#  include <tracy/TracyC.h>
   typedef TracyCZoneCtx flux_profile_zone;
#  define FLUX_PROFILE_ZONE(ctx, name)               TracyCZoneN(ctx, name, 1)
#  define FLUX_PROFILE_ZONE_COLOR(ctx, name, color)  TracyCZoneNC(ctx, name, color, 1)
#  define FLUX_PROFILE_ZONE_END(ctx)                 TracyCZoneEnd(ctx)
#  define FLUX_PROFILE_FRAME(name)                   TracyCFrameMarkNamed(name)
#  define FLUX_PROFILE_FRAME_MARK()                  TracyCFrameMark
#  define FLUX_PROFILE_FRAME_START(name)             TracyCFrameMarkStart(name)
#  define FLUX_PROFILE_FRAME_END(name)               TracyCFrameMarkEnd(name)
#  define FLUX_PROFILE_PLOT(name, val)               TracyCPlot(name, (double)(val))
#  define FLUX_PROFILE_PLOT_INT(name, val)           TracyCPlotI(name, (int64_t)(val))
#  define FLUX_PROFILE_ALLOC(ptr, size)              TracyCAlloc(ptr, size)
#  define FLUX_PROFILE_FREE(ptr)                     TracyCFree(ptr)
#  define FLUX_PROFILE_ALLOC_NAMED(ptr, size, name)  TracyCAllocN(ptr, size, name)
#  define FLUX_PROFILE_FREE_NAMED(ptr, name)         TracyCFreeN(ptr, name)
#  define FLUX_PROFILE_SET_THREAD_NAME(name)         TracyCSetThreadName(name)
#else
   typedef const void *flux_profile_zone;
#  define FLUX_PROFILE_ZONE(ctx, name)
#  define FLUX_PROFILE_ZONE_COLOR(ctx, name, color)
#  define FLUX_PROFILE_ZONE_END(ctx)
#  define FLUX_PROFILE_FRAME(name)                   ((void)0)
#  define FLUX_PROFILE_FRAME_MARK()                  ((void)0)
#  define FLUX_PROFILE_FRAME_START(name)             ((void)0)
#  define FLUX_PROFILE_FRAME_END(name)               ((void)0)
#  define FLUX_PROFILE_PLOT(name, val)               ((void)0)
#  define FLUX_PROFILE_PLOT_INT(name, val)           ((void)0)
#  define FLUX_PROFILE_ALLOC(ptr, size)              ((void)0)
#  define FLUX_PROFILE_FREE(ptr)                     ((void)0)
#  define FLUX_PROFILE_ALLOC_NAMED(ptr, size, name)  ((void)0)
#  define FLUX_PROFILE_FREE_NAMED(ptr, name)         ((void)0)
#  define FLUX_PROFILE_SET_THREAD_NAME(name)         ((void)0)
#endif
```

### 3. Canonical Seam Placements

Telemetry is positioned exclusively at architectural boundaries:
- **`libs/iris`**: `iris_mailbox_publish` (tracks handoff latency and plots sequence counter), `iris_mailbox_claim`, UI event loop iterations (`iris-ui` frame markers), and presenter worker thread iterations (`iris-presenter` frame markers).
- **`libs/lens`**: `lens_begin`/`lens_end` frame boundaries (`lens.frame`), `lensi_layout_solve` (Flex/Grid/Flow solve durations), and `lensi_store_reap` (GC phase transitions).
- **`libs/flux`**: `flux_frame_present` and `vkQueuePresentKHR` driver timing, `flux_arena_reset` (plots per-frame arena byte occupancy high-water marks).
- **`libs/glyph`**: `text_draw_impl` layout and shaping pass duration.
- **`libs/prism`**: `prism_liquid_glass_filter_apply` compute dispatch timing.

### 4. Single Opts Descriptor Query Contract

The subsystem exports a public status query in `<flux/profile.h>` adhering to `[INV-ARCH-OPTS]`:

```c
typedef struct flux_profile_opts {
    const char *app_info;
} flux_profile_opts;

typedef struct flux_profile_status {
    bool enabled;
    bool connected;
} flux_profile_status;

[[nodiscard]] FLUX_API flux_profile_status flux_profile_query(const flux_profile_opts *opts);
```

## Alternatives Considered

- **Sampling profilers only (`perf`, gprof, Instruments).** Rejected: sampling aggregates overall CPU utilization percentages across run lifetimes, but cannot visualize frame-by-frame concurrency bubbles, VSync handoff jitter, or thread migration stalls.
- **In-house custom timeline visualizer.** Rejected: reinventing a nanosecond multi-threaded visual timeline GUI, network socket streamer, callstack unwinder, and memory profiler requires thousands of hours and directly violates Gate 1 (Irreducibility).
- **Perfetto / Chrome Trace Event JSON output.** Rejected: emitting JSON events on high-frequency UI/graphics paths incurs substantial serialization overhead, lacks real-time streaming GUI interaction, and does not provide seamless Vulkan timeline alignment.
- **Static linking of `TracyClient.a` into individual shared objects.** Rejected: causes duplicate static `s_profiler` instances in the same process address space, triggering assertion failures (`!s_instance`) during library initialization.

## Consequences

- **Developer Superpower**: Developers can connect the Tracy GUI (`tracy-profiler`) over localhost TCP port 8086 to inspect UI thread build passes, Mailbox snapshot exchange, and hardware presentation timing on the exact same nanosecond timeline.
- **Zero Release Contamination**: In default builds (`-Dtracy=false`), the compiler encounters only empty macros. No extra link libraries, zero binary bloat, and zero ABI differences.
- **Strict Invariant Compliance**: Verified through `tools/optics check` across boundary isolation, library stdout purity, version lockstep, and symbols synchronization.
