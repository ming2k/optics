# ADR-0077: The `anim` motion vocabulary library — provable math, host-owned clocks

- Status: Proposed
- Date: 2026-08-23
- Scope: new sibling library `libs/anim` (+ `anim-rs` binding); amends
  [ADR-0074](0074-effect-intake-path.md) item 3. No existing library links it.

## Context

The stack has answered "where does animation math live?" three times, the same
way each time: nowhere in the libraries, clocks stay host-owned
([ADR-0029](0029-animation-and-effect-policy.md) in Aegis, ADR-0047, ADR-0074).
The consumer side then proved what that costs: before its ADR-0139, the Aegis
compositor carried **three hand-rolled springs with divergent integrators and
three local variants of the reduced-motion rule**, one of which (semi-implicit
Euler) *could diverge on a long frame stall* — the exact failure mode a
copied recipe invites. Aegis fixed it internally with `aegis-ui::motion`, but
that crate is private to one consumer. Every other host — Optics' own
`examples/showcase` recipes, the aphrodite pixel editor built on lens-rs, any
third-party compositor — still starts from a pasted `.c` file with physics
constants in it.

Two structural facts make this a monorepo problem rather than a consumer
problem:

1. **The math being duplicated is mechanism, not policy.** How a scalar
   travels between two values under a stiffness/damping pair — the analytic
   damped-spring solution, exponential approach, a hysteresis latch — has no
   product opinion in it. What moves, when it starts, and how strong it is
   stays policy. ADR-0047's pixel test does not reach scalar math, so the
   boundary was never forced to answer for it.
2. **Recipes cannot carry proofs.** A copied `spring_integrate` comes with
   constants and a comment. A library comes with property tests: boundedness
   under any accepted `dt`, energy monotonicity, analytic exactness. The
   divergence class Aegis hit is *structurally absent* from a closed-form
   spring — and only a shared, tested library makes that guarantee travel to
   every host.

## Decision

1. **`libs/anim` ships the shared motion vocabulary as a new sibling leaf
   library** (`<anim/anim.h>`, C23, `anim_*` symbols, house export macro
   `ANIM_API` in `anim/export.h`, no dependency beyond libm):
   - `anim_spring` — damped harmonic oscillator advanced by the **closed-form
     analytic solution** (under-damped and critically-damped branches), so a
     long frame stall cannot diverge for any `dt` the contract accepts;
   - `anim_approach` / `anim_decay` — exponential approach/decay helpers;
   - `anim_ease_*` — the standard easing vocabulary;
   - `anim_hysteresis` — a Schmitt-trigger latch with independently
     configurable rise/fall thresholds and a minimum dwell, the correct
     de-jitter primitive for **binary** decisions fed by noisy measurements
     (see ADR-0065's `plate_polarity`);
   - `anim_smoother` — critically-damped smoothing with a **motion-adaptive
     time constant** (long τ while the input moves fast, short τ at rest),
     for continuous signals that are temporally lagged (prism backdrop
     statistics are exactly 3 frames stale by construction);
   - reduced-motion is one flag on each advance entry point: when set, every
     primitive resolves to its end state in one step (the Aegis ADR-0029
     rule, stated once instead of per-consumer).

2. **The clock never enters.** No entry point reads a clock; `dt` is always a
   parameter, clamped once at the library boundary to `[0, 1/30]` s (zero
   integrates nothing — a duplicated frame). There is no timeline, no
   scheduler, no callback, no allocation, and no global state: every type is
   a plain `Copy`-shaped struct the caller owns and stores wherever it
   already stores widget state (`lens_skin_scratch`, `lens_node_state`,
   host-side).

3. **ADR-0074 item 3 is narrowed, not reversed.** "Choreography never enters
   Optics" keeps its meaning — *what* moves, *when* it starts, *how strong*
   it is, stays out. What the amendment admits is the *scalar-travel
   vocabulary* as a leaf dependency no rendering library links: `anim` has no
   knowledge of pixels, images, command buffers, widgets, or ids, and flux /
   lens / prism / iris do not depend on it. Consumers (`aegis-ui::motion`
   re-exports it; showcase recipes are rewritten on it) link it; the four
   rendering layers never do.

4. **Property tests are part of the contract** (`tests/anim/`): boundedness
   of every primitive over adversarial `dt` (including NaN injection), energy
   non-increase for the spring, exactness of the analytic solution against
   the integral, hysteresis dwell/width behaviour, smoother convergence and
   its motion-adaptive τ profile.

5. **The showcase recipes are rewritten on `anim`** so the copyable flavor
   keeps its ADR-0061 role but stops being a physics implementation: a copied
   recipe now copies *tuning constants and geometry*, never an integrator.

## Alternatives Considered

- **Keep recipes-only (status quo).** Reject: Aegis already demonstrated the
  outcome — three divergent integrators and a diverging one — and private
  fixes do not travel. The divergence bug class survives every copy.
- **Put the math in lens.** Reject: lens is a rendering/UI library; linking
  it drags Vulkan-adjacent dependencies into hosts that want one spring,
  and ADR-0061's "mechanism, never flavor" scoping would be re-litigated for
  every added curve. A leaf sibling keeps the dependency arrow one-way and
  optional.
- **Put the math in flux.** Reject: flux's charter is GPU mechanism; scalar
  math is not pixel-touching, and ADR-0074's own test ("needs to touch
  pixels, images, or command buffers") places it outside.
- **Adopt a third-party tween/easing crate.** Reject: every candidate owns a
  clock or a timeline; the host-owned-clock invariant is the one decision all
  three prior ADRs agree on.
- **Aegis-only (leave `aegis-ui::motion` as-is).** Reject: it leaves every
  non-Aegis host back at copy-paste, which is the gap this ADR exists to
  close.

## Consequences

- New library surface to maintain: one header, ~200 lines of pure math, one
  meson target, one pkg-config file, and an `anim-rs` binding crate. The
  cost is bounded and the vocabulary is closed (adding a curve is additive).
- `aegis-ui::motion` becomes a thin re-export with its ADR-0139 rationale
  intact; Aegis PRs stop carrying physics review load.
- ADR-0074's intake checklist gains one line: an effect operator whose
  parameters are animated *must* express the animation with `anim`
  primitives (or prove why not), the way blur sigma already expresses
  per-frame variation.
- Recipes that copied integrators (tabs_spring_skin.c) lose their "yours to
  tune" physics; the constants remain the caller's.
- `docs/reference/symbols.md` and `docs/reference/anim.md` gain the table;
  the docs install manifest registers the new reference page.

## References

- [ADR-0047](0047-caller-owned-policy-boundary-for-flux-effects.md) — the
  mechanism/policy boundary this library lives inside.
- [ADR-0074](0074-effect-intake-path.md) — item 3 as amended by this ADR.
- [ADR-0065](0065-per-group-overrides-and-backdrop-stats.md) — the 3-frame
  stale statistics whose correct consumers (hysteresis + adaptive smoothing)
  this vocabulary supplies.
- Aegis ADR-0139 — the consumer-side mirror that proved the cost of
  recipes-only.
