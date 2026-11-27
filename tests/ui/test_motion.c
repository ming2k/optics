/* test_motion.c — property and behaviour tests for <optics/ui/motion.h>.
 *
 * Includes <optics/ui/motion.h> directly (without <optics/ui.h>) to verify
 * that the header remains self-contained and free of rendering dependencies.
 *
 * The contract under test (ADR-0177):
 *   - dt is clamped at the boundary: NaN/<=0 integrate nothing, > 1/30 is
 *     clamped, and a huge dt cannot make any primitive diverge.
 *   - the spring is the closed-form analytic solution: advancing one big dt
 *     lands where many small dts land (path independence up to float error),
 *     energy never increases, and it converges to the target.
 *   - approach/decay are frame-rate independent (1 - exp(-rate * dt)).
 *   - reduced motion resolves every primitive in one step.
 *   - hysteresis: no output change until the dwell elapses; the dead band
 *     holds the previous state inside it.
 */

#include "optics_test.h"

#include <optics/ui/motion.h>

#include <math.h>

/* ---- dt clamp --------------------------------------------------------- */

static void test_dt_clamp(void) {
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(0.0f), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(-1.0f), 0.0f, 0.0);
    OT_EXPECT(isnan(optics_ui_motion_dt_clamp(NAN)) == false); /* NaN is "not > 0" -> 0 */
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(NAN), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(0.016f), 0.016f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(1.0f), OPTICS_UI_MOTION_DT_MAX, 1e-6);
    OT_EXPECT_NEAR(optics_ui_motion_dt_clamp(100.0f), OPTICS_UI_MOTION_DT_MAX, 1e-6);
    OT_EXPECT(optics_ui_motion_dt_clamp(OPTICS_UI_MOTION_DT_MAX) == OPTICS_UI_MOTION_DT_MAX);
}

/* ---- spring: boundedness and convergence ------------------------------ */

static void test_spring_bounded_under_adversarial_dt(void) {
    const float dts[] = {0.0f, 1e-6f, 0.0333f, OPTICS_UI_MOTION_DT_MAX, 1.0f, 5.0f, 1e6f};
    for (size_t i = 0; i < sizeof dts / sizeof dts[0]; i++) {
        optics_ui_spring s = optics_ui_spring_at(0.0f);
        optics_ui_spring_params p = optics_ui_spring_bouncy();
        for (int f = 0; f < 64; f++)
            optics_ui_spring_advance(&s, 100.0f, p, dts[i], false);
        OT_EXPECT(isfinite(s.value));
        OT_EXPECT(isfinite(s.velocity));
        OT_EXPECT(s.value > -50.0f && s.value < 250.0f);
    }
}

static void test_spring_nan_input_does_not_poison(void) {
    optics_ui_spring s = optics_ui_spring_at(0.0f);
    optics_ui_spring_params p = optics_ui_spring_snappy();
    float v = optics_ui_spring_advance(&s, NAN, p, 1.0f / 60.0f, false);
    OT_EXPECT(isfinite(v));
    OT_EXPECT_NEAR(v, 0.0f, 0.0);
    OT_EXPECT_NEAR(s.velocity, 0.0f, 0.0);
    v = optics_ui_spring_advance(&s, 10.0f, p, 1.0f / 60.0f, false);
    OT_EXPECT(isfinite(v));
}

static void test_spring_converges(void) {
    optics_ui_spring s = optics_ui_spring_at(0.0f);
    optics_ui_spring_params p = optics_ui_spring_snappy();
    for (int f = 0; f < 240; f++)
        optics_ui_spring_advance(&s, 1.0f, p, 1.0f / 60.0f, false);
    OT_EXPECT_NEAR(s.value, 1.0f, 1e-3);
    OT_EXPECT_NEAR(s.velocity, 0.0f, 1e-3);
    OT_EXPECT(optics_ui_spring_settled(&s, 1.0f, 1e-3f, 1e-3f));
}

static void test_spring_energy_never_increases(void) {
    const float dts[] = {1.0f / 120.0f, 1.0f / 60.0f, 1.0f / 30.0f, OPTICS_UI_MOTION_DT_MAX};
    for (size_t i = 0; i < sizeof dts / sizeof dts[0]; i++) {
        optics_ui_spring s = optics_ui_spring_at(0.0f);
        s.velocity = 40.0f;
        optics_ui_spring_params p = optics_ui_spring_gentle();
        float prev = INFINITY;
        for (int f = 0; f < 200; f++) {
            optics_ui_spring_advance(&s, 0.0f, p, dts[i], false);
            float e = 0.5f * p.stiffness * s.value * s.value + 0.5f * s.velocity * s.velocity;
            OT_EXPECT(e <= prev + 1e-2f);
            prev = e;
        }
    }
}

