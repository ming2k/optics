/*
 * transit/transit.h — the shared state transition and motion vocabulary (ADR-0077, ADR-0106).
 *
 * Pure math on caller-owned state. No clock, no timeline, no scheduler,
 * no allocation, no globals: every entry point takes `dt` as a parameter
 * and every type is a plain struct the caller stores wherever it already
 * stores widget state (lens_skin_scratch, lens_node_state, host-side).
 *
 * Header-only design: transit is a standalone header-only vocabulary.
 * All public symbols are `static inline` functions compiled directly into
 * the consumer TU. It produces no `.so` and carries zero dynamic linking tax.
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

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <transit/export.h> /* TRANSIT_API — static inline */

#ifdef __cplusplus
extern "C" {
#endif

#define TRANSIT_VERSION_MAJOR 0
#define TRANSIT_VERSION_MINOR 0
#define TRANSIT_VERSION_PATCH 78

/* Packed integer version, monotonic — identical layout to
 * FLUX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7). */
#define TRANSIT_VERSION_NUMBER                                                                     \
    (((uint32_t)TRANSIT_VERSION_MAJOR << 16) | ((uint32_t)TRANSIT_VERSION_MINOR << 8) |            \
     (uint32_t)TRANSIT_VERSION_PATCH)

#define TRANSIT_STR2(x) #x
#define TRANSIT_STR(x) TRANSIT_STR2(x)

/* Version accessors. */
TRANSIT_API void transit_version(int *major, int *minor, int *patch) {
    if (major)
        *major = TRANSIT_VERSION_MAJOR;
    if (minor)
        *minor = TRANSIT_VERSION_MINOR;
    if (patch)
        *patch = TRANSIT_VERSION_PATCH;
}

TRANSIT_API uint32_t transit_version_number(void) {
    return TRANSIT_VERSION_NUMBER;
}

TRANSIT_API bool transit_version_check(int major, int minor, int patch) {
    if (major != TRANSIT_VERSION_MAJOR)
        return false;
    if (minor > TRANSIT_VERSION_MINOR)
        return false;
    if (minor == TRANSIT_VERSION_MINOR && patch > TRANSIT_VERSION_PATCH)
        return false;
    return true;
}

TRANSIT_API const char *transit_version_string(void) {
    return TRANSIT_STR(TRANSIT_VERSION_MAJOR) "." TRANSIT_STR(
        TRANSIT_VERSION_MINOR) "." TRANSIT_STR(TRANSIT_VERSION_PATCH);
}

/* The largest delta time any primitive integrates over (1/30 s). */
#define TRANSIT_DT_MAX (1.0f / 30.0f)

/* Clamp `dt` into the integrable range. */
TRANSIT_API float transit_dt_clamp(float dt_seconds) {
    if (!(dt_seconds > 0.0f)) /* NaN and <= 0 integrate nothing */
        return 0.0f;
    return dt_seconds > TRANSIT_DT_MAX ? TRANSIT_DT_MAX : dt_seconds;
}

/* ================================================================== */
/*  Spring — closed-form damped harmonic oscillator                   */
/* ================================================================== */

typedef struct transit_spring {
    float value;    /* current eased value in caller units */
    float velocity; /* current velocity in value units per second */
} transit_spring;

/* Tuning pair for `transit_spring_advance`. `stiffness` is omega0^2 (rad/s)^2.
 * `damping` is the damping ratio zeta: 1.0 critically damped, <1.0 slight overshoot. */
typedef struct transit_spring_params {
    float stiffness;
    float damping;
} transit_spring_params;

/* Named presets. */
TRANSIT_API transit_spring_params transit_spring_snappy(void) {
    return (transit_spring_params){.stiffness = 480.0f, .damping = 0.90f};
}

TRANSIT_API transit_spring_params transit_spring_gentle(void) {
    return (transit_spring_params){.stiffness = 220.0f, .damping = 0.80f};
}

TRANSIT_API transit_spring_params transit_spring_bouncy(void) {
    return (transit_spring_params){.stiffness = 480.0f, .damping = 0.55f};
}

TRANSIT_API transit_spring transit_spring_at(float value) {
    transit_spring s = {.value = value, .velocity = 0.0f};
    return s;
}

/* True when the spring rests on `target` within the given tolerances. */
TRANSIT_API bool transit_spring_settled(const transit_spring *s, float target, float value_epsilon,
                                        float velocity_epsilon) {
    if (!s)
        return false;
    return fabsf(s->value - target) <= value_epsilon && fabsf(s->velocity) <= velocity_epsilon;
}

/* One-step resolve (reduced motion, or an animation that must end now). */
TRANSIT_API float transit_spring_snap_to(transit_spring *s, float target) {
    if (!s)
        return target;
    s->value = target;
    s->velocity = 0.0f;
    return target;
}

/* Advance toward `target` by clamped `dt_seconds`; returns the new value.
 * `reduced_motion` resolves to the target in one step. */
TRANSIT_API float transit_spring_advance(transit_spring *s, float target, transit_spring_params p,
                                         float dt_seconds, bool reduced_motion) {
    if (!s)
        return target;
    if (target != target) /* NaN target = no target: state stands */
        return s->value;
    if (reduced_motion)
        return transit_spring_snap_to(s, target);
    const float dt = transit_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return s->value;

    float omega0sq = p.stiffness > 0.0f ? p.stiffness : 0.0f;
    float omega0 = sqrtf(omega0sq);
    if (omega0 <= 0.0f)
        return transit_spring_snap_to(s, target);
    float zeta = p.damping;
    if (!(zeta >= 0.0f)) /* NaN -> critically damped */
        zeta = 1.0f;
    if (zeta > 1.0f)
        zeta = 1.0f;

    float x = s->value - target;
    if (zeta < 1.0f) {
        /* Under-damped: damped oscillation, analytic for any dt. */
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
        /* Critically damped (zeta >= 1 clamped): smooth approach, no overshoot. */
        float decay = expf(-omega0 * dt);
        float vterm = s->velocity + omega0 * x;
        s->value = target + decay * (x + vterm * dt);
        s->velocity = decay * (s->velocity - omega0 * vterm * dt);
    }
    /* NaN guards: a caller feeding pathological inputs gets the target,
     * not a persistent NaN. */
    if (!(s->value == s->value))
        s->value = target;
    if (!(s->velocity == s->velocity))
        s->velocity = 0.0f;
    return s->value;
}

/* Advance toward `target` using bounded sub-steps (each <= max_substep_dt) to prevent
 * discrete jumps under irregular frame delivery; returns the new value.
 * If max_substep_dt <= 0, defaults to 1/60 s. */
TRANSIT_API float transit_spring_advance_substep(transit_spring *s, float target,
                                                 transit_spring_params p, float dt_seconds,
                                                 float max_substep_dt, bool reduced_motion) {
    if (!s)
        return target;
    if (reduced_motion)
        return transit_spring_snap_to(s, target);
    if (target != target)
        return s->value;
    if (!(dt_seconds > 0.0f))
        return s->value;

    /* Cap total dt to 0.10s (100ms) to prevent divergence on long halts */
    float remaining = dt_seconds > 0.10f ? 0.10f : dt_seconds;
    float step = (max_substep_dt > 0.0f) ? max_substep_dt : (1.0f / 60.0f);
    if (step > TRANSIT_DT_MAX)
        step = TRANSIT_DT_MAX;

    while (remaining > 0.0f) {
        float sub_dt = remaining > step ? step : remaining;
        transit_spring_advance(s, target, p, sub_dt, false);
        remaining -= sub_dt;
    }
    return s->value;
}

/* ================================================================== */
/*  Approach / decay — exponential, frame-rate independent            */
/* ================================================================== */

/* Move `current` toward `target` by rate per second.
 * `reduced_motion` resolves to the target in one step. */
TRANSIT_API float transit_approach(float current, float target, float rate, float dt_seconds,
                                   bool reduced_motion) {
    if (target != target) /* NaN target: state stands */
        return current;
    if (reduced_motion)
        return target;
    if (!(rate > 0.0f))
        return target;
    float dt = transit_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return current;
    if (fabsf(target - current) < 1e-6f)
        return target;
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

/* Exponential decay toward zero (opacity tails, trailing values).
 * `reduced_motion` resolves to zero in one step. */
TRANSIT_API float transit_decay(float value, float rate, float dt_seconds, bool reduced_motion) {
    if (reduced_motion)
        return 0.0f;
    if (!(rate > 0.0f))
        return 0.0f;
    float dt = transit_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return value;
    return value * expf(-rate * dt);
}

/* ================================================================== */
/*  Easing — normalized [0,1] curves                                  */
/* ================================================================== */

TRANSIT_API float transit_ease_out_cubic(float t) {
    float inv = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
    return 1.0f - inv * inv * inv;
}

TRANSIT_API float transit_ease_in_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c * c * c;
}

TRANSIT_API float transit_ease_in_out_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c < 0.5f ? 4.0f * c * c * c : 1.0f - powf(-2.0f * c + 2.0f, 3.0f) / 2.0f;
}

