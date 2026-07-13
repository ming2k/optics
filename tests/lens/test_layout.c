/* test_layout.c — two-pass flexbox geometry (ADR-0005). CPU-only.
 *
 * Geometry expectations are derived through lens_text_measure rather than
 * hard-coded numbers, so the test passes against any text backend
 * (monospace stub or FT/HB) — what we're checking is that layout sums
 * those measurements correctly, not the exact glyph metrics. */

#include "test_helpers.h"
#include <lens/lens.h>

static float btn_w(lens *ui, const char *s) {
    lens_theme theme = lens_get_theme(ui);
    return lens_text_measure(ui, theme.font, s, theme.font_size).width + 2.0f * theme.padding;
}
static float btn_h(lens *ui, const char *s) {
    (void)s;
    lens_theme theme = lens_get_theme(ui);
    return theme.font_size + 2.0f * theme.padding;
}

static void test_row_packs_children(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    float wa = btn_w(ui, "A"), h = btn_h(ui, "A");
    float gap = lens_get_theme(ui).gap;

    lens_begin(ui, &in);
    lens_row(ui);
    (void)lens_button(ui, "A");
    (void)lens_button(ui, "B");
    lens_close(ui);
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *row = lens_node_first_child(root);
    CHECK(row != NULL);
    lens_node *a = lens_node_first_child(row);
    lens_node *b = a ? lens_node_next_sibling(a) : NULL;
    CHECK(a != NULL);
    CHECK(b != NULL);

    flux_rect ra = lens_node_bounds(a);
    flux_rect rb = lens_node_bounds(b);
    flux_rect rr = lens_node_bounds(row);

    CHECK_NEAR(rr.w, 200.0f, 0.5f); /* row stretched to display */
    CHECK_NEAR(ra.x, 0.0f, 0.5f);
    CHECK_NEAR(ra.w, wa, 0.5f);
    CHECK_NEAR(ra.h, h, 0.5f);
    CHECK_NEAR(rb.x, wa + gap, 0.5f);

    lens_destroy(ui);
}

static void test_flex_distributes_slack(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row_ex(ui, (lens_layout_opts){.gap = 0, .pad = 0, .cross = LENS_STRETCH});
    lens_flex(ui, 1.0f);
    (void)lens_button(ui, "A");
    lens_flex(ui, 1.0f);
    (void)lens_button(ui, "B");
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *a = lens_node_first_child(row);
    lens_node *b = lens_node_next_sibling(a);

    flux_rect ra = lens_node_bounds(a);
    flux_rect rb = lens_node_bounds(b);

    /* equal flex with zero gap and zero pad -> 100 each, regardless of
     * the intrinsic button size. */
    CHECK_NEAR(ra.x, 0.0f, 0.5f);
    CHECK_NEAR(ra.w, 100.0f, 0.5f);
    CHECK_NEAR(rb.x, 100.0f, 0.5f);
    CHECK_NEAR(rb.w, 100.0f, 0.5f);

    lens_destroy(ui);
}

static void test_column_stacks_children(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};

    float h = btn_h(ui, "A");
    float gap = lens_get_theme(ui).gap;

    lens_begin(ui, &in);
    (void)lens_button(ui, "A"); /* direct children of the root column */
    (void)lens_button(ui, "B");
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *a = lens_node_first_child(root);
    lens_node *b = lens_node_next_sibling(a);

    flux_rect ra = lens_node_bounds(a);
    flux_rect rb = lens_node_bounds(b);

    CHECK_NEAR(ra.y, 0.0f, 0.5f);
    CHECK_NEAR(rb.y, h + gap, 0.5f);
    CHECK_NEAR(ra.w, 200.0f, 0.5f); /* stretched across */

    lens_destroy(ui);
}

/* lens_flex(...) must apply to the *next node whether it is a widget OR a
 * container* (header contract). A terse lens_row/lens_column used to drop the
 * pending flex, collapsing to content height — so a flexed strip could not
 * fill its parent (e.g. a bottom-pinned settings button never reached the
 * bottom edge). */
static void test_flex_applies_to_terse_container(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 300}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_flex(ui, 1.0f); /* should stretch the row to fill the column */
    lens_row(ui);
    (void)lens_button(ui, "A");
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    flux_rect rr = lens_node_bounds(row);
    CHECK_NEAR(rr.h, 300.0f, 0.5f); /* grew to the full display height */

    lens_destroy(ui);
}

int main(void) {
    test_row_packs_children();
    test_flex_distributes_slack();
    test_column_stacks_children();
    test_flex_applies_to_terse_container();
    return TEST_REPORT();
}
