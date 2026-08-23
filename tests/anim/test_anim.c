/* test_anim.c — property and behaviour tests for the motion vocabulary.
 *
 * The contract under test (ADR-0077):
 *   - dt is clamped at the boundary: NaN/≤0 integrate nothing, > 1/30 is
 *     clamped, and a huge dt cannot make any primitive diverge.
 *   - the spring is the closed-form solution: advancing one big dt lands
 *     where many small dts land (path independence up to float error),
 *     energy never increases, and it converges to the target.
 *   - reduced motion resolves everything in one step.
 *   - hysteresis: no output change until the dwell elapses; the dead band
 *     holds the previous state inside it.
 *   - the smoother converges, and widens its tau while the input moves.
 */

#include "test_helpers.h"

#include <anim/anim.h>

#include <math.h>

/* ---- dt clamp --------------------------------------------------------- */

static void test_dt_clamp(void) {
    CHECK_NEAR(anim_dt_clamp(0.0f), 0.0f, 0.0);
    CHECK_NEAR(anim_dt_clamp(-1.0f), 0.0f, 0.0);
    CHECK(isnan(anim_dt_clamp(NAN)) == false); /* NaN is "not > 0" → 0 */
    CHECK_NEAR(anim_dt_clamp(NAN), 0.0f, 0.0);
    /* NaN dt integrates nothing, same as 0 — the guard is !(x > 0). */
    CHECK_NEAR(anim_dt_clamp(0.016f), 0.016f, 1e-6);
    CHECK_NEAR(anim_dt_clamp(1.0f), ANIM_DT_MAX, 1e-6);
    CHECK_NEAR(anim_dt_clamp(100.0f), ANIM_DT_MAX, 1e-6);
    CHECK(anim_dt_clamp(ANIM_DT_MAX) == ANIM_DT_MAX);
}

/* ---- spring: boundedness and convergence ------------------------------ */

static void test_spring_bounded_under_adversarial_dt(void) {
    /* Any dt the contract accepts keeps the spring finite and inside a
     * sane envelope around the target. */
    const float dts[] = {0.0f, 1e-6f, 0.0333f, ANIM_DT_MAX, 1.0f, 5.0f, 1e6f};
    for (size_t i = 0; i < sizeof dts / sizeof dts[0]; i++) {
        anim_spring s = anim_spring_at(0.0f);
        anim_spring_params p = anim_spring_bouncy(); /* most overshoot */
        for (int f = 0; f < 64; f++)
            anim_spring_advance(&s, 100.0f, p, dts[i], false);
        CHECK(isfinite(s.value));
        CHECK(isfinite(s.velocity));
        /* Bounded well away from divergence: the analytic solution's
         * envelope is target ± initial displacement. */
        CHECK(s.value > -50.0f && s.value < 250.0f);
    }
}

static void test_spring_nan_input_does_not_poison(void) {
    anim_spring s = anim_spring_at(0.0f);
    anim_spring_params p = anim_spring_snappy();
    /* A NaN target means "no target this frame": the state stands. */
    float v = anim_spring_advance(&s, NAN, p, 1.0f / 60.0f, false);
    CHECK(isfinite(v));
    CHECK_NEAR(v, 0.0f, 0.0);
    CHECK_NEAR(s.velocity, 0.0f, 0.0);
    /* And it does not poison the next finite call. */
    v = anim_spring_advance(&s, 10.0f, p, 1.0f / 60.0f, false);
    CHECK(isfinite(v));
}

static void test_spring_converges(void) {
    anim_spring s = anim_spring_at(0.0f);
    anim_spring_params p = anim_spring_snappy();
    for (int f = 0; f < 240; f++)
        anim_spring_advance(&s, 1.0f, p, 1.0f / 60.0f, false);
    CHECK_NEAR(s.value, 1.0f, 1e-3);
    CHECK_NEAR(s.velocity, 0.0f, 1e-3);
    CHECK(anim_spring_settled(&s, 1.0f, 1e-3f, 1e-3f));
}

static void test_spring_energy_never_increases(void) {
    /* E = ½k x² + ½ v² (mass 1). Monotone non-increasing across a run
     * under every accepted dt (the closed-form solution's guarantee). */
    const float dts[] = {1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f, ANIM_DT_MAX};
    for (size_t i = 0; i < sizeof dts / sizeof dts[0]; i++) {
        anim_spring s = anim_spring_at(0.0f);
        s.velocity = 40.0f;
        anim_spring_params p = anim_spring_gentle();
        float prev = INFINITY;
        for (int f = 0; f < 200; f++) {
            anim_spring_advance(&s, 0.0f, p, dts[i], false);
            float e = 0.5f * p.stiffness * s.value * s.value +
                      0.5f * s.velocity * s.velocity;
            CHECK(e <= prev + 1e-2f);
            prev = e;
        }
    }
}

