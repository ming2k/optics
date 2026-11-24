/*
 * test_frame_pipeline_seam.c — integrated headless simulation of liboptics_window's
 * dual-thread frame loop architecture (ADR-0107 + ADR-0122 + ADR-0124).
 *
 * Wires together all four platform-neutral core modules without needing a live
 * compositor:
 *   1. Pre-fold event seam (event.c): sequence stamping, host filtering, and
 *      undroppable guarantee;
 *   2. Display pacer (display_pacer.c): pulse-driven animation, skip-render
 *      no-spin guarantee (wait_timeout_ns >= 1), and idle quiesce;
 *   3. Triple-buffered lockless frame mailbox (frame_mailbox.c): main-thread
 *      publish -> presenter-thread claim with monotonic sequences;
 *   4. Cross-thread wakeup FIFO (platform_wakeup.c): presenter notifies the
 *      main loop when frames finish presenting.
 */

#include "display_pacer.h"
#include "event_internal.h"
#include "frame_mailbox.h"
#include "optics_test.h"
#include "platform_wakeup.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct sim_state {
    optics_window_frame_mailbox mailbox;
    atomic_bool stop;
    atomic_int kicks;
    atomic_int presented_count;
    atomic_int completions_drained;
    uint64_t last_presented_seq;
} sim_state;

static void sim_kick(void *user) {
    sim_state *s = user;
    atomic_fetch_add_explicit(&s->kicks, 1, memory_order_relaxed);
}

static void on_frame_presented(void *user) {
    sim_state *s = user;
    atomic_fetch_add_explicit(&s->completions_drained, 1, memory_order_relaxed);
}

static void *presenter_thread(void *arg) {
    sim_state *s = arg;
    for (;;) {
        optics_window_frame_packet *pkt = optics_window_mailbox_claim(&s->mailbox);
        if (pkt) {
            OT_EXPECT(pkt->sequence > s->last_presented_seq);
            OT_EXPECT(pkt->width == 1280u && pkt->height == 720u);
            s->last_presented_seq = pkt->sequence;
            atomic_fetch_add_explicit(&s->presented_count, 1, memory_order_relaxed);
            (void)optics_window_platform_wakeup_post(on_frame_presented, s);
            continue;
        }
        if (atomic_load_explicit(&s->stop, memory_order_acquire))
            break;
        sched_yield();
    }
    return nullptr;
}

typedef struct event_filter_ctx {
    int seen;
    int dropped;
    uint64_t last_seq;
} event_filter_ctx;

static optics_window_event_disposition filter_cb(optics_window_event *ev, void *user) {
    event_filter_ctx *fx = user;
    fx->seen++;
    OT_EXPECT(ev->sequence > fx->last_seq);
    fx->last_seq = ev->sequence;
    /* Attempt to drop every event; undroppable ones must still survive. */
    fx->dropped++;
    return OPTICS_WINDOW_EVENT_DROP;
}

int main(void) {
    sim_state sim = {};
    optics_window_mailbox_init(&sim.mailbox, nullptr);
    optics_window_platform_wakeup_set_kick(sim_kick, &sim);

    event_filter_ctx ev_ctx = {};
    optics_window__event_seam_configure(filter_cb, &ev_ctx);

    pthread_t presenter;
    OT_REQUIRE(pthread_create(&presenter, nullptr, presenter_thread, &sim) == 0);

    optics_window_pacer pacer = {};
    optics_window_pacer_init(&pacer);
    const int64_t refresh_ns = 8333333LL; /* 120 Hz */
    optics_window_pacer_set_refresh(&pacer, refresh_ns);
    OT_EXPECT(optics_window_pacer_refresh_ns(&pacer) == refresh_ns);

    int64_t now_ns = 1000000000LL;
    int painted_frames = 0;
    int skipped_frames = 0;

    /* Simulate 40 display intervals:
     *   - frames 0..19: input + chrome damage -> paints published to mailbox
     *   - frames 20..29: host_skip_render -> zero-render skip, no busy-spin
     *   - frames 30..39: idle -> quiesces cleanly */
    for (int step = 0; step < 40; ++step) {
        now_ns += refresh_ns;

        /* Emit a droppable pointer event and an undroppable close/resize-style event. */
        optics_window_event droppable = {.type = OPTICS_WINDOW_EVENT_POINTER_BUTTON, .flags = 0};
        OT_EXPECT(!optics_window__event_seam_emit(&droppable));

        optics_window_event undroppable = {.type = OPTICS_WINDOW_EVENT_RESIZE,
                                           .flags = OPTICS_WINDOW_EVENT_F_UNDROPPABLE};
        OT_EXPECT(optics_window__event_seam_emit(&undroppable));

        optics_window_pace_inputs pin = {
            .now_ns = now_ns,
            .pulse_time_ns = now_ns,
            .refresh_ns = refresh_ns,
            .pulse_fired = (step < 30),
            .pulse_pending = false,
            .have_pulse_source = true,
            .input_active = (step < 20),
            .host_animating = (step < 30),
            .host_skip_render = (step >= 20 && step < 30),
            .has_host_content = true,
            .chrome_damaged = (step < 20),
            .surface_needs_paint = (step == 0),
        };
        optics_window_pace_decision dec = optics_window_pacer_tick(&pacer, &pin);

        if (dec.action == OPTICS_WINDOW_PACE_PAINT) {
            OT_EXPECT(dec.wait_timeout_ns >= 1);
            optics_window_frame_packet *pkt = optics_window_mailbox_acquire(&sim.mailbox);
            OT_REQUIRE(pkt != nullptr);
            pkt->width = 1280u;
            pkt->height = 720u;
            pkt->scale = 1.0f;
            pkt->damage_count = 1u;
            pkt->damage[0] = (optics_gfx_rect){0, 0, 1280.0f, 720.0f};
            optics_window_mailbox_publish(&sim.mailbox, pkt);
            painted_frames++;
        } else if (dec.action == OPTICS_WINDOW_PACE_SKIP) {
            /* INV-PACE-03: skip must never emit wait_timeout_ns <= 0. */
            OT_EXPECT(dec.wait_timeout_ns >= 1);
            skipped_frames++;
        } else {
            OT_EXPECT(dec.action == OPTICS_WINDOW_PACE_QUIESCE);
            OT_EXPECT(dec.wait_timeout_ns == 0);
        }

        optics_window_platform_wakeup_drain();
    }

    OT_EXPECT(ev_ctx.seen == 80);
    OT_EXPECT(painted_frames >= 15);
    OT_EXPECT(skipped_frames >= 5);

    /* Stop presenter thread and drain all remaining wakeup completions. */
    atomic_store_explicit(&sim.stop, true, memory_order_release);
    OT_EXPECT(pthread_join(presenter, nullptr) == 0);
    optics_window_platform_wakeup_drain();

    int presented = atomic_load(&sim.presented_count);
    int drained = atomic_load(&sim.completions_drained);
    OT_EXPECT(presented >= 1);
    OT_EXPECT(presented <= painted_frames);
    OT_EXPECT(drained == presented);

    optics_window__event_seam_clear();
    optics_window_platform_wakeup_set_kick(nullptr, nullptr);
    optics_window_mailbox_destroy(&sim.mailbox);
    return OT_REPORT();
}
