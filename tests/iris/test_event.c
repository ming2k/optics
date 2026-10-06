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

#include <iris/app.h>
#include <iris/event.h>

#include "event_internal.h"
#include "test_helpers.h"

static int g_calls;
static iris_event_disposition g_disposition = IRIS_EVENT_PASS;
static bool g_rewrite;
static float g_rewrite_x;
static bool g_clear_flags;
static iris_event_type g_last_type;

static iris_event_disposition seam_cb(iris_event *ev, void *user) {
    (void)user;
    g_calls++;
    g_last_type = ev->type;
    if (g_rewrite && ev->type == IRIS_EVENT_POINTER_MOVE)
        ev->pointer.x = g_rewrite_x;
    if (g_clear_flags)
        ev->flags = IRIS_EVENT_F_NONE;
    return g_disposition;
}

static void reset_seam(void) {
    g_calls = 0;
    g_disposition = IRIS_EVENT_PASS;
    g_rewrite = false;
    g_rewrite_x = 0.0f;
    g_clear_flags = false;
    g_last_type = IRIS_EVENT_NONE;
}

static iris_event make_move(float x, float y) {
    iris_event ev = {.type = IRIS_EVENT_POINTER_MOVE, .pointer = {.x = x, .y = y}};
    return ev;
}

int main(void) {
    /* ---- No seam: every event is delivered, zero interception ---- */
    iris__event_seam_configure(NULL, NULL);
    reset_seam();
    iris_event a = make_move(1.0f, 2.0f);
    iris_event b = make_move(3.0f, 4.0f);
    CHECK(iris__event_seam_emit(&a) == true);
    CHECK(iris__event_seam_emit(&b) == true);
    CHECK(a.sequence == 1);
    CHECK(b.sequence == 2);
    CHECK(g_calls == 0);

    /* Sequence resets when the seam is (re)configured for a new run. */
    iris__event_seam_configure(NULL, NULL);
    iris_event c = make_move(0.0f, 0.0f);
    CHECK(iris__event_seam_emit(&c) == true);
    CHECK(c.sequence == 1);

    /* ---- PASS delivers; DROP discards ---- */
    iris__event_seam_configure(seam_cb, NULL);
    reset_seam();
    iris_event d = make_move(5.0f, 6.0f);
    CHECK(iris__event_seam_emit(&d) == true);
    CHECK(g_calls == 1);
    CHECK(g_last_type == IRIS_EVENT_POINTER_MOVE);

    g_disposition = IRIS_EVENT_DROP;
    iris_event e = make_move(7.0f, 8.0f);
    CHECK(iris__event_seam_emit(&e) == false);
    CHECK(g_calls == 2);

    /* ---- Host rewrites are visible to the caller ---- */
    reset_seam();
    g_rewrite = true;
    g_rewrite_x = 42.5f;
    iris_event f = make_move(9.0f, 10.0f);
    CHECK(iris__event_seam_emit(&f) == true);
    CHECK(f.pointer.x == 42.5f);

    /* ---- Undroppable: observed but cannot be dropped ---- */
    reset_seam();
    g_disposition = IRIS_EVENT_DROP;
    iris_event g = {.type = IRIS_EVENT_CLOSE, .flags = IRIS_EVENT_F_UNDROPPABLE};
    CHECK(iris__event_seam_emit(&g) == true);
    CHECK(g_calls == 1); /* host still got to observe it */

    /* ---- Undroppable survives a host that clears the flag mid-call ---- */
    reset_seam();
    g_disposition = IRIS_EVENT_DROP;
    g_clear_flags = true;
    iris_event h = {.type = IRIS_EVENT_RESIZE, .flags = IRIS_EVENT_F_UNDROPPABLE};
    CHECK(iris__event_seam_emit(&h) == true);

    /* ---- Droppable event with the flag cleared is still droppable ---- */
    reset_seam();
    g_disposition = IRIS_EVENT_DROP;
    iris_event i = {.type = IRIS_EVENT_KEY};
    CHECK(iris__event_seam_emit(&i) == false);

    iris__event_seam_clear();

    /* ---- Public wiring compiles and stores the callback ---- */
    iris_app_config cfg = {.on_event = seam_cb};
    CHECK(cfg.on_event == seam_cb);

    /* ---- Vocabulary sanity (lens values reused, not redefined) ---- */
    iris_event k = {.type = IRIS_EVENT_KEY,
                    .keyboard = {.key = LENS_KEY_F5, .pressed = true, .mods = LENS_MOD_CTRL}};
    CHECK(k.keyboard.key == 284);
    CHECK(k.keyboard.mods == LENS_MOD_CTRL);

    iris_event m = {.type = IRIS_EVENT_MODIFIERS, .modifiers = {.mods = LENS_MOD_ALT}};
    CHECK(m.modifiers.mods == LENS_MOD_ALT);

    return TEST_REPORT();
}
