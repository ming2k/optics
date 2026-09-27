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
    lens_row_begin(ui, NULL);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    (void)lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_close(ui);
    test_end(ui);

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

    lens_release(ui);
}

static void test_flex_distributes_slack(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 0, .pad = 0, .cross = LENS_STRETCH});
    lens_flex(ui, 1.0f);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_flex(ui, 1.0f);
    (void)lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_close(ui);
    test_end(ui);

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

    lens_release(ui);
}

static void test_flex_child_shrinks_between_fixed_siblings(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 5, .pad = 0, .cross = LENS_STRETCH});
    lens_size(ui, 40.0f, 0.0f);
    (void)lens_button(ui, &(lens_button_opts){.label = "sidebar"});
    lens_flex(ui, 1.0f);
    lens_column_begin(ui, NULL);
    (void)lens_button(ui,
                      &(lens_button_opts){.label = "workspace content with a wide intrinsic size"});
    lens_close(ui);
    lens_size(ui, 50.0f, 0.0f);
    (void)lens_button(ui, &(lens_button_opts){.label = "inspector"});
    lens_close(ui);
    test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *sidebar = lens_node_first_child(row);
    lens_node *workspace = lens_node_next_sibling(sidebar);
    lens_node *inspector = lens_node_next_sibling(workspace);

    flux_rect rs = lens_node_bounds(sidebar);
    flux_rect rw = lens_node_bounds(workspace);
    flux_rect ri = lens_node_bounds(inspector);

    CHECK_NEAR(rs.w, 40.0f, 0.5f);
    CHECK_NEAR(rw.x, 45.0f, 0.5f);
    CHECK_NEAR(rw.w, 100.0f, 0.5f);
    CHECK_NEAR(ri.x, 150.0f, 0.5f);
    CHECK_NEAR(ri.w, 50.0f, 0.5f);
    CHECK_NEAR(ri.x + ri.w, 200.0f, 0.5f);

    lens_release(ui);
}

static void test_column_stacks_children(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};

    float h = btn_h(ui, "A");
    float gap = lens_get_theme(ui).gap;

    lens_begin(ui, &in);
    (void)lens_button(ui,
                      &(lens_button_opts){.label = "A"}); /* direct children of the root column */
    (void)lens_button(ui, &(lens_button_opts){.label = "B"});
    test_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *a = lens_node_first_child(root);
    lens_node *b = lens_node_next_sibling(a);

    flux_rect ra = lens_node_bounds(a);
    flux_rect rb = lens_node_bounds(b);

    CHECK_NEAR(ra.y, 0.0f, 0.5f);
    CHECK_NEAR(rb.y, h + gap, 0.5f);
    CHECK_NEAR(ra.w, 200.0f, 0.5f); /* stretched across */

    lens_release(ui);
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
    lens_row_begin(ui, NULL);
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_close(ui);
    test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    flux_rect rr = lens_node_bounds(row);
    CHECK_NEAR(rr.h, 300.0f, 0.5f); /* grew to the full display height */

    lens_release(ui);
}

static void test_container_width_constraints_bound_intrinsic_size(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {500, 100}, .dt_seconds = 0.016f};
    const float pad = 8.0f;
    const float natural = btn_w(ui, "natural") + 2.0f * pad;

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.gap = 5.0f, .cross = LENS_STRETCH});
    lens_column_begin(ui, &(lens_layout_opts){.box = {.min_width = 90.0f}, .pad = pad});
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_close(ui);
    lens_column_begin(
        ui, &(lens_layout_opts){.box = {.min_width = 20.0f, .max_width = 240.0f}, .pad = pad});
    (void)lens_button(ui, &(lens_button_opts){.label = "natural"});
    lens_close(ui);
    lens_column_begin(ui, &(lens_layout_opts){.box = {.max_width = 100.0f}, .pad = pad});
    (void)lens_button(
        ui, &(lens_button_opts){.label = "content that is intentionally much wider than the cap"});
    lens_close(ui);
    lens_close(ui);
    test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *minimum = lens_node_first_child(row);
    lens_node *intrinsic = lens_node_next_sibling(minimum);
    lens_node *maximum = lens_node_next_sibling(intrinsic);
    CHECK_NEAR(lens_node_bounds(minimum).w, 90.0f, 0.5f);
    CHECK_NEAR(lens_node_bounds(intrinsic).w, natural, 0.5f);
    CHECK_NEAR(lens_node_bounds(maximum).w, 100.0f, 0.5f);

    lens_release(ui);
}