TRANSIT_API float transit_ease_out_back(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float d = c - 1.0f;
    return 1.0f + c3 * d * d * d + c1 * d * d;
}

TRANSIT_API float transit_smoothstep(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c * c * (3.0f - 2.0f * c);
}

/* ================================================================== */
/*  Hysteresis — the de-jitter primitive for binary decisions        */
/* ================================================================== */

typedef struct transit_hysteresis {
    bool high;      /* current latched output state */
    float dwell;    /* seconds the candidate state has been held */
    bool candidate; /* the state the input is currently asking for */
} transit_hysteresis;

TRANSIT_API transit_hysteresis transit_hysteresis_init(bool initial_high) {
    transit_hysteresis h = {.high = initial_high, .dwell = 0.0f, .candidate = initial_high};
    return h;
}

/* Advance the latch. `reduced_motion` latches to the input's request in one
 * step, bypassing the dwell timer. */
TRANSIT_API bool transit_hysteresis_step(transit_hysteresis *h, bool input_high,
                                         float low_threshold, float high_threshold,
                                         float measurement, float dwell_seconds, float dt_seconds,
                                         bool reduced_motion) {
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

    float dt = transit_dt_clamp(dt_seconds);

    if (wants != h->high) {
        h->candidate = wants;
        h->dwell += dt;
        if (h->dwell >= dwell_seconds) {
            h->high = wants;
            h->dwell = 0.0f;
        }
    } else {
        h->dwell = 0.0f;
        h->candidate = wants;
    }
    return h->high;
}

