---
id: ADR-0141
title: "Material compute pipeline pre-warming, cold-start hitch elimination, and headless performance governance"
status: accepted
date: 2026-11-03
superseded_by: null
scope: [prism, flux, tools, architecture]
negative_knowledge: true
---

# ADR-0141: Material compute pipeline pre-warming, cold-start hitch elimination, and headless performance governance

- Status: Accepted
- Date: 2026-11-03
- Scope: `libs/prism/include/prism/{liquid_glass,frosted,acrylic,mica,backdrop_layer}.h`, `libs/prism/src/{liquid_glass,frosted,acrylic,mica,backdrop_layer}.c`, `tools/optics.py`, `tools/guardrails/bench_sla.py`.
- Implementation: Landed. Closes the cold-start pipeline compilation hitch discovered via Tracy telemetry (ADR-0138). Extends all 5 Prism physical material filters with typed C23 warmup descriptors adhering to `[INV-ARCH-OPTS]`, moves `vkCreateComputePipelines` out of the frame walk hot-path, and establishes an automated headless SLA performance gate.
- Depends on: [ADR-0138](0138-unified-tracy-profiler-telemetry-and-c23-facade.md), [ADR-0063](0063-liquid-glass-material-library.md), [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md).

---

## Context and Problem Statement

Following the introduction of the zero-overhead Tracy Profiler telemetry facade ([ADR-0138](0138-unified-tracy-profiler-telemetry-and-c23-facade.md)), nanosecond-resolution performance measurements on Optics subsystems revealed an acute architectural defect:

1. **The 13.56 ms Cold-Start Hitch**: In physical material rendering benchmarks (`tests/prism/integration/test_liquid_glass`), the first execution of `prism_liquid_glass_filter_apply` took **13.56 ms**, while steady-state execution averaged only **0.55 ms**. On 120Hz (8.33 ms budget) and 240Hz (4.16 ms budget) displays, this caused an unavoidable first-frame hitch (frame drop).
2. **Lazy Pipeline Compilation in the Frame Walk**: All five material filters under `libs/prism` (`liquid_glass`, `frosted`, `acrylic`, `mica`, `backdrop_layer`) deferred `flux_compute_pipeline_create` (and therefore `vkCreateComputePipelines`) to the inside of `filter_apply()` on the active frame walk path.
3. **Absence of Mechanical Performance Governance**: While memory leaks are protected by a mechanical gate ([ADR-0130](0130-leak-coverage-mechanical-gate.md)), performance budgets had no automated CLI verification or regression assertions.

## Decision Drivers

1. **Clean-Break Architecture**: Zero backward compatibility compromises. If cold-start JIT inside render passes violates real-time frame budgets, the lazy compilation pattern must be eradicated at its root.
2. **Single Opts Descriptor Paradigm ([INV-ARCH-OPTS])**: Pre-warming APIs must adhere strictly to the uniform single-descriptor signature `<ReturnStruct> <module>_<noun>_<verb>(<Handle> *h, const <module>_<verb>_opts *opts)`.
3. **Zero Heap Allocation in Hot Paths ([INV-ARCH-ARENA])**: Rendering passes (`apply`) must be purely dispatch-only without allocating pipeline objects or invoking driver JIT compilers.
4. **Automated Headless Verification**: Profiling and SLA verification must be fully executable in headless environments (`tracy-capture` + `tracy-csvexport`) without human GUI dependency.

---

## Architectural Decisions

### 1. Pure C23 Material Pipeline Pre-warming Contracts

All five physical surface material filters in `prism` export typed warmup descriptors and entry points:

```c
typedef struct prism_liquid_glass_warmup_opts {
    flux_format format; /* Target image format (0 for FLUX_FORMAT_RGBA8_UNORM) */
    bool stats;         /* Pre-warm stats reduction pipeline */
} prism_liquid_glass_warmup_opts;

[[nodiscard]] PRISM_API flux_result
prism_liquid_glass_filter_warmup(prism_liquid_glass_filter *filter,
                                 const prism_liquid_glass_warmup_opts *opts);
```

Equivalent typed descriptors are provided for `frosted`, `acrylic`, `mica`, and `backdrop_layer`:
- `prism_frosted_filter_warmup(filter, opts)`
- `prism_acrylic_filter_warmup(filter, opts)`
- `prism_mica_filter_warmup(filter, opts)`
- `prism_backdrop_layer_filter_warmup(filter, opts)`

When `opts == nullptr` or `{}` is passed, the function eagerly compiles the standard RGBA8 compute pipelines (clear, evaluation, and stats). Callers can pre-warm materials during application boot, scene loading, or background workers, completely eliminating driver shader compilation during active frame recording.

### 2. Fine-Grained Telemetry Differentiation

Inside material implementations, pipeline creation is wrapped in a dedicated profiler telemetry zone:

```c
FLUX_PROFILE_ZONE(z_compile, "prism.liquid_glass.compile_pipelines");
```

This isolates one-time compilation overhead from runtime compute dispatch in Tracy's timeline, allowing instant visual differentiation between driver compilation latency and GPU command recording.

### 3. Headless Performance Governance (`tools/optics check bench`)

Optics integrates a headless performance guardrail into `./tools/optics check bench` backed by `tracy-capture` and `tracy-csvexport`. The guardrail mechanically verifies industrial-grade SLAs:
- **`iris.mailbox_publish` / `claim`**: P95 < 1.0 µs (lock-free communication).
- **`lens.layout_solve`**: P95 < 100 µs (500-node UI tree).
- **`lens.store_reap`**: P95 < 20 µs (retained state GC).
- **`prism.*.apply` (warmed)**: P95 < 1.0 ms (physical material dispatch).
- **`flux.frame_present`**: P95 < 10 µs (Vulkan queue present submission).

Any regression exceeding these limits immediately fails CI with a structured diagnostic diff.

---

## Consequences

- **Zero First-Frame Jank**: Calling `prism_*_filter_warmup` at initialization reduces first-frame material execution from **13.56 ms to 0.55 ms** (a 24x speedup), comfortably fitting within 120Hz and 240Hz frame budgets.
- **Architectural Symmetry**: All five material filters share uniform creation, warmup, apply, and release contracts without historical quirks or special-case laziness.
- **Strict Invariant Compliance**: Enforced through `./tools/optics check` across boundary separation, symbol tables, and headless benchmark assertion gates.
