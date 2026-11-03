---
id: ADR-0142
title: "Prism-rs safe descriptor modernization, material warmup options, and caller-declared revision caching"
status: accepted
date: 2026-11-03
superseded_by: null
scope: [prism, bindings, architecture]
negative_knowledge: true
---

# ADR-0142: Prism-rs safe descriptor modernization, material warmup options, and caller-declared revision caching

- Status: Accepted
- Date: 2026-11-03
- Scope: `bindings/prism-rs/crates/prism/src/lib.rs`, `bindings/prism-rs/crates/prism/tests/glass.rs`, `bindings/prism-rs/crates/prism/tests/materials.rs`.
- Implementation: Landed. Modernizes `bindings/prism-rs` to conform to `[INV-ARCH-OPTS]` (Single Opts Descriptor Paradigm). Exposes typed `WarmupOpts` across all Prism physical material filters (`LiquidGlassFilter`, `BackdropLayerFilter`, `FrostedFilter`, `AcrylicFilter`), introduces `BackdropLayerApplyOpts` with structured caller-declared `revision` caching (`prism_backdrop_layer_revision`, ADR-0137), and eliminates legacy multi-parameter apply signatures.
- Depends on: [ADR-0141](0141-material-compute-pipeline-prewarming-and-headless-performance-governance.md), [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md), [ADR-0139](0139-footprint-sized-material-slabs-and-batched-dispatch.md), [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md).

---

## Context and Problem Statement

ADR-0141 introduced typed C23 warmup descriptors (`prism_*_warmup_opts`) across all five physical surface materials in `prism` to eradicate cold-start compute pipeline compilation hitches (~13.56 ms down to 0.55 ms). ADR-0137 introduced caller-declared content revisions (`prism_backdrop_layer_revision`) to skip compute passes entirely when backdrop scene sources and layer parameters are static across frame slots.

However, the Rust language bindings under `bindings/prism-rs` lagged behind these architectural advances:

1. **Unconfigurable Warmup FFI**: `BackdropLayerFilter::warmup`, `LiquidGlassFilter::warmup`, and other filter warmup methods hardcoded `std::ptr::null()`, defaulting to RGBA8 UNORM. Compositors and high-fidelity UI runtimes utilizing RGBA16F (HDR or wide-gamut) experienced unexpected JIT hitching on their first frame despite calling `warmup()`.
2. **Missing Revision Caching in Safe Rust**: `prism_backdrop_layer_revision` was not exposed in `prism-rs`. Rust consumers (such as the Tessera compositor) had no mechanism to pass content revision tags through `desc.next`, preventing them from activating Prism's zero-dispatch slot cache.
3. **Parameter Proliferation & Invariant Violation**: `BackdropLayerFilter::apply` had accumulated 7 loose arguments, requiring clippy suppressions (`#[allow(clippy::too_many_arguments)]`) and violating the Optics Single Opts Descriptor Paradigm (`[INV-ARCH-OPTS]`).

## Decision Drivers

1. **Strict Invariant Adherence (`[INV-ARCH-OPTS]`)**: Clean-break modernization. Every functional entry point must accept exactly one typed descriptor. Dual API tiers (`apply_ex`, `apply_layered`) and parameter sprawl are prohibited.
2. **Target Format Fidelity**: Prewarming must accept optional target image formats (`flux::Format`) and reduction statistics options matching C descriptors.
3. **Cache Key Semantics**: Expose `revision: Option<u64>` where `Some(rev)` chains `prism_backdrop_layer_revision` to activate slot output reuse, and `None` / `Some(0)` safely re-renders.

---

## Architectural Decisions

### 1. Typed `WarmupOpts` Across Physical Material Filters

`prism-rs` defines a typed `WarmupOpts` descriptor:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WarmupOpts {
    pub format: flux::Format,
    pub stats: bool,
}

impl Default for WarmupOpts {
    fn default() -> Self {
        Self {
            format: flux::Format::Rgba8Unorm,
            stats: true,
        }
    }
}
```

All material filters (`LiquidGlassFilter`, `BackdropLayerFilter`, `FrostedFilter`, `AcrylicFilter`) update their `.warmup` method:

```rust
pub fn warmup(&mut self, opts: Option<&WarmupOpts>) -> Result<(), Error>;
```

Passing `None` safely invokes the C defaults (`nullptr`), while `Some(&opts)` maps the target format and stats reduction flag down to the underlying C descriptor.

### 2. Structured `BackdropLayerApplyOpts` and Revision Extension

`BackdropLayerFilter::apply` is refactored from loose parameters into a unified descriptor:

```rust
pub struct BackdropLayerApplyOpts<'a, 'frame> {
    pub frame: &'a flux::Frame<'frame>,
    pub input: &'a flux::Image,
    pub blurred: &'a flux::BlurredImage<'a>,
    pub frost: &'a [BackdropFrost],
    pub groups: &'a [LiquidGlassGroup],
    pub params: LiquidGlassParams,
    pub revision: Option<u64>,
}
```

When `opts.revision` is `Some(rev)`, the binding populates a `sys::prism_backdrop_layer_revision` structure (`PRISM_TYPE_BACKDROP_LAYER_REVISION = 6`) and chains it to `desc.next`. If the slot's cached revision matches `rev`, Prism's compute passes are completely elided and the persistent slab output is returned in $< 1\ \mu\text{s}$.

The redundant and non-standard `apply_layered` method is eradicated.

---

## Consequences

- **Zero-Allocation Slot Reuse**: Downstream Rust compositors can now achieve zero GPU compute dispatches on static frames by passing their content generation ID as `revision`.
- **Accurate Pipeline Prewarming**: Runtimes using FP16 / HDR pipelines can explicitly prewarm their format, eliminating cold-start frame drops.
- **Breaking Clean-Break**: Callers of `BackdropLayerFilter::apply` must update from loose parameters to `BackdropLayerApplyOpts`. No legacy shims or parameter suffixes are retained.

## References

- [ADR-0141](0141-material-compute-pipeline-prewarming-and-headless-performance-governance.md) — Material compute pipeline pre-warming and cold-start hitch elimination.
- [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md) — Caller-declared content revisions for prism backdrop-layer caching.
- [ADR-0082](0082-lens-minimal-orthogonal-components-and-single-descriptor-api.md) — Single Opts Descriptor API Paradigm.