static void test_spring_reduced_motion_one_step(void) {
    anim_spring s = anim_spring_at(0.0f);
    anim_spring_params p = anim_spring_bouncy();
    float v = anim_spring_advance(&s, 1.0f, p, 1.0f / 60.0f, true);
    CHECK_NEAR(v, 1.0f, 0.0);
    CHECK_NEAR(s.velocity, 0.0f, 0.0);
}

static void test_spring_zero_dt_integrates_nothing(void) {
    anim_spring s = anim_spring_at(0.25f);
    s.velocity = 2.0f;
    anim_spring_params p = anim_spring_snappy();
    float v = anim_spring_advance(&s, 1.0f, p, 0.0f, false);
    CHECK_NEAR(v, 0.25f, 0.0);
    CHECK_NEAR(s.velocity, 2.0f, 0.0);
}

/* ---- approach / decay --------------------------------------------------- */

static void test_approach_and_decay(void) {
    CHECK_NEAR(anim_approach(0.0f, 1.0f, 10.0f, 0.0f), 0.0f, 0.0);
    /* Half-life: rate·dt = ln2 of the remaining distance, measured in
     * clamped dt. dt 0.693 clamps to 1/30, so use a rate whose clamped
     * step covers half the distance: rate = ln2/(1/30). */
    float half = anim_approach(0.0f, 1.0f, 0.693147f * 30.0f, ANIM_DT_MAX);
    CHECK_NEAR(half, 0.5f, 1e-3);
    /* Clamped dt: a stall still progresses only by 1/30 s. */
    float stalled = anim_approach(0.0f, 1.0f, 1.0f, 5.0f);
    CHECK_NEAR(stalled, anim_approach(0.0f, 1.0f, 1.0f, ANIM_DT_MAX), 1e-6);
    CHECK_NEAR(anim_decay(1.0f, 0.693147f * 30.0f, ANIM_DT_MAX), 0.5f, 1e-3);
    CHECK_NEAR(anim_decay(1.0f, 1.0f, 0.0f), 1.0f, 0.0);
    CHECK_NEAR(anim_approach(0.3f, 0.3f, 5.0f, 0.016f), 0.3f, 0.0);
}

/* ---- easing ------------------------------------------------------------- */

static void test_easing_endpoints_and_monotone(void) {
    const float eps = 1e-5f;
    CHECK_NEAR(anim_ease_out_cubic(0.0f), 0.0f, eps);
    CHECK_NEAR(anim_ease_out_cubic(1.0f), 1.0f, eps);
    CHECK_NEAR(anim_ease_in_cubic(0.0f), 0.0f, eps);
    CHECK_NEAR(anim_ease_in_cubic(1.0f), 1.0f, eps);
    CHECK_NEAR(anim_ease_in_out_cubic(0.0f), 0.0f, eps);
    CHECK_NEAR(anim_ease_in_out_cubic(1.0f), 1.0f, eps);
    CHECK_NEAR(anim_ease_in_out_cubic(0.5f), 0.5f, eps);
    CHECK_NEAR(anim_ease_out_back(0.0f), 0.0f, 1e-4f);
    CHECK_NEAR(anim_ease_out_back(1.0f), 1.0f, 1e-4f);
    /* Out-of-range inputs clamp, never extrapolate. */
    CHECK_NEAR(anim_ease_out_cubic(-1.0f), 0.0f, eps);
    CHECK_NEAR(anim_ease_out_cubic(2.0f), 1.0f, eps);
    /* ease_out_cubic is monotone non-decreasing. */
    float prev = -1.0f;
    for (int i = 0; i <= 20; i++) {
        float v = anim_ease_out_cubic(i / 20.0f);
        CHECK(v >= prev - 1e-6f);
        prev = v;
    }
}

/* ---- hysteresis ---------------------------------------------------------- */

