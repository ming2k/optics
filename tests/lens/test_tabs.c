/* test_tabs.c — lens_tabs_begin / lens_tab / lens_tabs_end switching. */

#include "../../libs/lens/src/internal.h"
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

static void test_standard_tab_strip_contains_its_labels(void) {
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

static void test_connected_tabs_are_an_opt_in_inset_variant(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 1;

    lens_begin(ui, &IN0);
    lens_size(ui, 300.0f, 8.0f);
    lens_tabs_begin_ex(ui, "connected-tabs", &active,
                       (lens_tabs_opts){.style = LENS_TAB_STYLE_CONNECTED});
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
    CHECK(ra.y > rs.y);
    CHECK(rb.y > rs.y);
    CHECK(ra.y + ra.h < rs.y + rs.h);
    CHECK(rb.y + rb.h < rs.y + rs.h);
    CHECK(rs.h > 8.0f);

    lens_destroy(ui);
}

static void build_indicator_tabs(lens *ui, int *active) {
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "indicator-tabs", active,
                       (lens_tabs_opts){.style = LENS_TAB_STYLE_INDICATOR,
                                        .equal_width = true});
    lens_tab(ui, "Composition");
    lens_tab(ui, "Atmosphere");
    lens_tabs_end(ui);
}

static void test_equal_width_is_an_independent_layout_policy(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    lens_begin(ui, &IN0);
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "equal-standard-tabs", &active,
                       (lens_tabs_opts){.style = LENS_TAB_STYLE_STANDARD,
                                        .equal_width = true});
    lens_tab(ui, "A");
    lens_tab(ui, "A much longer label");
    lens_tabs_end(ui);
    lens_end(ui);

    lens_node *strip = lens_node_first_child(lens_root(ui));
    lens_node *first = lens_node_first_child(strip);
    lens_node *second = lens_node_next_sibling(first);
    CHECK_NEAR(lens_node_bounds(first).w, lens_node_bounds(second).w, 0.5f);

    lens_destroy(ui);
}

static void test_indicator_tabs_are_compact_and_animate_selection(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    lens_begin(ui, &IN0);
    build_indicator_tabs(ui, &active);
    lens_end(ui);

    lens_node *strip = lens_node_first_child(lens_root(ui));
    lens_node *first = lens_node_first_child(strip);
    lens_node *second = lens_node_next_sibling(first);
    CHECK(strip != NULL);
    CHECK(first != NULL);
    CHECK(second != NULL);
    CHECK(lens_node_bounds(strip).h > lens_node_bounds(first).h);
    CHECK_NEAR(lens_node_bounds(first).w, lens_node_bounds(second).w, 0.5f);

    lens_input hover = IN0;
    flux_rect second_bounds = lens_node_bounds(second);
    hover.cursor = (flux_point){second_bounds.x + second_bounds.w * 0.5f,
                                second_bounds.y + second_bounds.h * 0.5f};
    lens_begin(ui, &hover);
    build_indicator_tabs(ui, &active);
    lens_end(ui);

    strip = lens_node_first_child(lens_root(ui));
    first = lens_node_first_child(strip);
    second = lens_node_next_sibling(first);
    bool drew_hover_surface = false;
    for (uint32_t i = 0; i < second->cmd_count; i++) {
        if (second->cmds[i].kind == LENS_DRAW_RECT && second->cmds[i].color != 0)
            drew_hover_surface = true;
    }
    CHECK(drew_hover_surface);

    active = 1;
    lens_begin(ui, &IN0);
    build_indicator_tabs(ui, &active);
    lens_end(ui);
    CHECK(lens_anim_pending(ui));

    for (int frame = 0; frame < 90; frame++) {
        lens_begin(ui, &IN0);
        build_indicator_tabs(ui, &active);
        lens_end(ui);
    }

    active = 0;
    lens_begin(ui, &IN0);
    build_indicator_tabs(ui, &active);
    lens_end(ui);

    strip = lens_node_first_child(lens_root(ui));
    first = lens_node_first_child(strip);
    second = lens_node_next_sibling(first);
    const lens_draw_cmd *indicator = NULL;
    for (uint32_t i = 0; i < strip->cmd_count; i++) {
        if (strip->cmds[i].kind == LENS_DRAW_TAB_INDICATOR)
            indicator = &strip->cmds[i];
    }
    CHECK(indicator != NULL);
    if (indicator) {
        float second_right = second->prev_rect.x + second->prev_rect.w - first->prev_rect.x;
        CHECK(indicator->rel.x >= 0.0f);
        CHECK(indicator->rel.x < second_right - 1.0f);
        CHECK(indicator->rel.x + indicator->rel.w <= strip->prev_rect.w + 0.5f);
    }

    bool stayed_inside_strip = true;
    for (int frame = 0; frame < 90; frame++) {
        lens_begin(ui, &IN0);
        build_indicator_tabs(ui, &active);
        lens_end(ui);

        strip = lens_node_first_child(lens_root(ui));
        indicator = NULL;
        for (uint32_t i = 0; i < strip->cmd_count; i++) {
            if (strip->cmds[i].kind == LENS_DRAW_TAB_INDICATOR)
                indicator = &strip->cmds[i];
        }
        if (!indicator || indicator->rel.x < 0.0f ||
            indicator->rel.x + indicator->rel.w > strip->prev_rect.w + 0.5f) {
            stayed_inside_strip = false;
        }
    }
    CHECK(stayed_inside_strip);

    lens_destroy(ui);
}

int main(void) {
    test_tabs_switch();
    test_standard_tab_strip_contains_its_labels();
    test_connected_tabs_are_an_opt_in_inset_variant();
    test_equal_width_is_an_independent_layout_policy();
    test_indicator_tabs_are_compact_and_animate_selection();
    return TEST_REPORT();
}
