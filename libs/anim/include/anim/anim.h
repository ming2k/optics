/*
 * anim/anim.h — the shared motion vocabulary (ADR-0077).
 *
 * Pure math on caller-owned state. No clock, no timeline, no scheduler,
 * no allocation, no globals: every entry point takes `dt` as a parameter
 * and every type is a plain struct the caller stores wherever it already
 * stores widget state (lens_skin_scratch, lens_node_state, host-side).
 *
 * Design contract (ADR-0077):
 *   - Public symbols are `anim_*`; internals are private.
 *   - `dt` is clamped once at the boundary to [0, 1/30] s; zero
 *     integrates nothing (a duplicated frame).
 *   - The spring advances by the closed-form analytic solution, so no
 *     accepted `dt` can make it diverge (the semi-implicit Euler class of
 *     bugs — observed in a consumer before its ADR-0139 — is structurally
 *     absent).
 *   - Reduced motion is one flag per advance call: when set, every
 *     primitive resolves to its end state in one step.
 *
 * What this library never decides: what moves, when it starts, or how
 * strong it is. Those stay with the caller (ADR-0047 / ADR-0074 item 3).
 */

#ifndef ANIM_H
#define ANIM_H

#include <stdbool.h>
#include <stdint.h>

#include <anim/export.h> /* ANIM_API — single source of truth */

#ifdef __cplusplus
extern "C" {
#endif

#define ANIM_VERSION_MAJOR 0
#define ANIM_VERSION_MINOR 0
#define ANIM_VERSION_PATCH 1

/* The largest delta time any primitive integrates over. A longer stall is
 * absorbed across subsequent frames instead of producing a teleport or a
 * divergence; the value matches the clamp every consumer of this
 * vocabulary converged on independently (1/30 s = two dropped 60 Hz
 * frames). */
#define ANIM_DT_MAX (1.0f / 30.0f)

/* Clamp `dt` into the integrable range. The single boundary every advance
 * entry point funnels through. */
ANIM_API float anim_dt_clamp(float dt_seconds);

/* ================================================================== */
/*  Spring — closed-form damped harmonic oscillator                   */
/* ================================================================== */

typedef struct anim_spring {
    float value;    /* current eased value in caller units */
    float velocity; /* current velocity in value units per second */
} anim_spring;

/* Tuning pair for `anim_spring_advance`. `stiffness` is ω₀² (rad/s)² —
 * larger shortens the period (snappier). `damping` is the damping ratio
 * ζ: 1.0 critically damped, just under 1.0 the slight overshoot that
 * reads as physical. ζ ≥ 1 falls back to the critically-damped branch. */
typedef struct anim_spring_params {
    float stiffness;
    float damping;
} anim_spring_params;

/* Named presets (the vocabulary the consumers share; constants live in
 * the .c so tuning is one place). */
ANIM_API anim_spring_params anim_spring_snappy(void);
ANIM_API anim_spring_params anim_spring_gentle(void);
ANIM_API anim_spring_params anim_spring_bouncy(void);

ANIM_API anim_spring anim_spring_at(float value);

/* True when the spring rests on `target` within the given tolerances and
 * can be dropped from the frame-cadence decision. */
ANIM_API bool anim_spring_settled(const anim_spring *s, float target,
                                  float value_epsilon, float velocity_epsilon);

/* Advance toward `target` by clamped `dt_seconds`; returns the new value.
 * `reduced_motion` resolves to the target in one step. */
ANIM_API float anim_spring_advance(anim_spring *s, float target,
                                   anim_spring_params p, float dt_seconds,
                                   bool reduced_motion);

/* One-step resolve (reduced motion, or an animation that must end now). */
ANIM_API float anim_spring_snap_to(anim_spring *s, float target);

/* ================================================================== */
/*  Approach / decay — exponential, frame-rate independent            */
/* ================================================================== */

/* Move `current` toward `target` by rate per second: after t seconds the
 * remaining distance is e^(−rate·t) of the original. `rate` ≤ 0 snaps. */
ANIM_API float anim_approach(float current, float target, float rate,
                             float dt_seconds);

/* Exponential decay toward zero (opacity tails, trailing values). */
ANIM_API float anim_decay(float value, float rate, float dt_seconds);

/* ================================================================== */
/*  Easing — normalized [0,1] curves                                  */
/* ================================================================== */

ANIM_API float anim_ease_out_cubic(float t);
ANIM_API float anim_ease_in_cubic(float t);
ANIM_API float anim_ease_in_out_cubic(float t);
ANIM_API float anim_ease_out_back(float t); /* slight overshoot, ends at 1 */

/* ================================================================== */
/*  Hysteresis — the de-jitter primitive for binary decisions        */
/* ================================================================== */

/* A Schmitt-trigger latch: the output flips only when the input crosses
 * the threshold *for the direction of travel* AND has held it for
 * `dwell_seconds`. This is the correct consumer for a binary polarity
 * fed by a temporally lagged, spatially averaged measurement (ADR-0065's
 * `plate_polarity`): interpolation is meaningless for a binary output,
 * and an unsmoothed threshold oscillates at the boundary. */
typedef struct anim_hysteresis {
    bool high;      /* current latched output state */
    float dwell;    /* seconds the candidate state has been held */
    bool candidate; /* the state the input is currently asking for */
} anim_hysteresis;

ANIM_API anim_hysteresis anim_hysteresis_init(bool initial_high);

/* Advance the latch. `input_high` is the raw comparator decision for this
 * frame (e.g. mean_luminance < 0.4). The output flips only after the
 * input has disagreed with the latched state for `dwell_seconds`
 * continuously; any agreement resets the dwell. `low_threshold` /
 * `high_threshold` define the dead band (they are the caller's comparator
 * bounds; passing the same value for both disables the band, leaving only
 * the dwell de-jitter). Returns the latched state. */
ANIM_API bool anim_hysteresis_step(anim_hysteresis *h, bool input_high,
                                   float low_threshold, float high_threshold,
                                   float measurement, float dwell_seconds,
                                   float dt_seconds);

/* ================================================================== */
/*  Smoother — critically-damped filter, motion-adaptive τ            */
/* ================================================================== */

/* A one-pole critically-damped filter whose time constant widens while
 * the input moves fast (absorbing measurement lag and noise) and narrows
 * at rest (converging quickly). For temporally lagged signals — prism
 * backdrop statistics are exactly FLUX_MAX_FRAMES_IN_FLIGHT frames stale
 * — this beats prediction: extrapolating a 3-frame-stale spatial mean
 * amplifies noise faster than it recovers latency. */
typedef struct anim_smoother {
    float value;
    float velocity; /* for the critically-damped follow */
} anim_smoother;

ANIM_API anim_smoother anim_smoother_init(float initial);

/* Advance toward `target`. `tau_rest` is the time constant when the
 * input is stationary; while |target − value| exceeds `motion_epsilon`
 * the filter uses tau_rest × `motion_scale`. */
ANIM_API float anim_smoother_step(anim_smoother *s, float target, float tau_rest,
                                  float motion_scale, float motion_epsilon,
                                  float dt_seconds);

#ifdef __cplusplus
}
#endif

#endif /* ANIM_H */
