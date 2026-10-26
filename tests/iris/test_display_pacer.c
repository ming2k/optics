/* test_display_pacer.c — the frame-cadence state machine (ADR-0122).
 *
 * Headless: display_pacer.{h,c} has no clock, no platform header, and no
 * allocation, so the whole cadence policy is exercised here without a
 * compositor or a GPU. These tests are the acceptance evidence for the
 * ADR-0122 invariants:
 *
 *   - INV-PACE-01/02: cadence is driven by the display, never by a client
 *     period. A pulse-per-refresh drive paints exactly once per pulse and
 *     honours 60 / 120 / 144 Hz refresh periods without a hardcoded target.
 *
 *   - INV-PACE-03: a non-quiescent tick ALWAYS returns a poll budget of at
 *     least 1 ns, and the schedule never moves backwards. The regression
 *     this pins down is the pre-pacer bug: a run of zero-render skips left
 *     the scheduling baseline frozen at the last *paint*, so
 *     `next_deadline = last_render_ns + period` landed in the past, the poll
 *     budget went to 0, and the loop spun a CPU core at 100%.
 *
 *   - INV-PACE-04: the simulation clock is the compositor's pulse time when
 *     one was delivered, not the wall clock at build time.
 */

#include "display_pacer.h"

#include "test_helpers.h"

#define NS_PER_MS 1000000LL
#define NS_PER_S 1000000000LL

#define HZ60_NS 16666667LL
#define HZ120_NS 8333333LL
#define HZ144_NS 6944444LL

/* A baseline "the window is alive" input. Overlays are applied per scenario.
 * Deliberately a *lens-only* host (no cfg->paint): that is the configuration
 * that can reach full quiescence, so scenarios that want the low tick or the
 * pulse cadence opt in explicitly. */
static iris_pace_inputs base_inputs(int64_t now) {
    return (iris_pace_inputs){
        .now_ns = now,
        .refresh_ns = 0,
        .have_pulse_source = true,
        .has_custom_paint = false,
    };
}

/* Assert the hard postcondition on every decision the pacer can return. */
static void check_no_spin(const iris_pace_decision *d, int line) {
    g_checks++;
    if (d->action == IRIS_PACE_QUIESCE) {
        /* Quiesced: the caller blocks (wait 0 == "no deadline"). */
        if (d->wait_timeout_ns != 0) {
            g_fails++;
            fprintf(stderr, "FAIL %s:%d: quiesced decision must have wait 0, got %lld\n", __FILE__,
                    line, (long long)d->wait_timeout_ns);
        }
        return;
    }
    if (d->wait_timeout_ns < 1) {
        g_fails++;
        fprintf(stderr, "FAIL %s:%d: non-quiescent budget %lld <= 0 (0ms poll spin)\n", __FILE__,
                line, (long long)d->wait_timeout_ns);
    }
}
#define CHECK_NO_SPIN(d) check_no_spin(&(d), __LINE__)

/* ------------------------------------------------------------------ */

static void test_refresh_configuration(void) {
    iris_pacer p;
    iris_pacer_init(&p);

    /* The 60 Hz default is only a "refresh rate not learned yet" fallback
     * (INV-PACE-02): it must be replaced the moment the display reports. */
    CHECK(iris_pacer_refresh_ns(&p) == IRIS_PACE_DEFAULT_REFRESH_NS);

    iris_pacer_set_refresh(&p, HZ120_NS);
    CHECK(iris_pacer_refresh_ns(&p) == HZ120_NS);

    /* 0 / negative means "unknown" — keep the last known period. */
    iris_pacer_set_refresh(&p, 0);
    CHECK(iris_pacer_refresh_ns(&p) == HZ120_NS);
    iris_pacer_set_refresh(&p, -1);
    CHECK(iris_pacer_refresh_ns(&p) == HZ120_NS);

    /* Garbage cannot drive the pacer into a spin: clamp to the sane band. */
    iris_pacer_set_refresh(&p, 1);
    CHECK(iris_pacer_refresh_ns(&p) == IRIS_PACE_REFRESH_MIN_NS);
    iris_pacer_set_refresh(&p, 60LL * NS_PER_S);
    CHECK(iris_pacer_refresh_ns(&p) == IRIS_PACE_REFRESH_MAX_NS);
}

