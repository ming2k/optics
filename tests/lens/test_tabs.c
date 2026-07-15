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

static void test_tab_strip_contains_its_labels(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    /* Even an undersized host hint is raised to the themed content minimum. */
    lens_begin(ui, &IN0);
    lens_size(ui, 300.0f, 8.0f);
    lens_tabs_begin(ui, "contained-tabs", &active);
    lens_flex(ui, 1.0f);
    lens_tab(ui, "Composition");
    lens_flex(ui, 1.0f);
    lens_tab(ui, "Atmosphere");
    lens_tabs_end(ui);
    lens_end(ui);

    lens_node *strip = lens_node_first_child(lens_root(ui));
    lens_node *first = lens_node_first_child(strip);
    lens_node *second = lens_node_next_sibling(first);
    CHECK(strip != NULL);
    CHECK(first != NULL);
    CHECK(second != NULL);

    flux_rect rs = lens_node_bounds(strip);
    flux_rect ra = lens_node_bounds(first);
    flux_rect rb = lens_node_bounds(second);
    CHECK_NEAR(ra.y, rs.y, 0.5f);
    CHECK_NEAR(rb.y, rs.y, 0.5f);
    CHECK(ra.y + ra.h <= rs.y + rs.h + 0.5f);
    CHECK(rb.y + rb.h <= rs.y + rs.h + 0.5f);
    CHECK(rs.h > 8.0f);

    lens_destroy(ui);
}

int main(void) {
    test_tabs_switch();
    test_tab_strip_contains_its_labels();
    return TEST_REPORT();
}
