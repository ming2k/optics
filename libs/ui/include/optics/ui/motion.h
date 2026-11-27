/*
 * ui/motion.h — closed-form UI motion vocabulary (ADR-0177).
 *
 * Pure math on caller-owned state. No clock, no timeline, no scheduler,
 * no allocation, no globals: every entry point takes `dt` explicitly and
 * every type is a plain value struct the caller stores alongside widget or
 * application state (`optics_ui_skin_scratch`, retained state, host structs).
 *
 * Header-only by design: all functions are `static inline` via
 * `OPTICS_UI_INLINE`, compiled directly into calling translation units with
 * zero dynamic-linking overhead. Only `<math.h>` (`libm`) is required, so
 * callers may include `<optics/ui/motion.h>` standalone or through
 * `<optics/ui.h>`.
 *
 * Design contract:
 *   - Public symbols use the `optics_ui_*` / `OPTICS_UI_*` namespace.
 *   - `dt` is clamped once at the boundary to `[0, 1/30]` s; zero or NaN
 *     integrates nothing.
 *   - Springs advance by the closed-form analytic solution, so no accepted
 *     `dt` can make them diverge (the semi-implicit Euler blow-up class is
 *     structurally absent). Near-critical damping (`zeta >= 1 - 1e-4`) uses
 *     the critically damped branch to avoid catastrophic cancellation in
 *     `sqrt(1 - zeta^2)`.
 *   - Sub-stepping uses a bounded integer step count so pathological
 *     `max_substep_dt` values can never stall or loop infinitely.
 *   - Reduced motion is one flag per advance call: when set, every
 *     primitive resolves to its target end state in a single step.
 */

#ifndef OPTICS_UI_MOTION_H
#define OPTICS_UI_MOTION_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <optics/ui/export.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest delta time any motion primitive integrates over (1/30 s). */
#define OPTICS_UI_MOTION_DT_MAX (1.0f / 30.0f)

/* Maximum sub-steps permitted in a single `optics_ui_spring_advance_substep` call. */
#define OPTICS_UI_MOTION_SUBSTEP_MAX_COUNT 64

/* Clamp `dt_seconds` into the integrable range [0, 1/30] s. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_motion_dt_clamp(float dt_seconds) {
    if (!(dt_seconds > 0.0f)) /* NaN and <= 0 integrate nothing */
        return 0.0f;
    return dt_seconds > OPTICS_UI_MOTION_DT_MAX ? OPTICS_UI_MOTION_DT_MAX : dt_seconds;
}

/* Internal: clamp normalized t to [0, 1], mapping NaN and <= 0 to 0. */
static inline float optics_ui__motion_clamp01(float t) {
    if (!(t > 0.0f))
        return 0.0f;
    return t >= 1.0f ? 1.0f : t;
}

/* ================================================================== */
/*  Spring — closed-form damped harmonic oscillator                   */
/* ================================================================== */

typedef struct optics_ui_spring {
    float value;    /* current eased value in caller units */
    float velocity; /* current velocity in value units per second */
} optics_ui_spring;

/* Tuning pair for `optics_ui_spring_advance`. `stiffness` is omega0^2 (rad/s)^2.
 * `damping` is the damping ratio zeta: 1.0 critically damped, <1.0 slight overshoot. */
typedef struct optics_ui_spring_params {
    float stiffness;
    float damping;
} optics_ui_spring_params;

/* Crisp, fast UI response spring preset (stiffness 480, damping 0.90). */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE optics_ui_spring_params optics_ui_spring_snappy(void) {
    optics_ui_spring_params p = {.stiffness = 480.0f, .damping = 0.90f};
    return p;
}

/* Soft, low-overshoot UI spring preset (stiffness 220, damping 0.80). */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE optics_ui_spring_params optics_ui_spring_gentle(void) {
    optics_ui_spring_params p = {.stiffness = 220.0f, .damping = 0.80f};
    return p;
}

/* Playful, under-damped spring preset (stiffness 480, damping 0.55). */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE optics_ui_spring_params optics_ui_spring_bouncy(void) {
    optics_ui_spring_params p = {.stiffness = 480.0f, .damping = 0.55f};
    return p;
}

/* Construct a spring at rest holding `value`. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE optics_ui_spring optics_ui_spring_at(float value) {
    optics_ui_spring s = {.value = value, .velocity = 0.0f};
    return s;
}

/* True when the spring rests on `target` within the given tolerances. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE bool optics_ui_spring_settled(const optics_ui_spring *s,
                                                                   float target,
                                                                   float value_epsilon,
                                                                   float velocity_epsilon) {
    if (!s)
        return false;
    return fabsf(s->value - target) <= value_epsilon && fabsf(s->velocity) <= velocity_epsilon;
}

/* One-step resolve (reduced motion, or an animation that must end now). */
OPTICS_UI_INLINE float optics_ui_spring_snap_to(optics_ui_spring *s, float target) {
    if (!s)
        return target;
    s->value = target;
    s->velocity = 0.0f;
    return target;
}

