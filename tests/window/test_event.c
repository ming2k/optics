/* test_event — the pre-fold event seam (ADR-0107).
 *
 * Exercises the internal seam helper headlessly: sequence stamping, the
 * PASS / DROP dispositions, host rewrites, and the undroppable rule. The
 * helper is hidden-visibility, so event.c is compiled directly (as
 * test_a11y_util does for a11y_util.c).
 *
 * Also pins the public wiring: iris_app_opts.on_event exists and accepts
 * an iris_event_fn, and the iris/event.h vocabulary compiles under C23.
 */

#include <optics/window/event.h>
#include <optics/window/loop.h>

#include "event_internal.h"
#include "optics_test.h"

static int g_calls;
static optics_window_event_disposition g_disposition = OPTICS_WINDOW_EVENT_PASS;
static bool g_rewrite;
static float g_rewrite_x;
static bool g_clear_flags;
static optics_window_event_type g_last_type;

static optics_window_event_disposition seam_cb(optics_window_event *ev, void *user) {
    (void)user;
    g_calls++;
    g_last_type = ev->type;
    if (g_rewrite && ev->type == OPTICS_WINDOW_EVENT_POINTER_MOVE)
        ev->pointer.x = g_rewrite_x;
    if (g_clear_flags)
        ev->flags = OPTICS_WINDOW_EVENT_F_NONE;
    return g_disposition;
}

static void reset_seam(void) {
    g_calls = 0;
    g_disposition = OPTICS_WINDOW_EVENT_PASS;
    g_rewrite = false;
    g_rewrite_x = 0.0f;
    g_clear_flags = false;
    g_last_type = OPTICS_WINDOW_EVENT_NONE;
}

static optics_window_event make_move(float x, float y) {
    optics_window_event ev = {.type = OPTICS_WINDOW_EVENT_POINTER_MOVE,
                              .pointer = {.x = x, .y = y}};
    return ev;
}

int main(void) {
    /* ---- No seam: every event is delivered, zero interception ---- */
    optics_window__event_seam_configure(nullptr, nullptr);
    reset_seam();
    optics_window_event a = make_move(1.0f, 2.0f);
    optics_window_event b = make_move(3.0f, 4.0f);
    OT_EXPECT(optics_window__event_seam_emit(&a) == true);
    OT_EXPECT(optics_window__event_seam_emit(&b) == true);
    OT_EXPECT(a.sequence == 1);
    OT_EXPECT(b.sequence == 2);
    OT_EXPECT(g_calls == 0);

    /* Sequence resets when the seam is (re)configured for a new run. */
    optics_window__event_seam_configure(nullptr, nullptr);
    optics_window_event c = make_move(0.0f, 0.0f);
    OT_EXPECT(optics_window__event_seam_emit(&c) == true);
    OT_EXPECT(c.sequence == 1);

    /* ---- PASS delivers; DROP discards ---- */
    optics_window__event_seam_configure(seam_cb, nullptr);
    reset_seam();
    optics_window_event d = make_move(5.0f, 6.0f);
    OT_EXPECT(optics_window__event_seam_emit(&d) == true);
    OT_EXPECT(g_calls == 1);
    OT_EXPECT(g_last_type == OPTICS_WINDOW_EVENT_POINTER_MOVE);

    g_disposition = OPTICS_WINDOW_EVENT_DROP;
    optics_window_event e = make_move(7.0f, 8.0f);
    OT_EXPECT(optics_window__event_seam_emit(&e) == false);
    OT_EXPECT(g_calls == 2);

    /* ---- Host rewrites are visible to the caller ---- */
    reset_seam();
    g_rewrite = true;
    g_rewrite_x = 42.5f;
    optics_window_event f = make_move(9.0f, 10.0f);
    OT_EXPECT(optics_window__event_seam_emit(&f) == true);
    OT_EXPECT(f.pointer.x == 42.5f);

    /* ---- Undroppable: observed but cannot be dropped ---- */
    reset_seam();
    g_disposition = OPTICS_WINDOW_EVENT_DROP;
    optics_window_event g = {.type = OPTICS_WINDOW_EVENT_CLOSE,
                             .flags = OPTICS_WINDOW_EVENT_F_UNDROPPABLE};
    OT_EXPECT(optics_window__event_seam_emit(&g) == true);
    OT_EXPECT(g_calls == 1); /* host still got to observe it */

    /* ---- Undroppable survives a host that clears the flag mid-call ---- */
    reset_seam();
    g_disposition = OPTICS_WINDOW_EVENT_DROP;
    g_clear_flags = true;
    optics_window_event h = {.type = OPTICS_WINDOW_EVENT_RESIZE,
                             .flags = OPTICS_WINDOW_EVENT_F_UNDROPPABLE};
    OT_EXPECT(optics_window__event_seam_emit(&h) == true);

    /* ---- Droppable event with the flag cleared is still droppable ---- */
    reset_seam();
    g_disposition = OPTICS_WINDOW_EVENT_DROP;
    optics_window_event i = {.type = OPTICS_WINDOW_EVENT_KEY};
    OT_EXPECT(optics_window__event_seam_emit(&i) == false);

    optics_window__event_seam_clear();

    /* ---- Public wiring compiles and stores the callback ---- */
    optics_window_opts cfg = {.on_event = seam_cb};
    OT_EXPECT(cfg.on_event == seam_cb);

    /* ---- Vocabulary sanity (lens values reused, not redefined) ---- */
    optics_window_event k = {
        .type = OPTICS_WINDOW_EVENT_KEY,
        .keyboard = {.key = OPTICS_WINDOW_KEY_F5, .pressed = true, .mods = OPTICS_WINDOW_MOD_CTRL}};
    OT_EXPECT(k.keyboard.key == 284);
    OT_EXPECT(k.keyboard.mods == OPTICS_WINDOW_MOD_CTRL);

    optics_window_event m = {.type = OPTICS_WINDOW_EVENT_MODIFIERS,
                             .modifiers = {.mods = OPTICS_WINDOW_MOD_ALT}};
    OT_EXPECT(m.modifiers.mods == OPTICS_WINDOW_MOD_ALT);

    return OT_REPORT();
}