/* One paint per compositor pulse, at whatever refresh the display runs. */
static void test_pulse_lockstep(void) {
    const int64_t periods[] = {HZ60_NS, HZ120_NS, HZ144_NS};
    for (size_t k = 0; k < sizeof periods / sizeof periods[0]; k++) {
        const int64_t refresh = periods[k];
        iris_pacer p;
        iris_pacer_init(&p);
        iris_pacer_set_refresh(&p, refresh);

        int paints = 0, skips = 0;
        int64_t t = 0;
        for (int i = 0; i < 240; i++) {
            /* First tick: no pulse outstanding yet, so the pacer grants a
             * draw to make the first commit (which starts the pulse train). */
            iris_pace_inputs in = base_inputs(t);
            in.refresh_ns = refresh;
            in.host_animating = true;
            in.has_custom_paint = true;
            in.input_active = (i == 0);
            in.pulse_fired = (i != 0);
            in.pulse_time_ns = (i != 0) ? t : 0;
            /* The prior subscription has just fired; a new one is requested
             * as part of this cycle. */
            in.pulse_pending = false;

            iris_pace_decision d = iris_pacer_tick(&p, &in);
            CHECK_NO_SPIN(d);

            if (i == 0) {
                CHECK(d.action == IRIS_PACE_PAINT);
                CHECK(d.request_pulse);
            } else {
                CHECK(d.action == IRIS_PACE_PAINT);
                /* INV-PACE-04: sim time is the pulse time, not "now". */
                CHECK(d.sim_time_ns == t);
                paints++;
            }
            if (d.action == IRIS_PACE_SKIP)
                skips++;

            /* Wake-ups that are not a pulse must never publish a frame: with
             * a subscription outstanding the pacer stays in lockstep. */
            iris_pace_inputs spurious = base_inputs(t + refresh / 4);
            spurious.refresh_ns = refresh;
            spurious.host_animating = true;
            spurious.pulse_fired = false;
            spurious.pulse_pending = true;
            iris_pace_decision s = iris_pacer_tick(&p, &spurious);
            CHECK_NO_SPIN(s);
            CHECK(s.action == IRIS_PACE_SKIP);
            CHECK(s.request_pulse);

            t += refresh;
        }
        CHECK(paints == 239);
        CHECK(skips == 0);
    }
}

/* INV-PACE-03 regression: a storm of zero-render skips must never produce a
 * non-positive poll budget. Under the pre-pacer schedule this is precisely
 * where `next_deadline` decayed into the past and poll(0) spun. */
static void test_skip_render_storm_never_spins(void) {
    iris_pacer p;
    iris_pacer_init(&p);
    iris_pacer_set_refresh(&p, HZ120_NS);

    int64_t t = 0;
    int paints = 0, skips = 0;
    for (int i = 0; i < 2000; i++) {
        iris_pace_inputs in = base_inputs(t);
        in.refresh_ns = HZ120_NS;
        /* The media-host pattern: the cadence must keep running, the canvas
         * content is declared unchanged, nothing is animating. */
        in.host_skip_render = true;
        in.has_custom_paint = true;
        in.have_pulse_source = true;
        in.pulse_pending = true; /* a subscription is outstanding */
        in.pulse_fired = (i % 2) == 0;
        in.pulse_time_ns = in.pulse_fired ? t : 0;

        iris_pace_decision d = iris_pacer_tick(&p, &in);
        CHECK_NO_SPIN(d);
        CHECK(d.action != IRIS_PACE_QUIESCE); /* skip-streaming keeps cadence */
        CHECK(d.request_pulse);
        CHECK(d.wait_timeout_ns > 0);

        if (d.action == IRIS_PACE_PAINT)
            paints++;
        else
            skips++;
        t += HZ120_NS;
    }
    CHECK(paints == 0); /* every declared skip was honoured   */
    CHECK(skips == 2000);
}

/* A backend with no pulse source must still be display-aligned and spin-free:
 * the timer period is derived from the learned refresh, never hardcoded. */