/* Advance toward `target` by clamped `dt_seconds`; returns the new value.
 * `reduced_motion` resolves to the target in one step. */
OPTICS_UI_INLINE float optics_ui_spring_advance(optics_ui_spring *s, float target,
                                                optics_ui_spring_params p, float dt_seconds,
                                                bool reduced_motion) {
    if (!s)
        return target;
    if (!isfinite(target)) /* NaN/Inf target = no valid target: state stands */
        return s->value;
    if (reduced_motion)
        return optics_ui_spring_snap_to(s, target);
    if (!isfinite(s->value) || !isfinite(s->velocity))
        return optics_ui_spring_snap_to(s, target);
    const float dt = optics_ui_motion_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return s->value;

    if (!isfinite(p.stiffness) || !(p.stiffness > 0.0f))
        return optics_ui_spring_snap_to(s, target);
    float omega0 = sqrtf(p.stiffness);
    if (!isfinite(omega0) || omega0 <= 0.0f)
        return optics_ui_spring_snap_to(s, target);
    float zeta = p.damping;
    if (!(zeta >= 0.0f)) /* NaN or negative -> critically damped */
        zeta = 1.0f;
    if (zeta > 1.0f)
        zeta = 1.0f;

    float x = s->value - target;
    if (zeta < 1.0f - 1e-4f) {
        /* Under-damped: damped oscillation, analytic for any dt.
         * Values within 1e-4 of 1.0 take the critically damped branch below
         * to prevent division by a near-zero `omega_d`. */
        float decay_rate = zeta * omega0;
        float omega_d = omega0 * sqrtf(1.0f - zeta * zeta);
        float decay = expf(-decay_rate * dt);
        float sin_ = sinf(omega_d * dt);
        float cos_ = cosf(omega_d * dt);
        float vterm = (s->velocity + decay_rate * x) / omega_d;
        s->value = target + decay * (x * cos_ + vterm * sin_);
        s->velocity = decay * (s->velocity * cos_ -
                               (decay_rate * s->velocity + omega0 * omega0 * x) / omega_d * sin_);
    } else {
        /* Critically damped (zeta >= 1 - 1e-4): smooth approach, no overshoot. */
        float decay = expf(-omega0 * dt);
        float vterm = s->velocity + omega0 * x;
        s->value = target + decay * (x + vterm * dt);
        s->velocity = decay * (s->velocity - omega0 * vterm * dt);
    }
    /* Non-finite guards: pathological inputs resolve cleanly to the target. */
    if (!isfinite(s->value))
        s->value = target;
    if (!isfinite(s->velocity))
        s->velocity = 0.0f;
    return s->value;
}

/* Advance toward `target` using bounded sub-steps (each <= max_substep_dt) to prevent
 * discrete jumps under irregular frame delivery; returns the new value.
 * If max_substep_dt <= 0, defaults to 1/60 s. Step count is capped at
 * OPTICS_UI_MOTION_SUBSTEP_MAX_COUNT to guarantee O(1) termination. */
OPTICS_UI_INLINE float optics_ui_spring_advance_substep(optics_ui_spring *s, float target,
                                                        optics_ui_spring_params p, float dt_seconds,
                                                        float max_substep_dt, bool reduced_motion) {
    if (!s)
        return target;
    if (!isfinite(target))
        return s->value;
    if (reduced_motion)
        return optics_ui_spring_snap_to(s, target);
    if (!(dt_seconds > 0.0f))
        return s->value;

    /* Cap total dt to 0.10s (100ms) to bound catch-up after long halts. */
    const float total = dt_seconds > 0.10f ? 0.10f : dt_seconds;
    float step =
        (max_substep_dt > 0.0f && isfinite(max_substep_dt)) ? max_substep_dt : (1.0f / 60.0f);
    if (step > OPTICS_UI_MOTION_DT_MAX)
        step = OPTICS_UI_MOTION_DT_MAX;
    const float min_step = total / (float)OPTICS_UI_MOTION_SUBSTEP_MAX_COUNT;
    if (step < min_step)
        step = min_step;

    int steps = (int)ceilf(total / step);
    if (steps < 1)
        steps = 1;
    if (steps > OPTICS_UI_MOTION_SUBSTEP_MAX_COUNT)
        steps = OPTICS_UI_MOTION_SUBSTEP_MAX_COUNT;

    const float sub_dt = total / (float)steps;
    for (int i = 0; i < steps; i++)
        optics_ui_spring_advance(s, target, p, sub_dt, false);
    return s->value;
}

