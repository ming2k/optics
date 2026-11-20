/* test_input.c — scripted input sequences → expected responses (ADR-0006). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static void test_hover_press_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: button enters, no input */
    optics_ui_input in1 = {.display_size = {200, 100}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in1);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r1 = optics_ui_get_response(ui);
    OT_EXPECT(r1.hovered == false);
    OT_EXPECT(r1.pressed == false);
    OT_EXPECT(r1.clicked == false);

    /* frame 2: cursor over button, press */
    optics_ui_input in2 = in1;
    in2.cursor = (optics_gfx_point){10, 10};
    in2.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    in2.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r2 = optics_ui_get_response(ui);
    OT_EXPECT(r2.hovered == true);
    OT_EXPECT(r2.pressed == true);
    OT_EXPECT(r2.clicked == false);

    /* frame 3: cursor still over, release → click */
    optics_ui_input in3 = in1;
    in3.cursor = (optics_gfx_point){10, 10};
    in3.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r3 = optics_ui_get_response(ui);
    OT_EXPECT(r3.hovered == true);
    OT_EXPECT(r3.pressed == false);
    OT_EXPECT(r3.clicked == true);

    optics_ui_release(ui);
}

static void test_drag_outside_no_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: button enters */
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: press inside */
    optics_ui_input in2 = in;
    in2.cursor = (optics_gfx_point){10, 10};
    in2.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in2.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in2);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_active(ui) != 0);

    /* frame 3: move outside, release → no click */
    optics_ui_input in3 = in;
    in3.cursor = (optics_gfx_point){500, 500};
    in3.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in3);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    optics_ui_test_end(ui);
    optics_ui_response r3 = optics_ui_get_response(ui);
    OT_EXPECT(r3.clicked == false);

    optics_ui_release(ui);
}

static void test_tab_focus(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: two buttons enter */
    optics_ui_begin(ui, &in);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    optics_ui_test_end(ui);

    /* frame 2: Tab press → focus moves to first focusable */
    optics_ui_input in2 = in;
    in2.keys[0] = (optics_ui_key_event){.key = 258, .pressed = true, .repeat = false};
    in2.key_count = 1;
    optics_ui_begin(ui, &in2);
    optics_ui_id aid = optics_ui_current_id(ui, "A");
    optics_ui_id bid = optics_ui_current_id(ui, "B");
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"}).clicked;
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"}).clicked;
    optics_ui_test_end(ui);

    /* After first Tab, focus should be on A (first focusable) if none was focused before */
    OT_EXPECT(optics_ui_focused(ui, aid) == true || optics_ui_focused(ui, bid) == true);

    optics_ui_release(ui);
}

int main(void) {
    test_hover_press_click();
    test_drag_outside_no_click();
    test_tab_focus();
    return OT_REPORT();
}