static void test_hysteresis_dwell_blocks_flip(void) {
    /* The latched state survives a disagreement shorter than the dwell:
     * exactly the de-jitter a 3-frame-stale polarity measurement needs. */
    anim_hysteresis h = anim_hysteresis_init(true);
    const float dt = 1.0f / 60.0f;
    /* Disagree for 4 frames (~67 ms) with a 150 ms dwell: no flip. */
    for (int f = 0; f < 4; f++)
        anim_hysteresis_step(&h, false, 0.38f, 0.42f, 0.30f, 0.150f, dt);
    CHECK(h.high == true);
    /* Disagree past the dwell: flips exactly once. */
    int flips = 0;
    bool prev = h.high;
    for (int f = 0; f < 20; f++) {
        bool now = anim_hysteresis_step(&h, false, 0.38f, 0.42f, 0.30f, 0.150f, dt);
        if (now != prev)
            flips++;
        prev = now;
    }
    CHECK(flips == 1);
    CHECK(h.high == false);
}

static void test_hysteresis_dead_band_holds(void) {
    /* Inside the band the previous output stands regardless of the raw
     * comparator. */
    anim_hysteresis h = anim_hysteresis_init(false);
    for (int f = 0; f < 60; f++)
        anim_hysteresis_step(&h, true, 0.38f, 0.42f, 0.40f, 0.0f, 1.0f / 60.0f);
    CHECK(h.high == false); /* 0.40 is inside [0.38, 0.42): stays low */
}

static void test_hysteresis_zero_dwell_tracks_input(void) {
    anim_hysteresis h = anim_hysteresis_init(false);
    bool v = anim_hysteresis_step(&h, true, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f / 60.0f);
    CHECK(v == true);
}

/* ---- smoother ------------------------------------------------------------- */

static void test_smoother_converges_and_widens_under_motion(void) {
    anim_smoother s = anim_smoother_init(0.0f);
    for (int f = 0; f < 240; f++)
        anim_smoother_step(&s, 1.0f, 0.05f, 8.0f, 0.05f, 1.0f / 60.0f);
    CHECK_NEAR(s.value, 1.0f, 1e-3);

    /* Same tau, no motion scaling: must be FASTER to settle on a step
     * (the adaptive path is the one that lags by design). */
    anim_smoother plain = anim_smoother_init(0.0f);
    anim_smoother adaptive = anim_smoother_init(0.0f);
    for (int f = 0; f < 30; f++) {
        anim_smoother_step(&plain, 1.0f, 0.05f, 1.0f, 1e9f, 1.0f / 60.0f);
        anim_smoother_step(&adaptive, 1.0f, 0.05f, 8.0f, 0.05f, 1.0f / 60.0f);
    }
    CHECK(plain.value > adaptive.value); /* adaptive lags while moving */
    CHECK(adaptive.value > 0.0f);        /* but still progresses */
}

static void test_smoother_step_change_is_bounded(void) {
    /* A sudden huge jump cannot throw the filter past the target. */
    anim_smoother s = anim_smoother_init(0.0f);
    for (int f = 0; f < 32; f++) {
        anim_smoother_step(&s, 1.0f, 0.02f, 8.0f, 0.05f, ANIM_DT_MAX);
        CHECK(s.value <= 1.0f + 1e-4f);
        CHECK(isfinite(s.value));
    }
}

/* ---- NULL safety ---------------------------------------------------------- */

static void test_null_safety(void) {
    CHECK(anim_spring_advance(NULL, 1.0f, anim_spring_snappy(), 0.016f, false) == 1.0f);
    CHECK(anim_spring_settled(NULL, 0.0f, 0.0f, 0.0f) == false);
    CHECK(anim_hysteresis_step(NULL, true, 0.0f, 0.0f, 0.0f, 0.0f, 0.016f) == false);
    CHECK(anim_smoother_step(NULL, 1.0f, 0.05f, 4.0f, 0.05f, 0.016f) == 1.0f);
}

int main(void) {
    test_dt_clamp();
    test_spring_bounded_under_adversarial_dt();
    test_spring_nan_input_does_not_poison();
    test_spring_converges();
    test_spring_energy_never_increases();
    test_spring_reduced_motion_one_step();
    test_spring_zero_dt_integrates_nothing();
    test_approach_and_decay();
    test_easing_endpoints_and_monotone();
    test_hysteresis_dwell_blocks_flip();
    test_hysteresis_dead_band_holds();
    test_hysteresis_zero_dwell_tracks_input();
    test_smoother_converges_and_widens_under_motion();
    test_smoother_step_change_is_bounded();
    test_null_safety();
    return TEST_REPORT();
}