static void test_flex_redistributes_space_after_max_width(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.cross = LENS_STRETCH});
    lens_column_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f, .max_width = 100.0f}});
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_close(ui);
    lens_column_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f}});
    (void)lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_close(ui);
    lens_close(ui);
    test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *capped = lens_node_first_child(row);
    lens_node *remainder = lens_node_next_sibling(capped);
    CHECK_NEAR(lens_node_bounds(capped).w, 100.0f, 0.5f);
    CHECK_NEAR(lens_node_bounds(remainder).w, 200.0f, 0.5f);

    lens_release(ui);
}

static void test_flex_respects_min_width_while_shrinking(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.cross = LENS_STRETCH});
    lens_column_begin(
        ui, &(lens_layout_opts){.box = {.flex = 1.0f, .width = 100.0f, .min_width = 80.0f}});
    (void)lens_button(ui, &(lens_button_opts){.label = "A"});
    lens_close(ui);
    lens_column_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f, .width = 100.0f}});
    (void)lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_close(ui);
    lens_close(ui);
    test_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *floored = lens_node_first_child(row);
    lens_node *remainder = lens_node_next_sibling(floored);
    CHECK_NEAR(lens_node_bounds(floored).w, 80.0f, 0.5f);
    CHECK_NEAR(lens_node_bounds(remainder).w, 20.0f, 0.5f);

    lens_release(ui);
}

static void test_grid_uniform_tracks(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 3, .col_gap = 0, .row_gap = 0, .row_height = 40.0f});
    lens_response b0 = lens_button(ui, &(lens_button_opts){.label = "0"});
    lens_response b1 = lens_button(ui, &(lens_button_opts){.label = "1"});
    lens_response b2 = lens_button(ui, &(lens_button_opts){.label = "2"});
    lens_grid_end(ui);
    test_end(ui);

    flux_rect r0 = lens_node_bounds(lens_find(ui, b0.id));
    flux_rect r1 = lens_node_bounds(lens_find(ui, b1.id));
    flux_rect r2 = lens_node_bounds(lens_find(ui, b2.id));

    CHECK_NEAR(r0.x, 0.0f, 0.5f);
    CHECK_NEAR(r0.w, 100.0f, 0.5f);
    CHECK_NEAR(r1.x, 100.0f, 0.5f);
    CHECK_NEAR(r1.w, 100.0f, 0.5f);
    CHECK_NEAR(r2.x, 200.0f, 0.5f);
    CHECK_NEAR(r2.w, 100.0f, 0.5f);

    lens_release(ui);
}

