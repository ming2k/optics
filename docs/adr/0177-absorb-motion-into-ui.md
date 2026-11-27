---
id: ADR-0177
title: "Absorb Motion Vocabulary into UI — Five-Library Stack, Unified Easing, and Zero-Ceremony Header"
status: accepted
date: 2026-11-27
scope: [ui, motion, build, rust, docs]
superseded_by: null
negative_knowledge: true
---

# ADR-0177: Absorb Motion Vocabulary into `ui` — Five-Library Stack, Unified Easing, and Zero-Ceremony Header

- Status: Accepted
- Date: 2026-11-27
- Deciders: Optics Architecture Working Group
- Consulted: UI engine (`ui`), Rust bindings, build plumbing, documentation
- Informed: Core Engineering
- Supersedes: [ADR-0077](0077-anim-motion-vocabulary-library.md), [ADR-0158](0158-transit-header-only-and-zero-so-motion-vocabulary.md)
- Amends: [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md), [ADR-0174](0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md), [ADR-0176](0176-retire-rust-optics-facade-crate.md)

---

## Context and Problem Statement

After [ADR-0174](0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md) retired `svg`, `scene`, and `material`, `motion` remained as a disconnected sixth top-level library:

```text
app ──┬──► window ───────────────► gfx
      └──► ui ────► text ────────► gfx
motion   (header-only; libm only)
```

Auditing the tree exposed four structural problems with keeping `motion` as a peer top-level library:

1. **Severe ceremony-to-substance ratio**: The implementation was ~400 lines of stateless inline math, yet it carried the full top-level distribution surface of a major library — `libs/motion/meson.build`, `optics-motion.pc`, `-Dmotion` build option, `OPTICS_HAVE_MOTION` umbrella flag, a dedicated `no-motion` CI matrix leg, independent `optics_motion_version*` accessors, and a standalone `crates/optics-motion` crate.
2. **Zero internal consumers and duplicated easing**: No library under `libs/` included `<optics/motion.h>`. Meanwhile `libs/ui/src/internal.h` maintained its own private `optics_ui__approach` using a frame-rate-dependent linear step (`dt * rate`) instead of the frame-rate-independent exponential approach (`1 - exp(-rate * dt)`).
3. **Manual C/Rust version and implementation drift**: Because `crates/optics-motion` duplicated version constants and math by hand without C FFI, its `VERSION_PATCH` had already drifted (`0.0.82` vs `0.0.90` in `libs/motion/include/optics/motion.h`).
4. **Retired material-era relic**: `optics_motion_smoother` (`motion_scale` / `motion_epsilon`) existed specifically to damp the 3-frame-lagged backdrop statistics of the retired `prism`/`material` library (ADR-0077). In a pure UI motion vocabulary, critically damped springs (`damping = 1.0f`) and exponential approach already cover continuous smoothing without backdrop-specific parameters.

---

## Decision

Optics converges on a clean, connected **five-library one-way DAG**:

```text
app ──┬──► window ───────────────► gfx
      └──► ui ────► text ────────► gfx
```

1. **Absorb motion into `libs/ui` as `<optics/ui/motion.h>`**:
   - All motion primitives move into the `optics_ui_*` / `OPTICS_UI_*` namespace and live in a self-contained, header-only file (`OPTICS_UI_INLINE` = `static inline` in `<optics/ui/export.h>`, requiring only `libm`).
   - `<optics/ui.h>` includes `<optics/ui/motion.h>` automatically; callers that want motion math without pulling in rendering headers may also `#include <optics/ui/motion.h>` directly.
   - `ui_dep` and `optics-ui.pc` link `m_dep` (`-lm`) publicly so inline calls to `expf`/`sinf`/`cosf`/`sqrtf`/`powf` always resolve.
   - The retained vocabulary covers `optics_ui_motion_dt_clamp`, `optics_ui_spring*` (analytic closed-form oscillator + `advance_substep` + presets), `optics_ui_motion_approach`, `optics_ui_motion_decay`, `optics_ui_ease_*` / `optics_ui_smoothstep`, and `optics_ui_hysteresis*`.
   - `smoother` (the retired `prism` backdrop-lag filter) and the redundant `optics_motion_version*` accessors are removed cleanly with no compatibility shims.

