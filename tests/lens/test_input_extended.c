/* test_input_extended.c — input edge cases missing from test_input.c:
 *  - right / middle mouse buttons (currently no-ops, but must not crash)
 *  - multi-button press (left wins active capture)
 *  - drag outside then back inside before release (click on return)
 *  - wheel routing to innermost scroll container (one-frame latency)
 *  - Shift+Tab reverse focus traversal
 */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ------------------------------------------------------------------ */
/*  Right / middle button generate their own click events             */
/* ------------------------------------------------------------------ */
static void test_right_and_middle_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: button enters */
    lens_begin(ui, &IN0);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 2: right press inside */
    lens_input in = IN0;
    in.cursor = (flux_point){10, 10};
    in.mouse_pressed[LENS_MOUSE_RIGHT] = true;
    in.mouse_down[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 3: right release inside → right_clicked, not left clicked */
    lens_input in2 = IN0;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_released[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r = lens_get_response(ui);
    CHECK(r.right_clicked == true);
    CHECK(r.clicked == false);

    /* frame 4: middle press inside */
    lens_input in3 = IN0;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_pressed[LENS_MOUSE_MIDDLE] = true;
    in3.mouse_down[LENS_MOUSE_MIDDLE] = true;
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 5: middle release inside → middle_clicked */
    lens_input in4 = IN0;
    in4.cursor = (flux_point){10, 10};
    in4.mouse_released[LENS_MOUSE_MIDDLE] = true;
    lens_begin(ui, &in4);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response rm = lens_get_response(ui);
    CHECK(rm.middle_clicked == true);
    CHECK(rm.clicked == false);

    /* frame 6: right release outside → no right_clicked */
    lens_input in5 = IN0;
    in5.cursor = (flux_point){500, 500};
    in5.mouse_released[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in5);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response ro = lens_get_response(ui);
    CHECK(ro.right_clicked == false);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Left+Right pressed together: left wins active capture             */
/* ------------------------------------------------------------------ */
static void test_multi_button_left_wins(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: enter */
    lens_begin(ui, &IN0);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 2: both left and right pressed inside */
    lens_input in = IN0;
    in.cursor = (flux_point){10, 10};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    in.mouse_pressed[LENS_MOUSE_RIGHT] = true;
    in.mouse_down[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    lens_end(ui);
    CHECK(lens_active(ui) != 0); /* someone is active */

    /* frame 3: release right only — left stays pressed, right click fires */
    lens_input in2 = IN0;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_down[LENS_MOUSE_LEFT] = true; /* still held */
    in2.mouse_released[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);
    lens_response r = lens_get_response(ui);
    CHECK(r.pressed == true); /* left still held */
    CHECK(r.right_clicked == true);

    /* frame 4: release left → left click */
    lens_input in3 = IN0;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    bool clicked = lens_button(ui, "A");
    lens_end(ui);
    CHECK(clicked == true);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Drag outside, return inside, release → click                      */
/* ------------------------------------------------------------------ */
static void test_drag_back_inside_yields_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: enter */
    lens_begin(ui, &IN0);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 2: press inside */
    lens_input in2 = IN0;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);
    CHECK(lens_active(ui) != 0);

    /* frame 3: drag outside */
    lens_input in3 = IN0;
    in3.cursor = (flux_point){500, 500};
    in3.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 4: drag back inside and release → click */
    lens_input in4 = IN0;
    in4.cursor = (flux_point){10, 10};
    in4.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in4);
    bool clicked = lens_button(ui, "A");
    lens_end(ui);
    CHECK(clicked == true);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Wheel delta scrolls a hovered scroll container.                   */
/*  (Nested scroll hot-routing is exercised in test_widgets.)         */
/* ------------------------------------------------------------------ */
static void test_wheel_scrolls_content(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++)
        lens_label(ui, "line");
    lens_scroll_end(ui);
    lens_end(ui);

    lens_node *first0 = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y0 = lens_node_bounds(first0).y;

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.scroll_y = -3.0f;
    lens_begin(ui, &in);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++)
        lens_label(ui, "line");
    lens_scroll_end(ui);
    lens_end(ui);

    lens_node *first1 = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y1 = lens_node_bounds(first1).y;
    CHECK(y1 < y0); /* content scrolled down */

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Shift+Tab moves focus backward                                    */
/* ------------------------------------------------------------------ */
static void test_shift_tab_reverse_focus(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = IN0;

    /* frame 1: three buttons enter */
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    (void)lens_button(ui, "C");
    lens_end(ui);

    /* frame 2: Tab moves to A */
    lens_input in2 = in;
    in2.key_count = 1;
    in2.keys[0] = (lens_key_event){.key = LENS_KEY_TAB, .pressed = true};
    lens_begin(ui, &in2);
    lens_id aid = lens_current_id(ui, "A");
    lens_id bid = lens_current_id(ui, "B");
    lens_id cid = lens_current_id(ui, "C");
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    (void)lens_button(ui, "C");
    lens_end(ui);
    CHECK(lens_focused(ui, aid) == true);

    /* frame 3: Tab moves to B */
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    (void)lens_button(ui, "C");
    lens_end(ui);
    CHECK(lens_focused(ui, bid) == true);

    /* frame 4: Shift+Tab moves back to A */
    lens_input in3 = in;
    in3.key_count = 1;
    in3.keys[0] = (lens_key_event){.key = LENS_KEY_TAB, .pressed = true};
    in3.mods = (1u << 0); /* LENS_MOD_SHIFT */
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    (void)lens_button(ui, "C");
    lens_end(ui);
    CHECK(lens_focused(ui, aid) == true);

    /* frame 5: Shift+Tab from A wraps to C */
    lens_begin(ui, &in3);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    (void)lens_button(ui, "C");
    lens_end(ui);
    CHECK(lens_focused(ui, cid) == true);

    lens_destroy(ui);
}

int main(void) {
    test_right_and_middle_click();
    test_multi_button_left_wins();
    test_drag_back_inside_yields_click();
    test_wheel_scrolls_content();
    test_shift_tab_reverse_focus();
    return TEST_REPORT();
}
