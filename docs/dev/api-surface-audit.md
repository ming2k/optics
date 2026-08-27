# Public API Surface Audit (2026-06)

Goal: keep the externally exposed interface set — C headers and Rust bindings —
**clean, coarse-grained and properly layered**, because the surface is read by
humans *and* AI agents; excess, disorder and legacy baggage directly translate
into cognitive load and wrong decisions.

Numbers below are from a full audit of every installed header and every
bindings crate. C layer symbol counts are approximate where headers mix
functions and macros.

## Current shape (facts)

| Layer | Installed headers | Public functions | Notes |
|---|---|---|---|
| flux (L1) | flux.h, core.h, math.h, vulkan.h, dmabuf.h, canvas.h, canvas_cpu.h, scene.h, compute.h, effect.h + flux-scene-graph/scene-graph.h + flux-text/text.h | ~230 | version coherent `0.0.28`; install set == include set, nothing incidental |
| lens (L2) | lens.h (~2,900 lines, 119 fns), icon.h | ~119 | version string hardcoded in `context.c:6`, drift risk vs `LENS_VERSION_*` |
| iris (L3) | iris.h, app.h, capability.h, cursor.h, theme.h, file_dialog.h, a11y.h, a11y_prefs.h, window.h | ~29 | no version macros at all |
| prism | prism.h, types.h, liquid_glass.h, 4 more | ~16 | no version macros at all |
| anim | anim.h | 18 | smallest, matches ADR-0077 exactly |
| Rust | flux-rs (8 crates), lens-rs (2), iris-r-s (2), prism-rs (2) | ~400 safe fns | hand-written wrappers, real RAII; no dead symbols; no vendored headers |

Verdict on the two questions asked:

1. **Is the exposure correct?** Structurally yes — install sets are deliberate,
   every conditional header maps to a meson feature, and `prism/effect.h`
   being public is by design (prism owns material *identity*, flux owns
   rendering *mechanism*, ADR-0063).
2. **Are there obsolete / unnecessary interfaces?** Yes, a bounded list. No
   sweeping problems, but there are concrete dead surface and drift items.

## Ranked findings

### A. Dead surface (zero external callers — remove or wire up)

| Symbol | Where | Evidence |
|---|---|---|
| `flux_canvas_draw_image_coverage(_sub)` | `canvas.h` | only definition in `canvas.c:784/793`; zero callers repo-wide (incl. bindings) |
| `flux_canvas_create_cpu_aa` | `canvas_cpu.h:61` | bound in flux-sys but never wrapped in `flux` crate, never called from C |
| `flux_canvas_end_frame_checked` | `canvas.h:288` | bound but unwrapped; Rust `Canvas::end` calls the *deprecated* `flux_canvas_end` |
| `lens_icon_table` / `lens_icon_count` | `icon.h` | exposed const table; only consumed inside `icon_data.c` itself |
| `lens_label_wrapped_ex` / `lens_label_compact_ex2` | `lens.h:1057/1062` | public but zero callers; `_ex2`/`_ex` families are legacy-suffix debt |
| `iris_a11y_unique_name` | `a11y.h` | implemented in both backends, zero callers |

Action: delete the C symbols (semver-free pre-1.0, removal is cheap now);
for deprecated canvas verbs keep `flux_canvas_begin_frame/end_frame(_checked)`
only and wrap the modern pair in Rust.

### B. Version/ABI drift (highest silent-risk)

1. `lens_version_string()` returns a hardcoded `"0.0.28"` in
   `context.c:6` instead of deriving from `LENS_VERSION_*` — guaranteed drift.
2. iris and prism ship **no** version macros at all; iris-rs pins header
   macros in a test (`iris/src/lib.rs:1382`), which is a good tripwire but a
   substitute for the C side having version macros.
3. No bindings `build.rs` calls `pkg_config::atleast_version(...)`. Stale
   comments claim `flux >= 0.0.13` / `>= 0.1.0` / `0.2.4` while truth is
   `0.0.` — nothing fails loudly when the C lib is older than the wrapper
   assumes.
4. Cargo versions (0.1.x) are unrelated to the C version (0.0.28). Acceptable
   convention (openssl-sys style), but it must be *documented* as such; today
   it is implicit.

### C. Rust binding hygiene (structural)

