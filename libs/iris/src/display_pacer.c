/* display_pacer.c — the frame-cadence state machine (ADR-0122).
 *
 * Pure: no clock, no platform headers, no allocation, no I/O. See
 * display_pacer.h for the model and the invariants this file must uphold. */
#include "display_pacer.h"

static int64_t clamp_refresh(int64_t refresh_ns) {
    if (refresh_ns <= 0)
        return 0; /* "unknown" — caller keeps its previous period */
    if (refresh_ns < IRIS_PACE_REFRESH_MIN_NS)
        return IRIS_PACE_REFRESH_MIN_NS;
    if (refresh_ns > IRIS_PACE_REFRESH_MAX_NS)
        return IRIS_PACE_REFRESH_MAX_NS;
    return refresh_ns;
}

void iris_pacer_init(iris_pacer *p) {
    *p = (iris_pacer){
        .refresh_ns = IRIS_PACE_DEFAULT_REFRESH_NS,
        .grace_ns = 400000000LL,       /* 400 ms of warm input after a move  */
        .idle_target_ns = 250000000LL, /* caret tick target (≈4 Hz at 60 Hz) */
        .poll_cap_ns = 200000000LL,    /* bounds unrelated work while pulsed */
    };
}

void iris_pacer_set_refresh(iris_pacer *p, int64_t refresh_ns) {
    int64_t r = clamp_refresh(refresh_ns);
    if (r > 0)
        p->refresh_ns = r;
}

int64_t iris_pacer_refresh_ns(const iris_pacer *p) {
    return p->refresh_ns;
}

/* Idle tick: the smallest whole number of display intervals that is at least
 * `idle_target_ns`. Keeping it a multiple of the refresh is what stops the
 * caret tick from beating against the display (ADR-0122 §4). */
static int64_t idle_period_ns(const iris_pacer *p) {
    int64_t refresh = p->refresh_ns > 0 ? p->refresh_ns : IRIS_PACE_DEFAULT_REFRESH_NS;
    int64_t target = p->idle_target_ns > 0 ? p->idle_target_ns : refresh;
    int64_t periods = (target + refresh - 1) / refresh;
    if (periods < 1)
        periods = 1;
    return periods * refresh;
}

iris_pace_decision iris_pacer_tick(iris_pacer *p, const iris_pace_inputs *in) {
    if (in->refresh_ns > 0)
        iris_pacer_set_refresh(p, in->refresh_ns);

    int64_t now = in->now_ns;

    if (in->input_active) {
        p->last_input_ns = now;
        p->have_input = true;
    }
    /* The grace window is armed by real input only. A pacer that has never
     * seen input must not claim to be "warm" just because both clocks start
     * near zero — that would defeat quiescence at startup (INV-PACE-01). */
    bool grace = p->have_input && (now - p->last_input_ns) < p->grace_ns;

    /* Animation clock (INV-PACE-04): the compositor's pulse time when one was
     * delivered, else the wall clock. Never a previous *render* timestamp. */
    int64_t sim = in->pulse_fired ? in->pulse_time_ns : now;
    if (sim <= 0)
        sim = now;

    /* Conditions that override any "nothing changed" declaration: they alter
     * what is physically on screen, so a skip cannot hide them. */
    bool forced = in->surface_needs_paint || in->chrome_damaged;

    /* Latch the host's animation request. It is normally raised from
     * cfg->paint, which a skip iteration never runs — so it must survive
     * until the frame it asks for is actually produced (cleared below on a
     * PAINT), or the cadence collapses mid-animation. */
    if (in->host_animating)
        p->animating = true;
    bool animating = p->animating;

    /* Is the display actively driving frames? The caret tick is deliberately
     * excluded — a blinking caret does not justify a display-locked cadence. */
    bool cadence_active = grace || animating || in->host_skip_render || in->surface_needs_paint ||
                          in->chrome_damaged || in->lens_animating;

    /* Is a frame pending at all? `has_custom_paint` keeps at least the low
     * idle tick alive: such a host's content is opaque to lens, so running
     * cfg->build is how it observes its own state changes and resumes
     * animating. Only a lens-only host can reach full quiescence. */
    bool cadence_any = cadence_active || in->caret_visible || in->has_custom_paint;

    if (!cadence_any) {
        p->idle_armed = false;
        return (iris_pace_decision){
            .action = IRIS_PACE_QUIESCE,
            .request_pulse = false,
            .sim_time_ns = sim,
            .wait_timeout_ns = 0, /* caller blocks with its own cap */
        };
    }

    /* Paint / skip derivation. Precedence is unchanged from the backends'
     * pre-pacer behaviour, so only the schedule moved here. A pending
     * animation request outranks a static declaration: a host that asked for
     * the next frame has announced its content is about to change. */
    bool skip_render = in->has_custom_paint && in->host_skip_render && !forced;
    bool canvas_static = in->has_custom_paint && in->host_static && !animating && !forced;
    bool paint = !skip_render && !canvas_static && (in->has_custom_paint || forced || animating);

    if (cadence_active && in->have_pulse_source) {
        /* Pulse-driven (INV-PACE-01/02). The pacer never invents a period
         * here: the compositor's pulse *is* the schedule, so the old
         * `last_render_ns + period` arithmetic — and the past-deadline spin it
         * caused — does not exist on this path. */
        p->idle_armed = false;
        /* Draw only on an actual grant, or when no grant is outstanding (the
         * first commit is what makes the compositor start pulsing). */
        bool may_draw = in->pulse_fired || !in->pulse_pending;
        bool draw = may_draw && paint;
        if (draw)
            p->animating = false; /* the owed frame is being produced */
        return (iris_pace_decision){
            .action = draw ? IRIS_PACE_PAINT : IRIS_PACE_SKIP,
            .request_pulse = true,
            .sim_time_ns = sim,
            .wait_timeout_ns = p->poll_cap_ns,
        };
    }

    /* Timer-driven: the caret tick, or a backend with no pulse source. The
     * deadline is a whole number of display intervals from *now* on the tick
     * that arms it, so it can never land in the past (INV-PACE-03). */
    int64_t period = cadence_active ? p->refresh_ns : idle_period_ns(p);
    if (!p->idle_armed) {
        p->idle_next_ns = now + period;
        p->idle_armed = true;
    }
    bool due = now >= p->idle_next_ns;
    if (due)
        p->idle_next_ns = now + period;

    int64_t wait = p->idle_next_ns - now;
    if (wait < 1)
        wait = 1; /* hard postcondition: a non-quiescent tick never spins */

    bool draw = due && paint;
    if (draw)
        p->animating = false;
    return (iris_pace_decision){
        .action = draw ? IRIS_PACE_PAINT : IRIS_PACE_SKIP,
        .request_pulse = false,
        .sim_time_ns = sim,
        .wait_timeout_ns = wait,
    };
}
