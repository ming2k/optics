/* display_pacer.h — the frame-cadence state machine (ADR-0122).
 *
 * ============================================================================
 *  Why this exists
 * ============================================================================
 *
 * Optics used to pace presentation from the client side: each backend slept in
 * its own event loop for a hardcoded ~60 Hz period (`ACTIVE_PERIOD_NS`) and
 * re-derived the next wake-up from the timestamp of the *last real paint*.
 * That produced three structural failures (ADR-0122 §Context):
 *
 *   1. Phase drift. The client's monotonic clock and the display's VBlank
 *      never agree exactly, so a client that "counts to 16.6 ms on its own"
 *      beats against the compositor and drops/piles up frames. The user sees
 *      irregular micro-stutter, not a steady frame rate.
 *   2. High-refresh degradation. A hardcoded 60 Hz period caps a 120/144 Hz
 *      display and lands on non-integer frame multiples.
 *   3. A zero-render skip left the scheduling baseline frozen in the past,
 *      so `next_deadline = last_render_ns + period` fell before *now*, the
 *      poll budget went to 0, and the loop busy-spun a full CPU core.
 *
 * The pacer is the single authority for cadence. It is deliberately a *pure*
 * state machine: no platform headers, no clock, no allocation, no I/O. The
 * backend owns the platform VSync source and feeds pulses in; the pacer
 * answers "paint, skip, or sleep — and until when".
 *
 * ============================================================================
 *  Cadence model
 * ============================================================================
 *
 * Two driving modes, chosen per tick:
 *
 *   Pulse-driven (preferred). While input is warm, an animation is in flight,
 *   chrome is damaged, or the host is streaming, the backend subscribes to the
 *   compositor's VSync pulse (`wl_surface.frame` on Wayland, the display link
 *   on macOS, the waitable swapchain on Win32). The pacer paints *only* when a
 *   pulse has actually been delivered — lockstep with the display, which is
 *   what removes phase drift and unlocks high refresh rates for free
 *   (INV-PACE-01, INV-PACE-02).
 *
 *   Timer-driven (fallback). For the caret-blink idle tick, and for platform
 *   paths that cannot deliver a pulse, the pacer emits a display-aligned
 *   deadline: a whole number of refresh intervals, so the client never
 *   produces frames at a rate coprime with the display.
 *
 * ============================================================================
 *  Invariants (ADR-0122)
 * ============================================================================
 *
 * `[INV-PACE-03]`: every non-quiescent decision carries `wait_timeout_ns >= 1`.
 * That is a hard postcondition of `iris_pacer_tick` and the reason the old
 * 0 ms-poll spin is structurally impossible: the schedule can only move
 * forward (in whole display intervals), and it is never re-derived from a
 * render timestamp. A zero-render skip advances the cadence exactly like a
 * paint does.
 *
 * The simulation clock (`sim_time_ns`) is the compositor's pulse time when one
 * was delivered, else the wall clock. It is never derived from a previous
 * render timestamp either — animations are evaluated against the moment the
 * frame is projected to reach the display, not against when the last frame
 * finished (INV-PACE-04).
 */
#ifndef IRIS_DISPLAY_PACER_H
#define IRIS_DISPLAY_PACER_H

#include <stdbool.h>
#include <stdint.h>

/* What the caller should do with this iteration. */
typedef enum iris_pace_action {
    /* Render and present a frame. */
    IRIS_PACE_PAINT = 0,
    /* Do not render; keep the schedule (or the pulse subscription) alive.
     * This is the zero-render skip: the cadence advances, the GPU sleeps. */
    IRIS_PACE_SKIP = 1,
    /* Nothing is pending: drop the pulse subscription and block until an
     * external wake (input, wakeup seam, accessibility, a timeout cap). */
    IRIS_PACE_QUIESCE = 2,
} iris_pace_action;

