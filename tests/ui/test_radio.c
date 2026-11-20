/* test_radio.c — radio appearance checkbox mutual exclusion. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_radio_focus_selects(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool opt_a = false;
    bool opt_b = false;

    /* frame 1: build two radios */
    optics_ui_begin(ui, &IN0);
    optics_ui_column_begin(ui, nullptr);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "A",
                                                      .value = &opt_a,
                                                      .appearance = OPTICS_UI_CHECKBOX_RADIO});
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "B",
                                                      .value = &opt_b,
                                                      .appearance = OPTICS_UI_CHECKBOX_RADIO});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    /* frame 2: focus radio B and press Return */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_column_begin(ui, nullptr);
    optics_ui_id id_b = optics_ui_current_id(ui, "B");
    optics_ui_set_focus(ui, id_b);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "A",
                                                      .value = &opt_a,
                                                      .appearance = OPTICS_UI_CHECKBOX_RADIO});
    bool changed_b =
        optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "B",
                                                          .value = &opt_b,
                                                          .appearance = OPTICS_UI_CHECKBOX_RADIO})
            .changed;
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    OT_EXPECT(changed_b == true);
    OT_EXPECT(opt_b == true);

    optics_ui_release(ui);
}

static void test_radio_no_change_when_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool opt_a = false;

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50, 15};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "A",
                                                          .value = &opt_a,
                                                          .appearance = OPTICS_UI_CHECKBOX_RADIO,
                                                          .box = {.disabled = true}})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed == false);
    OT_EXPECT(opt_a == false);

    optics_ui_release(ui);
}

int main(void) {
    test_radio_focus_selects();
    test_radio_no_change_when_disabled();
    return OT_REPORT();
}