/* ================================================================== */
/*  Smoother — critically-damped filter, motion-adaptive tau          */
/* ================================================================== */

typedef struct transit_smoother {
    float value;
    float velocity; /* for the critically-damped follow */
} transit_smoother;

TRANSIT_API transit_smoother transit_smoother_init(float initial) {
    transit_smoother s = {.value = initial, .velocity = 0.0f};
    return s;
}

/* Advance toward `target`. `reduced_motion` resolves to the target in one
 * step (velocity cleared). */
TRANSIT_API float transit_smoother_step(transit_smoother *s, float target, float tau_rest,
                                        float motion_scale, float motion_epsilon, float dt_seconds,
                                        bool reduced_motion) {
    if (!s)
        return target;
    if (reduced_motion) {
        s->value = target;
        s->velocity = 0.0f;
        return target;
    }
    float dt = transit_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return s->value;
    if (tau_rest <= 0.0f)
        return s->value = target;

    float tau = tau_rest;
    if (motion_scale > 1.0f && fabsf(target - s->value) > motion_epsilon)
        tau = tau_rest * motion_scale;

    transit_spring sp = {.value = s->value, .velocity = s->velocity};
    transit_spring_params p = {.stiffness = 4.0f / (tau * tau), .damping = 1.0f};
    float v = transit_spring_advance(&sp, target, p, dt, false);
    s->value = sp.value;
    s->velocity = sp.velocity;
    return v;
}

#ifdef __cplusplus
}
#endif

#endif /* TRANSIT_H */
