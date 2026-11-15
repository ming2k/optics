/* test_radio.c — radio appearance checkbox mutual exclusion. */

#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_radio_focus_selects(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    bool opt_a = false;
    bool opt_b = false;

    /* frame 1: build two radios */
    lens_begin(ui, &IN0);
    lens_column_begin(ui, nullptr);
    lens_checkbox(ui, &(lens_checkbox_opts){
                          .label = "A", .value = &opt_a, .appearance = LENS_CHECKBOX_RADIO});
    lens_checkbox(ui, &(lens_checkbox_opts){
                          .label = "B", .value = &opt_b, .appearance = LENS_CHECKBOX_RADIO});
    lens_close(ui);
    lens_test_end(ui);

    /* frame 2: focus radio B and press Return */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    lens_column_begin(ui, nullptr);
    lens_id id_b = lens_current_id(ui, "B");
    lens_set_focus(ui, id_b);
    lens_checkbox(ui, &(lens_checkbox_opts){
                          .label = "A", .value = &opt_a, .appearance = LENS_CHECKBOX_RADIO});
    bool changed_b = lens_checkbox(ui, &(lens_checkbox_opts){.label = "B",
                                                             .value = &opt_b,
                                                             .appearance = LENS_CHECKBOX_RADIO})
                         .changed;
    lens_close(ui);
    lens_test_end(ui);

    OT_EXPECT(changed_b == true);
    OT_EXPECT(opt_b == true);

    lens_release(ui);
}

static void test_radio_no_change_when_disabled(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    bool opt_a = false;

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed = lens_checkbox(ui, &(lens_checkbox_opts){.label = "A",
                                                           .value = &opt_a,
                                                           .appearance = LENS_CHECKBOX_RADIO,
                                                           .box = {.disabled = true}})
                       .changed;
    lens_test_end(ui);

    OT_EXPECT(changed == false);
    OT_EXPECT(opt_a == false);

    lens_release(ui);
}

int main(void) {
    test_radio_focus_selects();
    test_radio_no_change_when_disabled();
    return OT_REPORT();
}