static void test_spring_reduced_motion_one_step(void) {
    optics_ui_spring s = optics_ui_spring_at(0.0f);
    optics_ui_spring_params p = optics_ui_spring_bouncy();
    float v = optics_ui_spring_advance(&s, 1.0f, p, 1.0f / 60.0f, true);
    OT_EXPECT_NEAR(v, 1.0f, 0.0);
    OT_EXPECT_NEAR(s.velocity, 0.0f, 0.0);
}

static void test_spring_zero_dt_integrates_nothing(void) {
    optics_ui_spring s = optics_ui_spring_at(0.25f);
    s.velocity = 2.0f;
    optics_ui_spring_params p = optics_ui_spring_snappy();
    float v = optics_ui_spring_advance(&s, 1.0f, p, 0.0f, false);
    OT_EXPECT_NEAR(v, 0.25f, 0.0);
    OT_EXPECT_NEAR(s.velocity, 2.0f, 0.0);
}

static void test_spring_advance_substep(void) {
    optics_ui_spring s1 = optics_ui_spring_at(0.0f);
    optics_ui_spring s2 = optics_ui_spring_at(0.0f);
    optics_ui_spring_params p = optics_ui_spring_snappy();

    float v1 = optics_ui_spring_advance_substep(&s1, 100.0f, p, 0.050f, 0.016f, false);
    OT_EXPECT(isfinite(v1));
    OT_EXPECT(v1 > 0.0f && v1 < 100.0f);

    /* Path independence: uniform sub-steps match irregular manual steps analytically. */
    optics_ui_spring_advance(&s2, 100.0f, p, 0.016f, false);
    optics_ui_spring_advance(&s2, 100.0f, p, 0.016f, false);
    optics_ui_spring_advance(&s2, 100.0f, p, 0.016f, false);
    optics_ui_spring_advance(&s2, 100.0f, p, 0.002f, false);
    OT_EXPECT_NEAR(v1, s2.value, 1e-4);
    OT_EXPECT_NEAR(s1.value, s2.value, 1e-4);
    OT_EXPECT_NEAR(s1.velocity, s2.velocity, 1e-3);

    /* Adversarial tiny max_substep_dt must terminate in O(1) steps without stalling. */
    optics_ui_spring s_tiny = optics_ui_spring_at(0.0f);
    float v_tiny = optics_ui_spring_advance_substep(&s_tiny, 100.0f, p, 0.050f, 1e-10f, false);
    OT_EXPECT(isfinite(v_tiny));
    OT_EXPECT_NEAR(v_tiny, s2.value, 1e-3);

    optics_ui_spring s_red = optics_ui_spring_at(0.0f);
    float v_red = optics_ui_spring_advance_substep(&s_red, 50.0f, p, 0.050f, 0.016f, true);
    OT_EXPECT_NEAR(v_red, 50.0f, 1e-6);
    OT_EXPECT_NEAR(s_red.value, 50.0f, 1e-6);
    OT_EXPECT_NEAR(s_red.velocity, 0.0f, 1e-6);
}

static void test_spring_near_critical_damping_and_inf_safe(void) {
    const float zetas[] = {0.9999f, 0.9999999f, 1.0f, 1.5f, NAN};
    for (size_t i = 0; i < sizeof zetas / sizeof zetas[0]; i++) {
        optics_ui_spring s = optics_ui_spring_at(0.0f);
        optics_ui_spring_params p = {.stiffness = 400.0f, .damping = zetas[i]};
        for (int f = 0; f < 120; f++) {
            float v = optics_ui_spring_advance(&s, 100.0f, p, 1.0f / 60.0f, false);
            OT_EXPECT(isfinite(v));
            OT_EXPECT(isfinite(s.velocity));
            OT_EXPECT(v >= -1e-3f && v <= 100.01f);
        }
        OT_EXPECT(optics_ui_spring_settled(&s, 100.0f, 1e-2f, 1e-1f));
    }

    optics_ui_spring s_inf = {.value = INFINITY, .velocity = -INFINITY};
    float v =
        optics_ui_spring_advance(&s_inf, 10.0f, optics_ui_spring_snappy(), 1.0f / 60.0f, false);
    OT_EXPECT_NEAR(v, 10.0f, 0.0);
    OT_EXPECT_NEAR(s_inf.velocity, 0.0f, 0.0);

    optics_ui_spring s_inf_target = optics_ui_spring_at(5.0f);
    float v2 = optics_ui_spring_advance(&s_inf_target, INFINITY, optics_ui_spring_snappy(),
                                        1.0f / 60.0f, false);
    OT_EXPECT_NEAR(v2, 5.0f, 0.0);
}