static void test_timer_fallback_is_display_aligned(void) {
    /* Descending: high refresh (small ns) up to 60 Hz (large ns). */
    const int64_t periods[] = {HZ144_NS, HZ120_NS, HZ60_NS};
    for (size_t k = 0; k < sizeof periods / sizeof periods[0]; k++) {
        const int64_t refresh = periods[k];
        iris_pacer p;
        iris_pacer_init(&p);
        iris_pacer_set_refresh(&p, refresh);

        int64_t t = 0;
        int paints = 0;
        for (int i = 0; i < 200; i++) {
            iris_pace_inputs in = base_inputs(t);
            in.refresh_ns = refresh;
            in.have_pulse_source = false; /* no compositor pulse available */
            in.host_animating = true;

            iris_pace_decision d = iris_pacer_tick(&p, &in);
            CHECK_NO_SPIN(d);
            CHECK(d.wait_timeout_ns > 0);
            /* Active cadence with no pulse source: exactly one refresh. */
            CHECK(d.wait_timeout_ns == iris_pacer_refresh_ns(&p));
            if (d.action == IRIS_PACE_PAINT)
                paints++;
            t += d.wait_timeout_ns;
        }
        CHECK(paints >= 199);
    }
}

/* Caret-only idle: a low-rate tick that is a whole multiple of the refresh,
 * so it cannot beat against the display, and still never spins. The tick
 * exists so lens keeps advancing its blink clock; whether a frame reaches the
 * GPU is decided by the chrome damage the build reports. */
static void test_caret_idle_tick(void) {
    iris_pacer p;
    iris_pacer_init(&p);
    iris_pacer_set_refresh(&p, HZ60_NS);

    int64_t t = 0;
    int paints = 0, ticks = 0;
    int64_t first_wait = 0;
    for (int i = 0; i < 200; i++) {
        /* lens toggles the caret and reports chrome damage on the ticks where
         * the blink state flips: the only reason to touch the GPU. */
        bool blink_toggle = (i > 0) && (i % 2 == 0);

        iris_pace_inputs in = base_inputs(t);
        in.refresh_ns = HZ60_NS;
        in.caret_visible = true; /* the only thing keeping us awake */
        in.chrome_damaged = blink_toggle;

        iris_pace_decision d = iris_pacer_tick(&p, &in);
        CHECK_NO_SPIN(d);

        if (blink_toggle) {
            /* Chrome damage is active cadence: a real frame is published. */
            CHECK(d.action == IRIS_PACE_PAINT);
        } else {
            /* Steady state: the blink clock advances, the GPU sleeps. */
            CHECK(d.action == IRIS_PACE_SKIP);
            CHECK(!d.request_pulse); /* caret tick is timer-driven, not pulsed */
            CHECK(d.wait_timeout_ns > 0);
            /* Display-aligned: an exact multiple of the refresh period. */
            CHECK(d.wait_timeout_ns % HZ60_NS == 0);
            if (ticks == 0)
                first_wait = d.wait_timeout_ns;
            else
                CHECK(d.wait_timeout_ns == first_wait);
            ticks++;
        }
        if (d.action == IRIS_PACE_PAINT)
            paints++;
        t += d.wait_timeout_ns > 0 ? d.wait_timeout_ns : HZ60_NS;
    }
    /* The idle tick is display-aligned: a whole number of 60 Hz refreshes
     * (15 x 16.667 ms ≈ 250 ms), not the literal 250 ms target. */
    CHECK(first_wait == 15 * HZ60_NS);
    CHECK(first_wait >= 250 * NS_PER_MS);
    CHECK(paints == 99);
}

