/* test_tabs.c — lens_tabs_begin / lens_tab / lens_tabs_end switching, and
 * the ADR-0061 skin migration: the default skin draws a STATIC indicator
 * (theme accent, fixed thickness, zero animation); the spring physics left
 * the core (the recipe lives in examples/showcase/tabs_spring_skin.c). */

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

static void test_tab_strip_contains_its_labels(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    /* Even an undersized host hint is raised to the content minimum (tabs
     * plus the indicator band below them). */
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

static void test_equal_width_is_an_independent_layout_policy(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    lens_begin(ui, &IN0);
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "equal-standard-tabs", &active,
                       (lens_tabs_opts){.equal_width = true});
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

static const lens_draw_cmd *find_indicator(const lens_node *strip) {
    for (uint32_t i = 0; i < strip->cmd_count; i++) {
        if (strip->cmds[i].kind == LENS_DRAW_TAB_INDICATOR)
            return &strip->cmds[i];
    }
    return NULL;
}

/* The default skin's indicator is static: it lands on the active tab the
 * frame after geometry settles — no animation frames, no anim_pending,
 * theme accent colour, fixed 3px thickness. */
static void test_static_indicator_tracks_selection_without_animation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;
    flux_color accent = lens_get_theme(ui).color_accent;

    /* Keep the cursor off the strip: a hover ease in transit would set
     * anim_pending on its own and drown the tabs signal. */
    const lens_input AWAY = {.display_size = {400, 200},
                             .dt_seconds = 0.016f,
                             .cursor = {390.0f, 195.0f}};

    lens_begin(ui, &AWAY);
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "tabs", &active, (lens_tabs_opts){.equal_width = true});
    lens_tab(ui, "Composition");
    lens_tab(ui, "Atmosphere");
    lens_tabs_end(ui);
    lens_end(ui);

    /* frame 2: geometry settled; the indicator sits under tab 0. */
    lens_begin(ui, &AWAY);
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "tabs", &active, (lens_tabs_opts){.equal_width = true});
    lens_tab(ui, "Composition");
    lens_tab(ui, "Atmosphere");
    lens_tabs_end(ui);
    lens_end(ui);

    lens_node *strip = lens_node_first_child(lens_root(ui));
    lens_node *first = lens_node_first_child(strip);
    lens_node *second = lens_node_next_sibling(first);
    CHECK(strip != NULL && first != NULL && second != NULL);

    const lens_draw_cmd *indicator = find_indicator(strip);
    CHECK(indicator != NULL);
    if (indicator) {
        CHECK(indicator->color == accent);
        CHECK_NEAR(indicator->width, 3.0f, 0.0f); /* fixed thickness */
        /* centred under the first tab (strip-local coordinates) */
        float first_w = first->prev_rect.w;
        CHECK(indicator->rel.x >= 0.0f);
        CHECK(indicator->rel.x + indicator->rel.w <= first_w + 0.5f);
    }
    CHECK(!lens_anim_pending(ui)); /* zero animation by construction */

    /* Switch: the very next frame has the indicator under tab 1 — it does
     * not travel over multiple frames. */
    active = 1;
    lens_begin(ui, &AWAY);
    lens_size(ui, 300.0f, 0.0f);
    lens_tabs_begin_ex(ui, "tabs", &active, (lens_tabs_opts){.equal_width = true});
    lens_tab(ui, "Composition");
    lens_tab(ui, "Atmosphere");
    lens_tabs_end(ui);
    lens_end(ui);

    strip = lens_node_first_child(lens_root(ui));
    first = lens_node_first_child(strip);
    second = lens_node_next_sibling(first);
    indicator = find_indicator(strip);
    CHECK(indicator != NULL);
    if (indicator) {
        float second_left = second->prev_rect.x - first->prev_rect.x;
        float second_right = second_left + second->prev_rect.w;
        CHECK(indicator->color == accent);
        CHECK(indicator->rel.x >= second_left - 0.5f);
        CHECK(indicator->rel.x + indicator->rel.w <= second_right + 0.5f);
    }
    CHECK(!lens_anim_pending(ui)); /* a selection change animates nothing */

    lens_destroy(ui);
}

/* A hover fill still lands on the hovered tab (per-tab chrome through the
 * strip's skin record). */
static void test_hover_fill_via_skin(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    int active = 0;

    lens_begin(ui, &IN0);
    lens_tabs_begin(ui, "tabs", &active);
    lens_tab(ui, "A");
    lens_tab(ui, "B");
    lens_tabs_end(ui);
    lens_end(ui);

    lens_node *strip = lens_node_first_child(lens_root(ui));
    lens_node *second = lens_node_next_sibling(lens_node_first_child(strip));
    flux_rect sb = lens_node_bounds(second);

    lens_input hover = IN0;
    hover.cursor = (flux_point){sb.x + sb.w * 0.5f, sb.y + sb.h * 0.5f};
    lens_begin(ui, &hover);
    lens_tabs_begin(ui, "tabs", &active);
    lens_tab(ui, "A");
    lens_tab(ui, "B");
    lens_tabs_end(ui);
    lens_end(ui);

    strip = lens_node_first_child(lens_root(ui));
    second = lens_node_next_sibling(lens_node_first_child(strip));
    bool drew_hover_surface = false;
    for (uint32_t i = 0; i < second->cmd_count; i++) {
        if (second->cmds[i].kind == LENS_DRAW_RECT && second->cmds[i].color != 0)
            drew_hover_surface = true;
    }
    CHECK(drew_hover_surface);

    lens_destroy(ui);
}

int main(void) {
    test_tabs_switch();
    test_tab_strip_contains_its_labels();
    test_equal_width_is_an_independent_layout_policy();
    test_static_indicator_tracks_selection_without_animation();
    test_hover_fill_via_skin();
    return TEST_REPORT();
}
