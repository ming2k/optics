/* test_input_extended.c — input edge cases missing from test_input.c:
 *  - right / middle mouse buttons (currently no-ops, but must not crash)
 *  - multi-button press (left wins active capture)
 *  - drag outside then back inside before release (click on return)
 *  - wheel routing to innermost scroll container (one-frame latency)
 *  - Shift+Tab reverse focus traversal
 */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ------------------------------------------------------------------ */
/*  Right / middle button generate their own click events             */
/* ------------------------------------------------------------------ */
static void test_right_and_middle_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: button enters */
    optics_ui_begin(ui, &IN0);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: right press inside */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){10, 10};
    in.mouse_pressed[OPTICS_UI_MOUSE_RIGHT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 3: right release inside → right_clicked, not left clicked */
    optics_ui_input in2 = IN0;
    in2.cursor = (optics_gfx_point){10, 10};
    in2.mouse_released[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r = optics_ui_get_response(ui);
    OT_EXPECT(r.right_clicked == true);
    OT_EXPECT(r.clicked == false);

    /* frame 4: middle press inside */
    optics_ui_input in3 = IN0;
    in3.cursor = (optics_gfx_point){10, 10};
    in3.mouse_pressed[OPTICS_UI_MOUSE_MIDDLE] = true;
    in3.mouse_down[OPTICS_UI_MOUSE_MIDDLE] = true;
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 5: middle release inside → middle_clicked */
    optics_ui_input in4 = IN0;
    in4.cursor = (optics_gfx_point){10, 10};
    in4.mouse_released[OPTICS_UI_MOUSE_MIDDLE] = true;
    optics_ui_begin(ui, &in4);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response rm = optics_ui_get_response(ui);
    OT_EXPECT(rm.middle_clicked == true);
    OT_EXPECT(rm.clicked == false);

    /* frame 6: right release outside → no right_clicked */
    optics_ui_input in5 = IN0;
    in5.cursor = (optics_gfx_point){500, 500};
    in5.mouse_released[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in5);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response ro = optics_ui_get_response(ui);
    OT_EXPECT(ro.right_clicked == false);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Left+Right pressed together: left wins active capture             */
/* ------------------------------------------------------------------ */
static void test_multi_button_left_wins(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: enter */
    optics_ui_begin(ui, &IN0);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: both left and right pressed inside */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){10, 10};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_RIGHT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_active(ui) != 0); /* someone is active */

    /* frame 3: release right only — left stays pressed, right click fires */
    optics_ui_input in2 = IN0;
    in2.cursor = (optics_gfx_point){10, 10};
    in2.mouse_down[OPTICS_UI_MOUSE_LEFT] = true; /* still held */
    in2.mouse_released[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r = optics_ui_get_response(ui);
    OT_EXPECT(r.pressed == true); /* left still held */
    OT_EXPECT(r.right_clicked == true);

    /* frame 4: release left → left click */
    optics_ui_input in3 = IN0;
    in3.cursor = (optics_gfx_point){10, 10};
    in3.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in3);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked == true);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Drag outside, return inside, release → click                      */
/* ------------------------------------------------------------------ */
static void test_drag_back_inside_yields_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: enter */
    optics_ui_begin(ui, &IN0);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: press inside */
    optics_ui_input in2 = IN0;
    in2.cursor = (optics_gfx_point){10, 10};
    in2.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in2.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_active(ui) != 0);

    /* frame 3: drag outside */
    optics_ui_input in3 = IN0;
    in3.cursor = (optics_gfx_point){500, 500};
    in3.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 4: drag back inside and release → click */
    optics_ui_input in4 = IN0;
    in4.cursor = (optics_gfx_point){10, 10};
    in4.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in4);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked == true);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Wheel delta scrolls a hovered scroll container.                   */
/*  (Nested scroll hot-routing is exercised in test_widgets.)         */
/* ------------------------------------------------------------------ */
static void test_wheel_scrolls_content(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 10; i++) {
        /* Unique id per row: same-label siblings share one node otherwise. */
        optics_ui_push_id_int(ui, i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "line"});
        optics_ui_pop_id(ui);
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *first0 =
        optics_ui_node_first_child(optics_ui_node_first_child(optics_ui_root(ui)));
    float y0 = optics_ui_node_bounds(first0).y;

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.scroll_y = -3.0f;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 10; i++) {
        optics_ui_push_id_int(ui, i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "line"});
        optics_ui_pop_id(ui);
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *first1 =
        optics_ui_node_first_child(optics_ui_node_first_child(optics_ui_root(ui)));
    float y1 = optics_ui_node_bounds(first1).y;
    OT_EXPECT(y1 < y0); /* content scrolled down */

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Shift+Tab moves focus backward                                    */
/* ------------------------------------------------------------------ */
static void test_shift_tab_reverse_focus(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = IN0;

    /* frame 1: three buttons enter */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: Tab moves to A */
    optics_ui_input in2 = in;
    in2.key_count = 1;
    in2.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_TAB, .pressed = true};
    optics_ui_begin(ui, &in2);
    optics_ui_id aid = optics_ui_current_id(ui, "A");
    optics_ui_id bid = optics_ui_current_id(ui, "B");
    optics_ui_id cid = optics_ui_current_id(ui, "C");
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_focused(ui, aid) == true);

    /* frame 3: Tab moves to B */
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_focused(ui, bid) == true);

    /* frame 4: Shift+Tab moves back to A */
    optics_ui_input in3 = in;
    in3.key_count = 1;
    in3.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_TAB, .pressed = true};
    in3.mods = (1u << 0); /* LENS_MOD_SHIFT */
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_focused(ui, aid) == true);

    /* frame 5: Shift+Tab from A wraps to C */
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_focused(ui, cid) == true);

    optics_ui_release(ui);
}

/* ------------------------------------------------------------------ */
/*  Function keys F1 through F12 (ADR-0099)                           */
/* ------------------------------------------------------------------ */
static void test_function_keys_f1_to_f12(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    for (int k = OPTICS_UI_KEY_F1; k <= OPTICS_UI_KEY_F12; k++) {
        optics_ui_input in = IN0;
        in.key_count = 1;
        in.keys[0] = (optics_ui_key_event){.key = k, .pressed = true, .repeat = false};
        optics_ui_begin(ui, &in);
        (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "FKey"}).clicked;
        optics_ui_test_end(ui);
    }

    optics_ui_release(ui);
}

int main(void) {
    test_right_and_middle_click();
    test_multi_button_left_wins();
    test_drag_back_inside_yields_click();
    test_wheel_scrolls_content();
    test_shift_tab_reverse_focus();
    test_function_keys_f1_to_f12();
    return OT_REPORT();
}