/* Nothing pending: drop the subscription and block. */
static void test_quiescence(void) {
    iris_pacer p;
    iris_pacer_init(&p);

    iris_pace_inputs in = base_inputs(1000);
    iris_pace_decision d = iris_pacer_tick(&p, &in);
    CHECK(d.action == IRIS_PACE_QUIESCE);
    CHECK(!d.request_pulse);
    CHECK(d.wait_timeout_ns == 0);

    /* Stable: repeated ticks stay quiesced. */
    for (int i = 0; i < 10; i++) {
        in.now_ns += 500 * NS_PER_MS;
        d = iris_pacer_tick(&p, &in);
        CHECK(d.action == IRIS_PACE_QUIESCE);
    }

    /* Input wakes it back into a display-driven cadence. */
    in.now_ns += NS_PER_MS;
    in.input_active = true;
    d = iris_pacer_tick(&p, &in);
    CHECK(d.action != IRIS_PACE_QUIESCE);
    CHECK(d.request_pulse);
    CHECK_NO_SPIN(d);
}

/* Forced conditions outrank every "nothing changed" declaration. */
static void test_forced_paint_precedence(void) {
    iris_pacer p;
    iris_pacer_init(&p);

    /* host_skip_render alone: skip honoured, cadence kept. */
    iris_pace_inputs in = base_inputs(0);
    in.has_custom_paint = true;
    in.host_skip_render = true;
    in.host_animating = true; /* a stale request must not veto the skip  */
    in.pulse_fired = true;
    in.pulse_time_ns = 0;
    in.pulse_pending = false;
    iris_pace_decision d = iris_pacer_tick(&p, &in);
    CHECK(d.action == IRIS_PACE_SKIP);

    /* ...but chrome damage outranks the skip. */
    in.now_ns += HZ60_NS;
    in.chrome_damaged = true;
    in.pulse_fired = true;
    in.pulse_time_ns = in.now_ns;
    d = iris_pacer_tick(&p, &in);
    CHECK(d.action == IRIS_PACE_PAINT);

    /* ...and so does a discarded swapchain. */
    in.now_ns += HZ60_NS;
    in.chrome_damaged = false;
    in.surface_needs_paint = true;
    in.pulse_fired = true;
    in.pulse_time_ns = in.now_ns;
    d = iris_pacer_tick(&p, &in);
    CHECK(d.action == IRIS_PACE_PAINT);

    /* A static declaration plus a live animation request is not static:
     * the host announced its content is about to change. */
    iris_pacer p2;
    iris_pacer_init(&p2);
    iris_pace_inputs st = base_inputs(0);
    st.has_custom_paint = true;
    st.host_static = true;
    st.pulse_fired = true;
    st.pulse_pending = false;
    CHECK(iris_pacer_tick(&p2, &st).action == IRIS_PACE_SKIP);
    st.now_ns += HZ60_NS;
    st.host_animating = true;
    st.pulse_fired = true;
    st.pulse_time_ns = st.now_ns;
    CHECK(iris_pacer_tick(&p2, &st).action == IRIS_PACE_PAINT);
}

/* A lens-only host (no cfg->paint) paints on chrome damage and quiesces the
 * moment the chrome stops changing. */
static void test_lens_only_host(void) {
    iris_pacer p;
    iris_pacer_init(&p);

    iris_pace_inputs in = base_inputs(0);
    in.has_custom_paint = false;
    in.chrome_damaged = true;
    in.pulse_fired = true;
    in.pulse_pending = false;
    CHECK(iris_pacer_tick(&p, &in).action == IRIS_PACE_PAINT);

    in.now_ns += HZ60_NS;
    in.chrome_damaged = false;
    in.pulse_fired = true;
    in.pulse_time_ns = in.now_ns;
    iris_pace_decision d = iris_pacer_tick(&p, &in);
    CHECK(d.action == IRIS_PACE_QUIESCE);
}

/* The simulation clock never walks backwards, even if a pulse timestamp is
 * stale (a compositor clock that lags the local monotonic clock). */
static void test_sim_clock_monotonic_under_stale_pulses(void) {
    iris_pacer p;
    iris_pacer_init(&p);
    iris_pacer_set_refresh(&p, HZ60_NS);

    int64_t now = 10 * NS_PER_S;
    int64_t last_sim = 0;
    for (int i = 0; i < 500; i++) {
        iris_pace_inputs in = base_inputs(now);
        in.refresh_ns = HZ60_NS;
        in.host_animating = true;
        in.pulse_fired = true;
        /* Stale by 5 ms: the pulse clock trails the local clock. */
        in.pulse_time_ns = now - 5 * NS_PER_MS;
        in.pulse_pending = false;
        in.pulse_fired = false; /* no grant: must not regress either */

        iris_pace_decision d = iris_pacer_tick(&p, &in);
        CHECK_NO_SPIN(d);
        CHECK(d.sim_time_ns >= now); /* sim falls back to now, never older */
        CHECK(d.sim_time_ns >= last_sim);
        last_sim = d.sim_time_ns;
        now += HZ60_NS;
    }
}

