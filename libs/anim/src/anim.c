/* anim.c — the shared motion vocabulary (ADR-0077).
 *
 * Pure math on caller-owned state: no clock, no allocation, no globals.
 * Every advance entry point funnels `dt` through anim_dt_clamp, and the
 * spring advances by the closed-form analytic solution so no accepted dt
 * can diverge. Property tests live in tests/anim/test_anim.c; the exact
 * values of the presets are pinned there too. */

#include <anim/anim.h>

#include <math.h>

float anim_dt_clamp(float dt_seconds) {
    if (!(dt_seconds > 0.0f)) /* NaN and ≤ 0 integrate nothing */
        return 0.0f;
    return dt_seconds > ANIM_DT_MAX ? ANIM_DT_MAX : dt_seconds;
}

/* ---- spring --------------------------------------------------------- */

anim_spring_params anim_spring_snappy(void) {
    return (anim_spring_params){.stiffness = 480.0f, .damping = 0.90f};
}
anim_spring_params anim_spring_gentle(void) {
    return (anim_spring_params){.stiffness = 220.0f, .damping = 0.80f};
}
anim_spring_params anim_spring_bouncy(void) {
    return (anim_spring_params){.stiffness = 480.0f, .damping = 0.55f};
}

anim_spring anim_spring_at(float value) {
    anim_spring s = {.value = value, .velocity = 0.0f};
    return s;
}

bool anim_spring_settled(const anim_spring *s, float target, float value_epsilon,
                         float velocity_epsilon) {
    if (!s)
        return false;
    return fabsf(s->value - target) <= value_epsilon && fabsf(s->velocity) <= velocity_epsilon;
}

float anim_spring_snap_to(anim_spring *s, float target) {
    if (!s)
        return target;
    s->value = target;
    s->velocity = 0.0f;
    return target;
}

float anim_spring_advance(anim_spring *s, float target, anim_spring_params p, float dt_seconds,
                          bool reduced_motion) {
    if (!s)
        return target;
    if (target != target) /* NaN target = no target: state stands */
        return s->value;
    if (reduced_motion)
        return anim_spring_snap_to(s, target);
    const float dt = anim_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return s->value;

    float omega0sq = p.stiffness > 0.0f ? p.stiffness : 0.0f;
    float omega0 = sqrtf(omega0sq);
    if (omega0 <= 0.0f)
        return anim_spring_snap_to(s, target);
    float zeta = p.damping;
    if (!(zeta >= 0.0f)) /* NaN → critically damped */
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
        /* Critically damped (ζ ≥ 1 clamped): smooth approach, no overshoot. */
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

/* ---- approach / decay ------------------------------------------------ */

float anim_approach(float current, float target, float rate, float dt_seconds) {
    if (target != target) /* NaN target: state stands */
        return current;
    if (!(rate > 0.0f))
        return target;
    float dt = anim_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return current;
    if (fabsf(target - current) < 1e-6f)
        return target;
    return current + (target - current) * (1.0f - expf(-rate * dt));
}

float anim_decay(float value, float rate, float dt_seconds) {
    if (!(rate > 0.0f))
        return 0.0f;
    float dt = anim_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return value;
    return value * expf(-rate * dt);
}

/* ---- easing ----------------------------------------------------------- */

float anim_ease_out_cubic(float t) {
    float inv = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
    return 1.0f - inv * inv * inv;
}

float anim_ease_in_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c * c * c;
}

float anim_ease_in_out_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c < 0.5f ? 4.0f * c * c * c : 1.0f - powf(-2.0f * c + 2.0f, 3.0f) / 2.0f;
}

float anim_ease_out_back(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float d = c - 1.0f;
    return 1.0f + c3 * d * d * d + c1 * d * d;
}

/* ---- hysteresis ------------------------------------------------------- */

anim_hysteresis anim_hysteresis_init(bool initial_high) {
    anim_hysteresis h = {.high = initial_high, .dwell = 0.0f, .candidate = initial_high};
    return h;
}

bool anim_hysteresis_step(anim_hysteresis *h, bool input_high, float low_threshold,
                          float high_threshold, float measurement, float dwell_seconds,
                          float dt_seconds) {
    if (!h)
        return false;
    float dt = anim_dt_clamp(dt_seconds);

    /* Comparator with a dead band: inside the band the previous output
     * stands (a hysteresis latch in the classic sense). The caller may
     * pass the same value for both thresholds to disable the band and
     * rely on the dwell alone. */
    bool wants;
    if (low_threshold < high_threshold && measurement == measurement) {
        if (h->high) {
            /* currently high: only drop when we cross BELOW low */
            wants = measurement >= low_threshold ? true : false;
        } else {
            /* currently low: only rise when we cross ABOVE high */
            wants = measurement > high_threshold ? true : false;
        }
    } else {
        wants = input_high;
    }

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

/* ---- smoother ---------------------------------------------------------- */

anim_smoother anim_smoother_init(float initial) {
    anim_smoother s = {.value = initial, .velocity = 0.0f};
    return s;
}

float anim_smoother_step(anim_smoother *s, float target, float tau_rest, float motion_scale,
                         float motion_epsilon, float dt_seconds) {
    if (!s)
        return target;
    float dt = anim_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return s->value;
    if (tau_rest <= 0.0f)
        return s->value = target;

    float tau = tau_rest;
    if (motion_scale > 1.0f && fabsf(target - s->value) > motion_epsilon)
        tau = tau_rest * motion_scale;

    /* Critically-damped follow: ω₀ = 2/τ for a two-pole critically-damped
     * response with settling time ≈ τ·(a few). Integrated by the same
     * closed-form spring with ζ = 1 — reuse, not a second integrator. */
    anim_spring sp = {.value = s->value, .velocity = s->velocity};
    anim_spring_params p = {.stiffness = 4.0f / (tau * tau), .damping = 1.0f};
    float v = anim_spring_advance(&sp, target, p, dt, false);
    s->value = sp.value;
    s->velocity = sp.velocity;
    return v;
}