static void test_grid_column_span(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {320, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(ui, &(lens_grid_opts){
                            .columns = 3, .col_gap = 10.0f, .row_gap = 10.0f, .row_height = 40.0f});
    lens_response ba = lens_button(ui, &(lens_button_opts){.box = {.col_span = 2}, .label = "A"});
    lens_response bb = lens_button(ui, &(lens_button_opts){.box = {.col_span = 1}, .label = "B"});
    lens_grid_end(ui);
    test_end(ui);

    flux_rect ra = lens_node_bounds(lens_find(ui, ba.id));
    flux_rect rb = lens_node_bounds(lens_find(ui, bb.id));

    CHECK_NEAR(ra.x, 0.0f, 0.5f);
    CHECK_NEAR(ra.w, 210.0f, 0.5f);
    CHECK_NEAR(rb.x, 220.0f, 0.5f);
    CHECK_NEAR(rb.w, 100.0f, 0.5f);

    lens_release(ui);
}

static void test_grid_row_span_and_bento_packing(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 300}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 50.0f});
    lens_response ba =
        lens_button(ui, &(lens_button_opts){.box = {.col_span = 2, .row_span = 1}, .label = "A"});
    lens_response bb =
        lens_button(ui, &(lens_button_opts){.box = {.col_span = 1, .row_span = 2}, .label = "B"});
    lens_response bc =
        lens_button(ui, &(lens_button_opts){.box = {.col_span = 1, .row_span = 1}, .label = "C"});
    lens_response bd =
        lens_button(ui, &(lens_button_opts){.box = {.col_span = 1, .row_span = 1}, .label = "D"});
    lens_grid_end(ui);
    test_end(ui);

    flux_rect ra = lens_node_bounds(lens_find(ui, ba.id));
    flux_rect rb = lens_node_bounds(lens_find(ui, bb.id));
    flux_rect rc = lens_node_bounds(lens_find(ui, bc.id));
    flux_rect rd = lens_node_bounds(lens_find(ui, bd.id));

    CHECK_NEAR(ra.x, 0.0f, 0.5f);
    CHECK_NEAR(ra.y, 0.0f, 0.5f);
    CHECK_NEAR(ra.w, 200.0f, 0.5f);
    CHECK_NEAR(ra.h, 50.0f, 0.5f);

    CHECK_NEAR(rb.x, 200.0f, 0.5f);
    CHECK_NEAR(rb.y, 0.0f, 0.5f);
    CHECK_NEAR(rb.w, 100.0f, 0.5f);
    CHECK_NEAR(rb.h, 100.0f, 0.5f);

    CHECK_NEAR(rc.x, 0.0f, 0.5f);
    CHECK_NEAR(rc.y, 50.0f, 0.5f);
    CHECK_NEAR(rc.w, 100.0f, 0.5f);
    CHECK_NEAR(rc.h, 50.0f, 0.5f);

    CHECK_NEAR(rd.x, 100.0f, 0.5f);
    CHECK_NEAR(rd.y, 50.0f, 0.5f);
    CHECK_NEAR(rd.w, 100.0f, 0.5f);
    CHECK_NEAR(rd.h, 50.0f, 0.5f);

    lens_release(ui);
}

static void test_grid_explicit_coordinates(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 40.0f});
    lens_response ba =
        lens_button(ui, &(lens_button_opts){.box = {.grid_col = 3, .grid_row = 1}, .label = "A"});
    lens_response bb = lens_button(ui, &(lens_button_opts){.label = "B"});
    lens_response bc = lens_button(ui, &(lens_button_opts){.label = "C"});
    lens_grid_end(ui);
    test_end(ui);

    flux_rect ra = lens_node_bounds(lens_find(ui, ba.id));
    flux_rect rb = lens_node_bounds(lens_find(ui, bb.id));
    flux_rect rc = lens_node_bounds(lens_find(ui, bc.id));

    CHECK_NEAR(ra.x, 200.0f, 0.5f);
    CHECK_NEAR(rb.x, 0.0f, 0.5f);
    CHECK_NEAR(rc.x, 100.0f, 0.5f);

    lens_release(ui);
}

static void test_nested_1d_flex_inside_2d_grid(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 2, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 80.0f});
    lens_column_begin(ui, &(lens_layout_opts){.gap = 10.0f, .cross = LENS_STRETCH});
    lens_response b0 =
        lens_button(ui, &(lens_button_opts){.box = {.height = 30.0f}, .label = "Top"});
    lens_response b1 =
        lens_button(ui, &(lens_button_opts){.box = {.height = 30.0f}, .label = "Btm"});
    lens_column_end(ui);

    lens_response b2 = lens_button(ui, &(lens_button_opts){.label = "Right"});
    lens_grid_end(ui);
    test_end(ui);

    flux_rect r0 = lens_node_bounds(lens_find(ui, b0.id));
    flux_rect r1 = lens_node_bounds(lens_find(ui, b1.id));
    flux_rect r2 = lens_node_bounds(lens_find(ui, b2.id));

    CHECK_NEAR(r0.x, 0.0f, 0.5f);
    CHECK_NEAR(r0.y, 0.0f, 0.5f);
    CHECK_NEAR(r0.w, 150.0f, 0.5f);
    CHECK_NEAR(r0.h, 30.0f, 0.5f);

    CHECK_NEAR(r1.x, 0.0f, 0.5f);
    CHECK_NEAR(r1.y, 40.0f, 0.5f);
    CHECK_NEAR(r1.w, 150.0f, 0.5f);
    CHECK_NEAR(r1.h, 30.0f, 0.5f);

    CHECK_NEAR(r2.x, 150.0f, 0.5f);
    CHECK_NEAR(r2.w, 150.0f, 0.5f);

    lens_release(ui);
}