/* The host's animation request is raised from cfg->paint, which a skip
 * iteration never runs. If the pacer treated it as edge-triggered, one
 * lockstep skip would look like "the host stopped animating" and the cadence
 * would collapse to the idle tick in the middle of an animation. */
static void test_animation_request_survives_skips(void) {
    iris_pacer p;
    iris_pacer_init(&p);
    iris_pacer_set_refresh(&p, HZ120_NS);

    int64_t t = 0;

    /* Frame 1: a grant is available, so the frame is produced. The host's
     * paint callback asks for the next one. */
    iris_pace_inputs in = base_inputs(t);
    in.has_custom_paint = true;
    in.host_animating = true; /* request raised by the previous paint */
    in.pulse_fired = true;
    in.pulse_time_ns = t;
    in.pulse_pending = false;
    CHECK(iris_pacer_tick(&p, &in).action == IRIS_PACE_PAINT);
    t += HZ120_NS;

    /* Frame 2: the paint's request has arrived, but the compositor has not
     * granted a frame yet, so this iteration skips. This is where the latch
     * must engage. */
    in = base_inputs(t);
    in.refresh_ns = HZ120_NS;
    in.has_custom_paint = true;
    in.host_animating = true;
    in.pulse_fired = false;
    in.pulse_pending = true;
    iris_pace_decision d = iris_pacer_tick(&p, &in);
    CHECK_NO_SPIN(d);
    CHECK(d.action == IRIS_PACE_SKIP);
    t += HZ120_NS;

    /* Frames 3..N: paint never ran again, so no fresh request arrives. The
     * latch must keep the display cadence alive rather than let it fall back
     * to the idle tick. */
    for (int i = 0; i < 8; i++) {
        iris_pace_inputs skip = base_inputs(t);
        skip.refresh_ns = HZ120_NS;
        skip.has_custom_paint = true;
        skip.host_animating = false; /* paint did not run: no fresh request */
        skip.pulse_fired = false;
        skip.pulse_pending = true;

        d = iris_pacer_tick(&p, &skip);
        CHECK_NO_SPIN(d);
        CHECK(d.action == IRIS_PACE_SKIP);
        CHECK(d.request_pulse);                    /* still subscribed       */
        CHECK(d.wait_timeout_ns == p.poll_cap_ns); /* not the idle tick      */
        t += HZ120_NS;
    }

    /* The grant finally arrives: the owed frame is produced, retiring the
     * latch. */
    in = base_inputs(t);
    in.refresh_ns = HZ120_NS;
    in.has_custom_paint = true;
    in.host_animating = false;
    in.pulse_fired = true;
    in.pulse_time_ns = t;
    in.pulse_pending = false;
    CHECK(iris_pacer_tick(&p, &in).action == IRIS_PACE_PAINT);
    t += HZ120_NS;

    /* Latch retired and no further request: the animation really did stop, so
     * the cadence drops to the display-aligned idle tick. */
    in = base_inputs(t);
    in.refresh_ns = HZ120_NS;
    in.has_custom_paint = true;
    d = iris_pacer_tick(&p, &in);
    CHECK_NO_SPIN(d);
    CHECK(!d.request_pulse);
    CHECK(d.wait_timeout_ns % HZ120_NS == 0);
}

int main(void) {
    test_refresh_configuration();
    test_pulse_lockstep();
    test_skip_render_storm_never_spins();
    test_timer_fallback_is_display_aligned();
    test_caret_idle_tick();
    test_quiescence();
    test_forced_paint_precedence();
    test_lens_only_host();
    test_animation_request_survives_skips();
    test_sim_clock_monotonic_under_stale_pulses();
    return TEST_REPORT();
}