/* One iteration's worth of platform/application observations. */
typedef struct iris_pace_inputs {
    /* Monotonic clock (CLOCK_MONOTONIC on POSIX). Always required. */
    int64_t now_ns;

    /* Compositor pulse time for this iteration, in the same monotonic clock.
     * Zero when no pulse was delivered. On Wayland this is `wl_callback.done`;
     * on macOS the display-link timestamp; on Win32 the present time. */
    int64_t pulse_time_ns;

    /* Display refresh period. Zero or out of range keeps the last known value
     * (a backend that has not learned the refresh rate yet). */
    int64_t refresh_ns;

    /* A compositor pulse was consumed this iteration. */
    bool pulse_fired;

    /* A pulse subscription is outstanding (requested, not yet fired). This is
     * what keeps the cadence in lockstep: while one is pending the pacer
     * refuses to draw even if some unrelated event woke the loop, so a burst
     * of wakeups cannot publish two frames into the same refresh interval. */
    bool pulse_pending;

    /* This backend can deliver pulses. False makes the pacer substitute a
     * display-aligned timer, which preserves the no-spin invariant. */
    bool have_pulse_source;

    /* Genuine user input (pointer/keyboard/scroll/IME) since the last tick.
     * Frame-callback and buffer-release traffic must NOT set this. */
    bool input_active;

    /* The host asked for another frame (iris_request_animation_frame). */
    bool host_animating;

    /* An eased value / spring is still in flight inside lens
     * (lens_anim_pending). Keeps the cadence warm; it does not by itself
     * force a paint — the chrome damage the build reports does. */
    bool lens_animating;

    /* The host declared this frame a zero-render skip
     * (iris_request_frame_skip_render): canvas unchanged, cadence continues. */
    bool host_skip_render;

    /* The host declared this frame's canvas static (iris_paint_mark_static). */
    bool host_static;

    /* The host supplies canvas content via cfg->paint / cfg->prepare. Hosts
     * without one only ever paint on lens chrome damage. */
    bool has_custom_paint;

    /* lens_frame_needs_repaint(): the UI chrome itself changed. */
    bool chrome_damaged;

    /* A focused text caret is blinking. */
    bool caret_visible;

    /* The swapchain contents were discarded (resize / scale change / first
     * frame). Forces a paint regardless of any skip declaration. */
    bool surface_needs_paint;
} iris_pace_inputs;

/* The pacer's verdict for this iteration. */
typedef struct iris_pace_decision {
    iris_pace_action action;

    /* Subscribe to the next compositor pulse. The backend requests
     * `wl_surface.frame` before the next commit when this is set. */
    bool request_pulse;

    /* Animation clock for this iteration, in the monotonic clock domain. */
    int64_t sim_time_ns;

    /* Poll budget in nanoseconds. Guaranteed `>= 1` unless the action is
     * IRIS_PACE_QUIESCE, in which case it is 0 and the caller blocks with its
     * own cap (INV-PACE-03). */
    int64_t wait_timeout_ns;
} iris_pace_decision;

/* Pacing tunables. Exposed for tests / hosts that need to retune; the
 * defaults are what the backends use. */
typedef struct iris_pacer {
    int64_t refresh_ns;     /* display period; always in [MIN, MAX]        */
    int64_t grace_ns;       /* stay active this long after input           */
    int64_t idle_target_ns; /* caret-tick target; rounded up to a multiple */
    int64_t poll_cap_ns;    /* max sleep while subscribed to pulses        */
    int64_t last_input_ns;  /* last genuine input                          */
    int64_t idle_next_ns;   /* next timer-driven tick (0 = unarmed)        */
    bool have_input;        /* input seen at least once (arms the grace)   */
    bool idle_armed;        /* idle_next_ns holds a valid deadline         */
    /* The host asked for another frame and has not been given one yet.
     * Latched rather than edge-triggered because the request normally comes
     * from cfg->paint, which does not run on a skip iteration: without the
     * latch a single lockstep skip would look like "the host stopped
     * animating" and drop the cadence to the idle tick mid-animation. */
    bool animating;
} iris_pacer;

/* 60 Hz is the *fallback* period for a backend that has not yet learned the
 * real refresh rate — never a hardcoded target (INV-PACE-02). */
#define IRIS_PACE_DEFAULT_REFRESH_NS 16666667LL
/* 1000 Hz..10 Hz bounds: below/above these a "refresh rate" is a bug, not a
 * display. Out-of-range values are clamped so the pacer can never be driven
 * into a spin by a garbage pulse. */
#define IRIS_PACE_REFRESH_MIN_NS 1000000LL
#define IRIS_PACE_REFRESH_MAX_NS 100000000LL

void iris_pacer_init(iris_pacer *p);
void iris_pacer_set_refresh(iris_pacer *p, int64_t refresh_ns);
int64_t iris_pacer_refresh_ns(const iris_pacer *p);

/* Advance one iteration. Called by the backend after it has drained platform
 * events (so `pulse_fired` / `input_active` reflect this iteration) and before
 * it decides whether to render. */
[[nodiscard]] iris_pace_decision iris_pacer_tick(iris_pacer *p, const iris_pace_inputs *in);

#endif /* IRIS_DISPLAY_PACER_H */