/* ---- approach / decay --------------------------------------------------- */

static void test_approach_and_decay(void) {
    OT_EXPECT_NEAR(optics_ui_motion_approach(0.0f, 1.0f, 10.0f, 0.0f, false), 0.0f, 0.0);
    float half =
        optics_ui_motion_approach(0.0f, 1.0f, 0.693147f * 30.0f, OPTICS_UI_MOTION_DT_MAX, false);
    OT_EXPECT_NEAR(half, 0.5f, 1e-3);
    /* Frame-rate independence: two 120 Hz steps match one 60 Hz step. */
    float one_60 = optics_ui_motion_approach(0.0f, 1.0f, 14.0f, 1.0f / 60.0f, false);
    float two_120 = optics_ui_motion_approach(0.0f, 1.0f, 14.0f, 1.0f / 120.0f, false);
    two_120 = optics_ui_motion_approach(two_120, 1.0f, 14.0f, 1.0f / 120.0f, false);
    OT_EXPECT_NEAR(one_60, two_120, 1e-6);
    /* Clamped dt: a stall still progresses only by 1/30 s. */
    float stalled = optics_ui_motion_approach(0.0f, 1.0f, 1.0f, 5.0f, false);
    OT_EXPECT_NEAR(
        stalled, optics_ui_motion_approach(0.0f, 1.0f, 1.0f, OPTICS_UI_MOTION_DT_MAX, false), 1e-6);
    OT_EXPECT_NEAR(optics_ui_motion_decay(1.0f, 0.693147f * 30.0f, OPTICS_UI_MOTION_DT_MAX, false),
                   0.5f, 1e-3);
    OT_EXPECT_NEAR(optics_ui_motion_decay(1.0f, 1.0f, 0.0f, false), 1.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_approach(0.3f, 0.3f, 5.0f, 0.016f, false), 0.3f, 0.0);
}

static void test_approach_and_decay_reduced_motion_one_step(void) {
    OT_EXPECT_NEAR(optics_ui_motion_approach(0.0f, 1.0f, 1.0f, 0.0f, true), 1.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_approach(0.7f, -0.3f, 0.5f, OPTICS_UI_MOTION_DT_MAX, true),
                   -0.3f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_decay(0.9f, 0.5f, 0.0f, true), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_motion_decay(0.9f, 3.0f, OPTICS_UI_MOTION_DT_MAX, true), 0.0f, 0.0);
}

/* ---- easing ------------------------------------------------------------- */

static void test_easing_endpoints_and_monotone(void) {
    const float eps = 1e-5f;
    OT_EXPECT_NEAR(optics_ui_ease_out_cubic(0.0f), 0.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_out_cubic(1.0f), 1.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_in_cubic(0.0f), 0.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_in_cubic(1.0f), 1.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_in_out_cubic(0.0f), 0.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_in_out_cubic(1.0f), 1.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_in_out_cubic(0.5f), 0.5f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_out_back(0.0f), 0.0f, 1e-4f);
    OT_EXPECT_NEAR(optics_ui_ease_out_back(1.0f), 1.0f, 1e-4f);
    OT_EXPECT_NEAR(optics_ui_ease_out_cubic(-1.0f), 0.0f, eps);
    OT_EXPECT_NEAR(optics_ui_ease_out_cubic(2.0f), 1.0f, eps);
    /* NaN inputs clamp cleanly to 0 rather than propagating NaN. */
    OT_EXPECT_NEAR(optics_ui_ease_out_cubic(NAN), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_ease_in_cubic(NAN), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_ease_in_out_cubic(NAN), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_ease_out_back(NAN), 0.0f, 0.0);
    OT_EXPECT_NEAR(optics_ui_smoothstep(NAN), 0.0f, 0.0);
    float prev = -1.0f;
    for (int i = 0; i <= 20; i++) {
        float v = optics_ui_ease_out_cubic(i / 20.0f);
        OT_EXPECT(v >= prev - 1e-6f);
        prev = v;
    }
}