2. **Dogfood `<optics/ui/motion.h>` inside `libs/ui`**:
   - `optics_ui__approach` in `libs/ui/src/internal.h` delegates directly to `optics_ui_motion_approach`, giving every built-in widget hover/active transition true frame-rate-independent exponential motion (`1 - exp(-rate * dt)`) while preserving ADR-0152's terminal snap (`OPTICS_UI_ARRIVE_EPS`) and `ui->anim_pending` quiescence tracking.

3. **Retire `libs/motion`, `-Dmotion`, `OPTICS_HAVE_MOTION`, and `crates/optics-motion`**:
   - Remove `libs/motion/`, `tests/motion/`, `crates/optics-motion/`, the Meson `motion` option, `OPTICS_HAVE_MOTION`, and the `no-motion` CI matrix leg.
   - Move unit property tests to `tests/ui/test_motion.c` (including `<optics/ui/motion.h>` standalone to keep the header self-contained).
   - Provide the pure Safe Rust mirror as `optics_ui::motion` inside `crates/optics-ui/src/motion.rs` (zero FFI overhead, zero independent version constants; `optics-ui-sys` blocklists the inline motion symbols).

4. **Mechanical enforcement**:
   - `tools/check_library_purity.py` rejects `libs/motion` alongside retired `libs/svg`, `libs/scene`, and `libs/material`.
   - `tools/check_versions.py` rejects `crates/optics-motion`.
   - `tools/gen_symbols.py` scans both `OPTICS_<MOD>_API` and `OPTICS_<MOD>_INLINE` so `<optics/ui/motion.h>` stays mechanically synchronized with `docs/reference/symbols.md`.

---

## Consequences

### Positive
- Eliminates an isolated top-level library, a build option, a `pkg-config` file, a CI matrix job, and an extra Rust crate.
- Achieves true 1:1 symmetry: five C libraries under `libs/` (`gfx`, `text`, `window`, `ui`, `app`), five safe Rust crates under `crates/`.
- Built-in `liboptics_ui` widget transitions are now genuinely frame-rate independent across 60 Hz, 120 Hz, and variable-refresh displays, backed by the same tested math exposed to applications.
- Eliminates version drift between C and Rust motion code permanently.

### Negative & Trade-offs
- Consumers that want motion primitives without `optics-ui` on Rust must either depend on `optics-ui` or supply their own math; C consumers can still `#include <optics/ui/motion.h>` standalone with zero link dependency beyond `libm`.
- Pre-1.0 clean break: callers migrating from `<optics/motion.h>` / `optics_motion_*` / `optics-motion` update includes and prefixes to `<optics/ui/motion.h>` (or `<optics/ui.h>`) / `optics_ui_*` / `optics_ui::motion`.

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep `libs/motion` as a standalone sixth top-level library
- **Why rejected**: Permanent build/CI/packaging/versioning tax for ~400 lines of inline arithmetic that zero internal libraries consumed and whose Rust crate had already drifted in version.

### 2. Move motion into `libs/gfx` (`<optics/gfx/motion.h>`)
- **Why rejected**: `gfx` owns GPU/CPU spatial geometry and rendering mechanisms, whereas Optics' motion contract centers on UI interaction semantics — `reduced_motion` accessibility resolution, state-transition latches (`hysteresis`), and convergence toward UI quiescence (`anim_pending`). Placing it in `ui` keeps `gfx` strictly spatial and lets `ui` dogfood its own primitives cleanly.

---

## Links

- Supersedes: [ADR-0077](0077-anim-motion-vocabulary-library.md), [ADR-0158](0158-transit-header-only-and-zero-so-motion-vocabulary.md)
- Amends: [ADR-0166](0166-one-way-library-graph-and-enforced-invariants.md), [ADR-0170](0170-semantic-module-naming-and-optics-namespace.md), [ADR-0174](0174-pure-graphics-ui-core-retire-format-parsers-3d-and-materials.md), [ADR-0176](0176-retire-rust-optics-facade-crate.md)