1. **Foreign-type duplication in sys crates.** `lens-sys`,
   `flux-text-sys`, `flux-scene-graph-sys`, `iris-sys` all allowlist
   `flux_.*` / `lens_.*` wholesale, producing 3–4 independent Rust
   definitions of the same C types per process, stitched by raw-pointer
   casts at crate seams. In-tree proof of divergence: two cached
   `flux-sys` builds disagree on whether `flux_canvas_create_cpu_aa`
   exists. Fix pattern already exists — `prism-sys/build.rs:207-210`
   (`blocklist_type("flux_.*")` + `raw_line` re-export from `flux-sys`);
   apply it to the other four sys crates.
2. **Wrapper targets deprecated verbs.** `Canvas::begin/end/end_checked`
   wrap the deprecated trio; the modern `begin_frame` exists but
   `end_frame(_checked)` is bound-but-unwrapped. Users of the safe crate
   currently *cannot* use the non-deprecated path.
3. **Misplaced / redundant crates in flux-rs.**
   - `flux-composition-graph` (1,474 LOC): zero flux dependency, no C
     counterpart under `libs/` — a planning library living in a bindings
     workspace, misleadingly `flux-`-prefixed.
   - `flux-text-layout` (1,289 LOC, 4 public functions): implements a layer
     the C side has reserved (`flux_text_layout` "not yet defined"); plus
     rpath-only sys dependencies as a hack.
   Both should move out of `bindings/` (either `crates/` at repo root or
   docs-only until C lands).
4. **Leaky safe APIs.** Safe fns returning raw pointers
   (`PaintHost::canvas()/device()` → `*mut c_void`; `Device::vk_*()` →
   raw `sys::Vk*`), bindgen types in public signatures
   (`flux::Format`, `lens::Color(pub sys::flux_color)`, `SkinFn`), and
   `pub use *_sys as sys` re-exports in all four wrappers.
5. **Inconsistent error model.** Four different `Error` shapes over one
   `flux_result` (`flux::Error` tuple struct, `flux_text::Error`,
   `flux_scene_graph::Error` + `LoadError`, `lens::Error` enum), while
   `prism` simply reuses `flux::Error`. Standardize on re-use or one
   shared error crate.
6. **Orphaned example** `lens-rs/examples/u2.rs` sits at a virtual-manifest
   workspace root and is never compiled.

### D. Layering (mostly correct, two nits)

- Correct by design: prism→flux effect seam (ADR-0063), iris→lens input
  feed, flux-text as lens's text seam (deliberately *not* installed by
  libflux), anim as a pure-math layer with no binding yet.
- Nit 1: `flux/effect.h` is public with exactly one consumer family
  (prism + showcase). Public-with-single-consumer is a legitimate seam, but
  it should be documented as such in the header ("intended for prism") or
  demoted behind `-Deffect` documentation.
- Nit 2: anim has no Rust binding while every sibling has one. If anim is
  to be consumed from Rust (lens-rs users will want springs), either add
  `anim-rs` or explicitly document "no binding by design".

### E. Naming / consistency nits (cheap wins)

- `_ex` / `_ex2` suffix families in lens (`lens_label_compact_ex2`) are
  positional-argument debt; prefer named-options structs going forward.
- iris cursor / theme / dialog headers are clean and small; keep them that
  way.
- Standardize deprecation policy: currently only flux has
  `FLUX_DEPRECATED`; lens/iris/prism have no deprecation machinery at all.

## Recommended order of operations

1. Delete the dead C symbols (A) — one mechanical commit, tests still pass.
2. Fix `lens_version_string()` to derive from macros (B1); add version
   macros to prism + iris (B2); add `atleast_version` to every bindings
   `build.rs` (B3) and document the Cargo-vs-C version convention (B4).
3. Port the prism-sys blocklist+re-export pattern to lens-sys, iris-sys,
   flux-text-sys, flux-scene-graph-sys (C1).
4. Wrap `end_frame(_checked)` / `create_cpu_aa` in `flux` crate and
   deprecate `Canvas::begin/end/end_checked` (C2).
5. Relocate `flux-composition-graph` / `flux-flux-text-layout` out of
   bindings/ (C3).
6. Then the longer tail: error model unification (C5), pointer leaks (C4),
   orphaned example (C6).

- Verified against `libs/iris/meson.build:187-198`: iris installs exactly
  iris.h, app.h, capability.h, cursor.h, theme.h, file_dialog.h, a11y.h,
  a11y_prefs.h, window.h — no stray headers.
- anim is fully standalone: no lens/iris/prism/flux dependency on it
  (`grep anim libs/*/meson.build` → only the root `meson.build` option and
  `tests/anim`); its sole consumer outside itself is `tests/anim/test_anim.c`.
  There is no anim-rs binding. If anim stays a leaf math library, this is
  correct and minimal; document it as "binding optional, math-only leaf".