/* ================================================================== */
/*  Approach / decay — exponential, frame-rate independent            */
/* ================================================================== */

/* Move `current` toward `target` by `rate` per second (frame-rate independent).
 * `reduced_motion` resolves to the target in one step. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_motion_approach(float current, float target,
                                                                     float rate, float dt_seconds,
                                                                     bool reduced_motion) {
    if (!isfinite(target)) /* NaN/Inf target: state stands */
        return current;
    if (reduced_motion || !isfinite(current))
        return target;
    if (!(rate > 0.0f) || !isfinite(rate))
        return target;
    float dt = optics_ui_motion_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return current;
    if (fabsf(target - current) < 1e-6f)
        return target;
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

/* Exponential decay toward zero (opacity tails, trailing values).
 * `reduced_motion` resolves to zero in one step. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float
optics_ui_motion_decay(float value, float rate, float dt_seconds, bool reduced_motion) {
    if (reduced_motion || !isfinite(value))
        return 0.0f;
    if (!(rate > 0.0f) || !isfinite(rate))
        return 0.0f;
    float dt = optics_ui_motion_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return value;
    return value * expf(-rate * dt);
}

/* ================================================================== */
/*  Easing — normalized [0,1] curves                                  */
/* ================================================================== */

/* Cubic ease-out over normalized t in [0, 1]. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_ease_out_cubic(float t) {
    float inv = 1.0f - optics_ui__motion_clamp01(t);
    return 1.0f - inv * inv * inv;
}

/* Cubic ease-in over normalized t in [0, 1]. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_ease_in_cubic(float t) {
    float c = optics_ui__motion_clamp01(t);
    return c * c * c;
}

/* Symmetric cubic ease-in-out over normalized t in [0, 1]. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_ease_in_out_cubic(float t) {
    float c = optics_ui__motion_clamp01(t);
    if (c < 0.5f)
        return 4.0f * c * c * c;
    float k = -2.0f * c + 2.0f;
    return 1.0f - (k * k * k) * 0.5f;
}

/* Slight overshoot ease-out over normalized t in [0, 1]. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_ease_out_back(float t) {
    float c = optics_ui__motion_clamp01(t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float d = c - 1.0f;
    return 1.0f + c3 * d * d * d + c1 * d * d;
}

/* Hermite smoothstep over normalized t in [0, 1]. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE float optics_ui_smoothstep(float t) {
    float c = optics_ui__motion_clamp01(t);
    return c * c * (3.0f - 2.0f * c);
}

/* ================================================================== */
/*  Hysteresis — Schmitt-trigger de-jitter latch for binary UI state  */
/* ================================================================== */

typedef struct optics_ui_hysteresis {
    bool high;      /* current latched output state */
    float dwell;    /* seconds the candidate state has been held */
    bool candidate; /* the state the input is currently asking for */
} optics_ui_hysteresis;

/* Construct a hysteresis latch initialized to `initial_high`. */
OPTICS_UI_NODISCARD OPTICS_UI_INLINE optics_ui_hysteresis
optics_ui_hysteresis_init(bool initial_high) {
    optics_ui_hysteresis h = {.high = initial_high, .dwell = 0.0f, .candidate = initial_high};
    return h;
}

/* Advance the latch. `reduced_motion` latches to the input's request in one
 * step, bypassing the dwell timer. */
OPTICS_UI_INLINE bool optics_ui_hysteresis_step(optics_ui_hysteresis *h, bool input_high,
                                                float low_threshold, float high_threshold,
                                                float measurement, float dwell_seconds,
                                                float dt_seconds, bool reduced_motion) {
    if (!h)
        return false;

    bool wants;
    if (low_threshold < high_threshold && measurement == measurement) {
        if (h->high) {
            wants = measurement >= low_threshold ? true : false;
        } else {
            wants = measurement > high_threshold ? true : false;
        }
    } else {
        wants = input_high;
    }

    if (reduced_motion) {
        /* Bypass the dwell/dead band entirely: latch to the request now. */
        h->high = wants;
        h->candidate = wants;
        h->dwell = 0.0f;
        return h->high;
    }

    float dt = optics_ui_motion_dt_clamp(dt_seconds);
    float dwell_req = dwell_seconds > 0.0f ? dwell_seconds : 0.0f;

    if (wants != h->high) {
        h->candidate = wants;
        h->dwell += dt;
        if (h->dwell >= dwell_req) {
            h->high = wants;
            h->dwell = 0.0f;
        }
    } else {
        h->dwell = 0.0f;
        h->candidate = wants;
    }
    return h->high;
}

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_MOTION_H */