static void test_nested_2d_grid_inside_1d_column(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 300}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_response h =
        lens_button(ui, &(lens_button_opts){.box = {.height = 40.0f}, .label = "Header"});
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 2, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 30.0f});
    lens_response g0 = lens_button(ui, &(lens_button_opts){.label = "G0"});
    lens_response g1 = lens_button(ui, &(lens_button_opts){.label = "G1"});
    lens_response g2 = lens_button(ui, &(lens_button_opts){.box = {.col_span = 2}, .label = "G2"});
    lens_grid_end(ui);
    lens_response f =
        lens_button(ui, &(lens_button_opts){.box = {.height = 40.0f}, .label = "Footer"});
    test_end(ui);

    flux_rect rh = lens_node_bounds(lens_find(ui, h.id));
    flux_rect rg0 = lens_node_bounds(lens_find(ui, g0.id));
    flux_rect rg1 = lens_node_bounds(lens_find(ui, g1.id));
    flux_rect rg2 = lens_node_bounds(lens_find(ui, g2.id));
    flux_rect rf = lens_node_bounds(lens_find(ui, f.id));

    CHECK_NEAR(rh.y, 0.0f, 0.5f);
    CHECK_NEAR(rh.h, 40.0f, 0.5f);

    float theme_gap = lens_get_theme(ui).gap;
    float grid_top = 40.0f + theme_gap;
    CHECK_NEAR(rg0.y, grid_top, 0.5f);
    CHECK_NEAR(rg0.x, 0.0f, 0.5f);
    CHECK_NEAR(rg0.w, 100.0f, 0.5f);

    CHECK_NEAR(rg1.y, grid_top, 0.5f);
    CHECK_NEAR(rg1.x, 100.0f, 0.5f);
    CHECK_NEAR(rg1.w, 100.0f, 0.5f);

    CHECK_NEAR(rg2.y, grid_top + 30.0f, 0.5f);
    CHECK_NEAR(rg2.x, 0.0f, 0.5f);
    CHECK_NEAR(rg2.w, 200.0f, 0.5f);

    CHECK_NEAR(rf.y, grid_top + 60.0f + theme_gap, 0.5f);

    lens_release(ui);
}

static void test_grid_positional_hints(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_grid_begin(
        ui, &(lens_grid_opts){.columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 40.0f});

    lens_col_span(ui, 2);
    lens_response b0 = lens_button(ui, &(lens_button_opts){.label = "HintSpan2"});

    lens_response b1 = lens_button(ui, &(lens_button_opts){.label = "Col3"});

    lens_grid_end(ui);
    test_end(ui);

    flux_rect r0 = lens_node_bounds(lens_find(ui, b0.id));
    flux_rect r1 = lens_node_bounds(lens_find(ui, b1.id));

    CHECK_NEAR(r0.x, 0.0f, 0.5f);
    CHECK_NEAR(r0.w, 200.0f, 0.5f);
    CHECK_NEAR(r1.x, 200.0f, 0.5f);
    CHECK_NEAR(r1.w, 100.0f, 0.5f);

    lens_release(ui);
}

