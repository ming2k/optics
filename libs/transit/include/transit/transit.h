/*
 * transit/transit.h — the shared state transition and motion vocabulary (ADR-0077, ADR-0106).
 *
 * Pure math on caller-owned state. No clock, no timeline, no scheduler,
 * no allocation, no globals: every entry point takes `dt` as a parameter
 * and every type is a plain struct the caller stores wherever it already
 * stores widget state (lens_skin_scratch, lens_node_state, host-side).
 *
 * Library position: transit is a standalone leaf with no dependency on — and
 * no consumer inside — flux, lens, iris, or prism. Applications (and any
 * Rust host) link it directly for springs/easing; nothing in the stack
 * below it requires it.
 *
 * Design contract (ADR-0077, ADR-0106):
 *   - Public symbols are `transit_*`; internals are private.
 *   - `dt` is clamped once at the boundary to [0, 1/30] s; zero
 *     integrates nothing (a duplicated frame).
 *   - The spring advances by the closed-form analytic solution, so no
 *     accepted `dt` can make it diverge (the semi-implicit Euler class of
 *     bugs is structurally absent).
 *   - Reduced motion is one flag per advance call: when set, every
 *     primitive resolves to its end state in one step.
 *
 * What this library never decides: what moves, when it starts, or how
 * strong it is. Those stay with the caller.
 */

#ifndef TRANSIT_H
#define TRANSIT_H

#include <stdbool.h>
#include <stdint.h>

#include <transit/export.h> /* TRANSIT_API — single source of truth */

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSIT_VERSION_MAJOR 0
#define TRANSIT_VERSION_MINOR 0
#define TRANSIT_VERSION_PATCH 51

/* Packed integer version, monotonic — identical layout to
 * FLUX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7).
 * The whole stack shares one versioning scheme so a consumer can check
 * every library it loads the same way. */
#define TRANSIT_VERSION_NUMBER                                                                     \
    (((uint32_t)TRANSIT_VERSION_MAJOR << 16) | ((uint32_t)TRANSIT_VERSION_MINOR << 8) |            \
     (uint32_t)TRANSIT_VERSION_PATCH)

/* Version accessors. */
TRANSIT_API void transit_version(int *major, int *minor, int *patch);
TRANSIT_API uint32_t transit_version_number(void);
TRANSIT_API bool transit_version_check(int major, int minor, int patch);
TRANSIT_API const char *transit_version_string(void);

/* The largest delta time any primitive integrates over (1/30 s). */
#define TRANSIT_DT_MAX (1.0f / 30.0f)

/* Clamp `dt` into the integrable range. */
TRANSIT_API float transit_dt_clamp(float dt_seconds);

/* ================================================================== */
/*  Spring — closed-form damped harmonic oscillator                   */
/* ================================================================== */

typedef struct transit_spring {
    float value;    /* current eased value in caller units */
    float velocity; /* current velocity in value units per second */
} transit_spring;

/* Tuning pair for `transit_spring_advance`. `stiffness` is ω₀² (rad/s)².
 * `damping` is the damping ratio ζ: 1.0 critically damped, <1.0 slight overshoot. */
typedef struct transit_spring_params {
    float stiffness;
    float damping;
} transit_spring_params;

/* Named presets. */
TRANSIT_API transit_spring_params transit_spring_snappy(void);
TRANSIT_API transit_spring_params transit_spring_gentle(void);
TRANSIT_API transit_spring_params transit_spring_bouncy(void);

TRANSIT_API transit_spring transit_spring_at(float value);

/* True when the spring rests on `target` within the given tolerances. */
TRANSIT_API bool transit_spring_settled(const transit_spring *s, float target, float value_epsilon,
                                       float velocity_epsilon);

/* Advance toward `target` by clamped `dt_seconds`; returns the new value.
 * `reduced_motion` resolves to the target in one step. */
TRANSIT_API float transit_spring_advance(transit_spring *s, float target, transit_spring_params p,
                                        float dt_seconds, bool reduced_motion);

/* One-step resolve (reduced motion, or an animation that must end now). */
TRANSIT_API float transit_spring_snap_to(transit_spring *s, float target);

/* ================================================================== */
/*  Approach / decay — exponential, frame-rate independent            */
/* ================================================================== */

/* Move `current` toward `target` by rate per second. */
TRANSIT_API float transit_approach(float current, float target, float rate, float dt_seconds);

/* Exponential decay toward zero (opacity tails, trailing values). */
TRANSIT_API float transit_decay(float value, float rate, float dt_seconds);

/* ================================================================== */
/*  Easing — normalized [0,1] curves                                  */
/* ================================================================== */

TRANSIT_API float transit_ease_out_cubic(float t);
TRANSIT_API float transit_ease_in_cubic(float t);
TRANSIT_API float transit_ease_in_out_cubic(float t);
TRANSIT_API float transit_ease_out_back(float t);

/* ================================================================== */
/*  Hysteresis — the de-jitter primitive for binary decisions        */
/* ================================================================== */

typedef struct transit_hysteresis {
    bool high;      /* current latched output state */
    float dwell;    /* seconds the candidate state has been held */
    bool candidate; /* the state the input is currently asking for */
} transit_hysteresis;

TRANSIT_API transit_hysteresis transit_hysteresis_init(bool initial_high);

TRANSIT_API bool transit_hysteresis_step(transit_hysteresis *h, bool input_high,
                                        float low_threshold, float high_threshold,
                                        float measurement, float dwell_seconds,
                                        float dt_seconds);

/* ================================================================== */
/*  Smoother — critically-damped filter, motion-adaptive τ            */
/* ================================================================== */

typedef struct transit_smoother {
    float value;
    float velocity; /* for the critically-damped follow */
} transit_smoother;

TRANSIT_API transit_smoother transit_smoother_init(float initial);

TRANSIT_API float transit_smoother_step(transit_smoother *s, float target, float tau_rest,
                                       float motion_scale, float motion_epsilon,
                                       float dt_seconds);

#ifdef __cplusplus
}
#endif

#endif /* TRANSIT_H */
