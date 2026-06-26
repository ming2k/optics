/* test_tabs.c — lens_tabs_begin / lens_tab / lens_tabs_end switching. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_tabs_switch(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    int active = 0;

    /* frame 1: build tabs */
    lens_begin(ui, &IN0);
    lens_tabs_begin(ui, "tabs", &active);
    lens_tab(ui, "A");
    lens_tab(ui, "B");
    lens_tabs_end(ui);
    lens_end(ui);

    /* frame 2: focus tab B and press Return */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    lens_tabs_begin(ui, "tabs", &active);
    lens_id id_b = lens_current_id(ui, "B");
    lens_set_focus(ui, id_b);
    lens_tab(ui, "A");
    bool changed_b = lens_tab(ui, "B");
    lens_tabs_end(ui);
    lens_end(ui);

    CHECK(changed_b == true);
    CHECK(active == 1);

    lens_destroy(ui);
}

int main(void) {
    test_tabs_switch();
    return TEST_REPORT();
}
