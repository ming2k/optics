/* test_radio.c — lens_radio mutual exclusion. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_radio_focus_selects(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int choice = 0;

    /* frame 1: build two radios */
    lens_begin(ui, &IN0);
    lens_column(ui);
    lens_radio(ui, "A", &choice, 1);
    lens_radio(ui, "B", &choice, 2);
    lens_close(ui);
    lens_end(ui);

    /* frame 2: focus radio B and press Return */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    lens_column(ui);
    lens_id id_b = lens_current_id(ui, "B");
    lens_set_focus(ui, id_b);
    lens_radio(ui, "A", &choice, 1);
    bool changed_b = lens_radio(ui, "B", &choice, 2);
    lens_close(ui);
    lens_end(ui);

    CHECK(changed_b == true);
    CHECK(choice == 2);

    lens_destroy(ui);
}

static void test_radio_no_change_when_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int choice = 1;

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed = lens_radio_ex(ui, (lens_radio_opts){.label = "A",
                                                       .value = &choice,
                                                       .option_value = 2,
                                                       .box = {.disabled = true}})
                       .changed;
    lens_end(ui);

    CHECK(changed == false);
    CHECK(choice == 1);

    lens_destroy(ui);
}

int main(void) {
    test_radio_focus_selects();
    test_radio_no_change_when_disabled();
    return TEST_REPORT();
}
