/* transit.c — the shared state transition and motion vocabulary (ADR-0077, ADR-0106).
 *
 * Pure math on caller-owned state: no clock, no allocation, no globals.
 * Every advance entry point funnels `dt` through transit_dt_clamp, and the
 * spring advances by the closed-form analytic solution so no accepted dt
 * can diverge. Property tests live in tests/transit/test_transit.c; the exact
 * values of the presets are pinned there too. */

#include <transit/transit.h>

#include <math.h>

/* ---- Version accessors ------------------------------------------------ */

#define TRANSIT_STR2(x) #x
#define TRANSIT_STR(x) TRANSIT_STR2(x)

void transit_version(int *major, int *minor, int *patch) {
    if (major)
        *major = TRANSIT_VERSION_MAJOR;
    if (minor)
        *minor = TRANSIT_VERSION_MINOR;
    if (patch)
        *patch = TRANSIT_VERSION_PATCH;
}

uint32_t transit_version_number(void) {
    return TRANSIT_VERSION_NUMBER;
}

bool transit_version_check(int major, int minor, int patch) {
    if (major != TRANSIT_VERSION_MAJOR)
        return false;
    if (minor > TRANSIT_VERSION_MINOR)
        return false;
    if (minor == TRANSIT_VERSION_MINOR && patch > TRANSIT_VERSION_PATCH)
        return false;
    return true;
}

const char *transit_version_string(void) {
    return TRANSIT_STR(TRANSIT_VERSION_MAJOR) "." TRANSIT_STR(TRANSIT_VERSION_MINOR) "." TRANSIT_STR(
        TRANSIT_VERSION_PATCH);
}

float transit_dt_clamp(float dt_seconds) {
    if (!(dt_seconds > 0.0f)) /* NaN and ≤ 0 integrate nothing */
        return 0.0f;
    return dt_seconds > TRANSIT_DT_MAX ? TRANSIT_DT_MAX : dt_seconds;
}

/* ---- spring --------------------------------------------------------- */

transit_spring_params transit_spring_snappy(void) {
    return (transit_spring_params){.stiffness = 480.0f, .damping = 0.90f};
}
transit_spring_params transit_spring_gentle(void) {
    return (transit_spring_params){.stiffness = 220.0f, .damping = 0.80f};
}
transit_spring_params transit_spring_bouncy(void) {
    return (transit_spring_params){.stiffness = 480.0f, .damping = 0.55f};
}

transit_spring transit_spring_at(float value) {
    transit_spring s = {.value = value, .velocity = 0.0f};
    return s;
}

bool transit_spring_settled(const transit_spring *s, float target, float value_epsilon,
                            float velocity_epsilon) {
    if (!s)
        return false;
    return fabsf(s->value - target) <= value_epsilon && fabsf(s->velocity) <= velocity_epsilon;
}

float transit_spring_snap_to(transit_spring *s, float target) {
    if (!s)
        return target;
    s->value = target;
    s->velocity = 0.0f;
    return target;
}

float transit_spring_advance(transit_spring *s, float target, transit_spring_params p, float dt_seconds,
                             bool reduced_motion) {
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

float transit_approach(float current, float target, float rate, float dt_seconds,
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

float transit_decay(float value, float rate, float dt_seconds, bool reduced_motion) {
    if (reduced_motion)
        return 0.0f;
    if (!(rate > 0.0f))
        return 0.0f;
    float dt = transit_dt_clamp(dt_seconds);
    if (dt <= 0.0f)
        return value;
    return value * expf(-rate * dt);
}

/* ---- easing ----------------------------------------------------------- */

float transit_ease_out_cubic(float t) {
    float inv = 1.0f - (t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t));
    return 1.0f - inv * inv * inv;
}

float transit_ease_in_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c * c * c;
}

float transit_ease_in_out_cubic(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return c < 0.5f ? 4.0f * c * c * c : 1.0f - powf(-2.0f * c + 2.0f, 3.0f) / 2.0f;
}

float transit_ease_out_back(float t) {
    float c = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    float d = c - 1.0f;
    return 1.0f + c3 * d * d * d + c1 * d * d;
}

/* ---- hysteresis ------------------------------------------------------- */

transit_hysteresis transit_hysteresis_init(bool initial_high) {
    transit_hysteresis h = {.high = initial_high, .dwell = 0.0f, .candidate = initial_high};
    return h;
}

bool transit_hysteresis_step(transit_hysteresis *h, bool input_high, float low_threshold,
                             float high_threshold, float measurement, float dwell_seconds,
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

/* ---- smoother ---------------------------------------------------------- */

transit_smoother transit_smoother_init(float initial) {
    transit_smoother s = {.value = initial, .velocity = 0.0f};
    return s;
}

float transit_smoother_step(transit_smoother *s, float target, float tau_rest, float motion_scale,
                            float motion_epsilon, float dt_seconds, bool reduced_motion) {
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
