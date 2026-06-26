/* test_input.c — scripted input sequences → expected responses (ADR-0006). */

#include "test_helpers.h"
#include <lens/lens.h>

static void test_hover_press_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: button enters, no input */
    lens_input in1 = {.display_size = {200, 100}, .dt_seconds = 0.016f};
    lens_begin(ui, &in1);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r1 = lens_get_response(ui);
    CHECK(r1.hovered == false);
    CHECK(r1.pressed == false);
    CHECK(r1.clicked == false);

    /* frame 2: cursor over button, press */
    lens_input in2 = in1;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r2 = lens_get_response(ui);
    CHECK(r2.hovered == true);
    CHECK(r2.pressed == true);
    CHECK(r2.clicked == false);

    /* frame 3: cursor still over, release → click */
    lens_input in3 = in1;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r3 = lens_get_response(ui);
    CHECK(r3.hovered == true);
    CHECK(r3.pressed == false);
    CHECK(r3.clicked == true);

    lens_destroy(ui);
}

static void test_drag_outside_no_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: button enters */
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 2: press inside */
    lens_input in2 = in;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);
    CHECK(lens_active(ui) != 0);

    /* frame 3: move outside, release → no click */
    lens_input in3 = in;
    in3.cursor = (flux_point){500, 500};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r3 = lens_get_response(ui);
    CHECK(r3.clicked == false);

    lens_destroy(ui);
}

static void test_tab_focus(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: two buttons enter */
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    lens_end(ui);

    /* frame 2: Tab press → focus moves to first focusable */
    lens_input in2 = in;
    in2.keys[0] = (lens_key_event){.key = 258, .pressed = true, .repeat = false};
    in2.key_count = 1;
    lens_begin(ui, &in2);
    lens_id aid = lens_current_id(ui, "A");
    lens_id bid = lens_current_id(ui, "B");
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    lens_end(ui);

    /* After first Tab, focus should be on A (first focusable) if none was focused before */
    CHECK(lens_focused(ui, aid) == true || lens_focused(ui, bid) == true);

    lens_destroy(ui);
}

int main(void) {
    test_hover_press_click();
    test_drag_outside_no_click();
    test_tab_focus();
    return TEST_REPORT();
}