static void test_smoothstep(void) {
    OT_EXPECT_NEAR(optics_ui_smoothstep(0.0f), 0.0f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_smoothstep(1.0f), 1.0f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_smoothstep(0.5f), 0.5f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_smoothstep(0.2f) + optics_ui_smoothstep(0.8f), 1.0f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_smoothstep(-0.5f), 0.0f, 1e-6);
    OT_EXPECT_NEAR(optics_ui_smoothstep(1.5f), 1.0f, 1e-6);
    OT_EXPECT(optics_ui_smoothstep(0.3f) < optics_ui_smoothstep(0.4f));
}

/* ---- hysteresis --------------------------------------------------------- */

static void test_hysteresis_dwell_blocks_flip(void) {
    optics_ui_hysteresis h = optics_ui_hysteresis_init(true);
    const float dt = 1.0f / 60.0f;
    for (int f = 0; f < 4; f++)
        optics_ui_hysteresis_step(&h, false, 0.38f, 0.42f, 0.30f, 0.150f, dt, false);
    OT_EXPECT(h.high == true);
    int flips = 0;
    bool prev = h.high;
    for (int f = 0; f < 20; f++) {
        bool now = optics_ui_hysteresis_step(&h, false, 0.38f, 0.42f, 0.30f, 0.150f, dt, false);
        if (now != prev)
            flips++;
        prev = now;
    }
    OT_EXPECT(flips == 1);
    OT_EXPECT(h.high == false);
}

static void test_hysteresis_dead_band_holds(void) {
    optics_ui_hysteresis h = optics_ui_hysteresis_init(false);
    for (int f = 0; f < 60; f++)
        optics_ui_hysteresis_step(&h, true, 0.38f, 0.42f, 0.40f, 0.0f, 1.0f / 60.0f, false);
    OT_EXPECT(h.high == false);
}

static void test_hysteresis_zero_dwell_tracks_input(void) {
    optics_ui_hysteresis h = optics_ui_hysteresis_init(false);
    bool v = optics_ui_hysteresis_step(&h, true, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f / 60.0f, false);
    OT_EXPECT(v == true);
}

static void test_hysteresis_reduced_motion_latches_in_one_step(void) {
    optics_ui_hysteresis h = optics_ui_hysteresis_init(false);
    OT_EXPECT(optics_ui_hysteresis_step(&h, true, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, true) == true);
    OT_EXPECT(h.high == true);
    OT_EXPECT(h.dwell == 0.0f);
    OT_EXPECT(optics_ui_hysteresis_step(&h, false, 0.0f, 0.0f, 0.0f, 10.0f, 0.0f, true) == false);
    OT_EXPECT(h.high == false);
}

static void test_hysteresis_nan_dwell_does_not_lock(void) {
    optics_ui_hysteresis h = optics_ui_hysteresis_init(false);
    OT_EXPECT(optics_ui_hysteresis_step(&h, true, 0.0f, 0.0f, 0.0f, NAN, 1.0f / 60.0f, false) ==
              true);
    OT_EXPECT(h.high == true);
}

/* ---- NULL safety -------------------------------------------------------- */

static void test_null_safety(void) {
    OT_EXPECT(optics_ui_spring_advance(nullptr, 1.0f, optics_ui_spring_snappy(), 0.016f, false) ==
              1.0f);
    OT_EXPECT(optics_ui_spring_advance_substep(nullptr, 1.0f, optics_ui_spring_snappy(), 0.016f,
                                               0.016f, false) == 1.0f);
    OT_EXPECT(optics_ui_spring_settled(nullptr, 0.0f, 0.0f, 0.0f) == false);
    OT_EXPECT(optics_ui_spring_snap_to(nullptr, 2.0f) == 2.0f);
    OT_EXPECT(optics_ui_hysteresis_step(nullptr, true, 0.0f, 0.0f, 0.0f, 0.0f, 0.016f, false) ==
              false);
}

int main(void) {
    test_dt_clamp();
    test_spring_bounded_under_adversarial_dt();
    test_spring_nan_input_does_not_poison();
    test_spring_converges();
    test_spring_energy_never_increases();
    test_spring_reduced_motion_one_step();
    test_spring_zero_dt_integrates_nothing();
    test_spring_advance_substep();
    test_spring_near_critical_damping_and_inf_safe();
    test_approach_and_decay();
    test_approach_and_decay_reduced_motion_one_step();
    test_easing_endpoints_and_monotone();
    test_smoothstep();
    test_hysteresis_dwell_blocks_flip();
    test_hysteresis_dead_band_holds();
    test_hysteresis_zero_dwell_tracks_input();
    test_hysteresis_reduced_motion_latches_in_one_step();
    test_hysteresis_nan_dwell_does_not_lock();
    test_null_safety();
    return OT_REPORT();
}