static void test_flow_wrap_layout(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    /* Wrap container: fixed width 200, pad 0, gap 10, row_gap 12.
     * Buttons: fixed width 80, height 30.
     * Item 0: x=0, y=0.
     * Item 1: x=90, y=0. (80 + 10 = 90. Next item would be 90 + 80 + 10 = 180 + 80 > 200, breaks!)
     * Item 2: x=0, y=42 (30 + 12 = 42).
     */
    lens_begin(ui, &in);
    lens_wrap_begin(ui, &(lens_layout_opts){
                            .box = {.id = "wrap_box", .width = 200.0f},
                            .gap = 10.0f,
                            .row_gap = 12.0f,
                            .pad = 0.0f,
                        });
    lens_button(ui, &(lens_button_opts){.box = {.id = "b0", .width = 80.0f, .height = 30.0f},
                                        .label = "0"});
    lens_button(ui, &(lens_button_opts){.box = {.id = "b1", .width = 80.0f, .height = 30.0f},
                                        .label = "1"});
    lens_button(ui, &(lens_button_opts){.box = {.id = "b2", .width = 80.0f, .height = 30.0f},
                                        .label = "2"});
    lens_wrap_end(ui);
    test_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *wrap_node = lens_node_first_child(root);
    CHECK(wrap_node != NULL);
    lens_node *n0 = lens_node_first_child(wrap_node);
    lens_node *n1 = n0 ? lens_node_next_sibling(n0) : NULL;
    lens_node *n2 = n1 ? lens_node_next_sibling(n1) : NULL;

    CHECK(n0 != NULL);
    CHECK(n1 != NULL);
    CHECK(n2 != NULL);

    flux_rect r0 = lens_node_bounds(n0);
    flux_rect r1 = lens_node_bounds(n1);
    flux_rect r2 = lens_node_bounds(n2);

    CHECK_NEAR(r0.x, 0.0f, 0.5f);
    CHECK_NEAR(r0.y, 0.0f, 0.5f);
    CHECK_NEAR(r1.x, 90.0f, 0.5f);
    CHECK_NEAR(r1.y, 0.0f, 0.5f);
    CHECK_NEAR(r2.x, 0.0f, 0.5f);
    CHECK_NEAR(r2.y, 42.0f, 0.5f);

    lens_release(ui);
}

static void test_stack_layering_layout(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    /* Stack container: width 100, height 100.
     * Base card: 100 x 100.
     * Top-right badge: 24 x 24 with align = LENS_END, cross = LENS_START.
     * Expected badge x = 76, y = 0.
     */
    lens_begin(ui, &in);
    lens_stack_begin(ui, &(lens_layout_opts){
                             .box = {.id = "stack_box", .width = 100.0f, .height = 100.0f},
                             .pad = 0.0f,
                         });
    lens_button(ui, &(lens_button_opts){.box = {.id = "base", .width = 100.0f, .height = 100.0f},
                                        .label = "base"});
    lens_button(ui, &(lens_button_opts){.box = {.id = "badge",
                                                .width = 24.0f,
                                                .height = 24.0f,
                                                .align = LENS_END,
                                                .cross = LENS_START},
                                        .label = "1"});
    lens_stack_end(ui);
    test_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *stack_node = lens_node_first_child(root);
    CHECK(stack_node != NULL);
    lens_node *n_base = lens_node_first_child(stack_node);
    lens_node *n_badge = n_base ? lens_node_next_sibling(n_base) : NULL;

    CHECK(n_base != NULL);
    CHECK(n_badge != NULL);

    flux_rect r_base = lens_node_bounds(n_base);
    flux_rect r_badge = lens_node_bounds(n_badge);

    CHECK_NEAR(r_base.x, 0.0f, 0.5f);
    CHECK_NEAR(r_base.y, 0.0f, 0.5f);
    CHECK_NEAR(r_badge.x, 76.0f, 0.5f);
    CHECK_NEAR(r_badge.y, 0.0f, 0.5f);

    lens_release(ui);
}

int main(void) {
    test_row_packs_children();
    test_flex_distributes_slack();
    test_flex_child_shrinks_between_fixed_siblings();
    test_column_stacks_children();
    test_flex_applies_to_terse_container();
    test_container_width_constraints_bound_intrinsic_size();
    test_flex_redistributes_space_after_max_width();
    test_flex_respects_min_width_while_shrinking();
    test_grid_uniform_tracks();
    test_grid_column_span();
    test_grid_row_span_and_bento_packing();
    test_grid_explicit_coordinates();
    test_nested_1d_flex_inside_2d_grid();
    test_nested_2d_grid_inside_1d_column();
    test_grid_positional_hints();
    test_flow_wrap_layout();
    test_stack_layering_layout();
    return TEST_REPORT();
}
