/* test_a11y_activate.c — AT activation seam (ADR-0062).
 *
 * lens_a11y_activate records a pending id; the next frame's lensi_interact
 * reports clicked for that node (focusable + not disabled), moves focus,
 * and the request is single-shot. Pointer occlusion does not block it;
 * disabled does.
 */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Helper: one frame with a single button; returns the button's id and
 * whether it reported clicked. */
static lens_id build_button_frame(lens *ui, bool *clicked) {
    lens_begin(ui, &IN0);
    *clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    lens_id id = lens_get_response(ui).id;
    lens_end(ui);
    return id;
}

/* Activation fires on the next frame, moves focus, and is single-shot. */
static void test_activate_fires_and_focuses(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool clicked = false;
    lens_id id = build_button_frame(ui, &clicked);
    CHECK(id != 0);
    CHECK(!clicked);
    CHECK(!lens_focused(ui, id));

    /* The request lands between frames (the AT-SPI pump runs after
     * lens_end); the upcoming build must report the click. */
    lens_a11y_activate(ui, id);
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(clicked);
    CHECK(lens_focused(ui, id));

    /* Single-shot: the following frames see no further clicks. */
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(!clicked);
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(!clicked);

    lens_destroy(ui);
}

/* A request for a nonexistent id is dropped at frame end and never fires. */
static void test_activate_unknown_id_dropped(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool clicked = false;
    lens_id id = build_button_frame(ui, &clicked);
    (void)id;

    lens_a11y_activate(ui, 0xDEADBEEF);
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(!clicked);
    /* …and does not linger into a later frame either. */
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(!clicked);

    lens_destroy(ui);
}

/* A newer request replaces an unconsumed older one (single pending slot). */
static void test_activate_replaces_pending(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool clicked = false;
    lens_id id = build_button_frame(ui, &clicked);

    lens_a11y_activate(ui, 0xDEADBEEF); /* stale, then replaced */
    lens_a11y_activate(ui, id);
    clicked = false;
    build_button_frame(ui, &clicked);
    CHECK(clicked);

    lens_destroy(ui);
}

/* Disabled widgets do not activate, and the request is consumed/dropped so
 * it cannot fire later if the widget becomes enabled again mid-frame. */
static void test_activate_disabled_blocked(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool disabled = true;
    lens_id id = 0;
    bool clicked = false;

    /* frame 1: disabled button */
    lens_begin(ui, &IN0);
    clicked =
        lens_button(ui, &(lens_button_opts){.box = {.disabled = disabled}, .label = "No"}).clicked;
    id = lens_get_response(ui).id;
    lens_end(ui);
    CHECK(id != 0);
    CHECK(!clicked);

    /* activation request → blocked by disabled; no focus move */
    lens_a11y_activate(ui, id);
    lens_begin(ui, &IN0);
    clicked =
        lens_button(ui, &(lens_button_opts){.box = {.disabled = disabled}, .label = "No"}).clicked;
    lens_end(ui);
    CHECK(!clicked);
    CHECK(!lens_focused(ui, id));

    /* enabled again: the dropped request must NOT fire retroactively */
    disabled = false;
    lens_begin(ui, &IN0);
    clicked =
        lens_button(ui, &(lens_button_opts){.box = {.disabled = disabled}, .label = "No"}).clicked;
    lens_end(ui);
    CHECK(!clicked);

    /* fresh request now fires */
    lens_a11y_activate(ui, id);
    lens_begin(ui, &IN0);
    clicked =
        lens_button(ui, &(lens_button_opts){.box = {.disabled = disabled}, .label = "No"}).clicked;
    lens_end(ui);
    CHECK(clicked);
    CHECK(lens_focused(ui, id));

    lens_destroy(ui);
}

/* Occlusion does not block AT activation: a transient popup over the
 * button swallows its pointer interaction but not the a11y request. */
static void test_activate_bypasses_occlusion(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: button under an open full-window popup (POPUP band
     * occludes; a BACKDROP would be hit-transparent) */
    bool clicked = false;
    lens_begin(ui, &IN0);
    clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    lens_id id = lens_get_response(ui).id;
    lens_place_open(ui, "cover");
    if (lens_place_begin(ui, &(lens_place_opts){.box = {.id = "cover"},
                                                .band = LENS_BAND_POPUP,
                                                .mode = LENS_PLACE_EXACT,
                                                .rect = {0, 0, 400, 200},
                                                .transient = true})) {
        lens_size(ui, 400, 200);
        lens_label(ui, &(lens_label_opts){.text = "cover"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(!clicked);

    /* frame 2: pointer press over the covered button — occluded, and the
     * press is inside the popup so it does not even dismiss it */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    bool pressed = lens_get_response(ui).pressed;
    if (lens_place_begin(ui, &(lens_place_opts){.box = {.id = "cover"},
                                                .band = LENS_BAND_POPUP,
                                                .mode = LENS_PLACE_EXACT,
                                                .rect = {0, 0, 400, 200},
                                                .transient = true})) {
        lens_size(ui, 400, 200);
        lens_label(ui, &(lens_label_opts){.text = "cover"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(!clicked);
    CHECK(!pressed);

    /* frame 3: AT activation fires anyway */
    lens_a11y_activate(ui, id);
    lens_begin(ui, &IN0);
    clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    if (lens_place_begin(ui, &(lens_place_opts){.box = {.id = "cover"},
                                                .band = LENS_BAND_POPUP,
                                                .mode = LENS_PLACE_EXACT,
                                                .rect = {0, 0, 400, 200},
                                                .transient = true})) {
        lens_size(ui, 400, 200);
        lens_label(ui, &(lens_label_opts){.text = "cover"});
        lens_place_end(ui);
    }
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

int main(void) {
    test_activate_fires_and_focuses();
    test_activate_unknown_id_dropped();
    test_activate_replaces_pending();
    test_activate_disabled_blocked();
    test_activate_bypasses_occlusion();
    return TEST_REPORT();
}
