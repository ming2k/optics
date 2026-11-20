/* test_layout.c — two-pass flexbox geometry (ADR-0005). CPU-only.
 *
 * Geometry expectations are derived through lens_text_measure rather than
 * hard-coded numbers, so the test passes against any text backend
 * (monospace stub or FT/HB) — what we're checking is that layout sums
 * those measurements correctly, not the exact glyph metrics. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static float btn_w(optics_ui *ui, const char *s) {
    optics_ui_theme theme = optics_ui_get_theme(ui);
    return optics_ui_text_measure(
               ui, &(optics_ui_text_measure_opts){.utf8 = s, .size_px = theme.font_size})
               .width +
           2.0f * theme.padding;
}
static float btn_h(optics_ui *ui, const char *s) {
    (void)s;
    optics_ui_theme theme = optics_ui_get_theme(ui);
    return theme.font_size + 2.0f * theme.padding;
}

static void test_row_packs_children(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    float wa = btn_w(ui, "A"), h = btn_h(ui, "A");
    float gap = optics_ui_get_theme(ui).gap;

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *row = optics_ui_node_first_child(root);
    OT_EXPECT(row != nullptr);
    optics_ui_node *a = optics_ui_node_first_child(row);
    optics_ui_node *b = a ? optics_ui_node_next_sibling(a) : nullptr;
    OT_EXPECT(a != nullptr);
    OT_EXPECT(b != nullptr);

    optics_gfx_rect ra = optics_ui_node_bounds(a);
    optics_gfx_rect rb = optics_ui_node_bounds(b);
    optics_gfx_rect rr = optics_ui_node_bounds(row);

    OT_EXPECT_NEAR(rr.w, 200.0f, 0.5f); /* row stretched to display */
    OT_EXPECT_NEAR(ra.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(ra.w, wa, 0.5f);
    OT_EXPECT_NEAR(ra.h, h, 0.5f);
    OT_EXPECT_NEAR(rb.x, wa + gap, 0.5f);

    optics_ui_release(ui);
}

static void test_flex_distributes_slack(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = 0, .pad = 0, .cross = OPTICS_UI_STRETCH});
    optics_ui_flex(ui, 1.0f);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_flex(ui, 1.0f);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *a = optics_ui_node_first_child(row);
    optics_ui_node *b = optics_ui_node_next_sibling(a);

    optics_gfx_rect ra = optics_ui_node_bounds(a);
    optics_gfx_rect rb = optics_ui_node_bounds(b);

    /* equal flex with zero gap and zero pad -> 100 each, regardless of
     * the intrinsic button size. */
    OT_EXPECT_NEAR(ra.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(ra.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rb.x, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rb.w, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_flex_child_shrinks_between_fixed_siblings(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = 5, .pad = 0, .cross = OPTICS_UI_STRETCH});
    optics_ui_size(ui, 40.0f, 0.0f);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "sidebar"});
    optics_ui_flex(ui, 1.0f);
    optics_ui_column_begin(ui, nullptr);
    (void)optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "workspace content with a wide intrinsic size"});
    optics_ui_close(ui);
    optics_ui_size(ui, 50.0f, 0.0f);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "inspector"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *sidebar = optics_ui_node_first_child(row);
    optics_ui_node *workspace = optics_ui_node_next_sibling(sidebar);
    optics_ui_node *inspector = optics_ui_node_next_sibling(workspace);

    optics_gfx_rect rs = optics_ui_node_bounds(sidebar);
    optics_gfx_rect rw = optics_ui_node_bounds(workspace);
    optics_gfx_rect ri = optics_ui_node_bounds(inspector);

    OT_EXPECT_NEAR(rs.w, 40.0f, 0.5f);
    OT_EXPECT_NEAR(rw.x, 45.0f, 0.5f);
    OT_EXPECT_NEAR(rw.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(ri.x, 150.0f, 0.5f);
    OT_EXPECT_NEAR(ri.w, 50.0f, 0.5f);
    OT_EXPECT_NEAR(ri.x + ri.w, 200.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_column_stacks_children(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};

    float h = btn_h(ui, "A");
    float gap = optics_ui_get_theme(ui).gap;

    optics_ui_begin(ui, &in);
    (void)optics_ui_button(
        ui, &(optics_ui_button_opts){.label = "A"}); /* direct children of the root column */
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *a = optics_ui_node_first_child(root);
    optics_ui_node *b = optics_ui_node_next_sibling(a);

    optics_gfx_rect ra = optics_ui_node_bounds(a);
    optics_gfx_rect rb = optics_ui_node_bounds(b);

    OT_EXPECT_NEAR(ra.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rb.y, h + gap, 0.5f);
    OT_EXPECT_NEAR(ra.w, 200.0f, 0.5f); /* stretched across */

    optics_ui_release(ui);
}

/* lens_flex(...) must apply to the *next node whether it is a widget OR a
 * container* (header contract). A terse lens_row/lens_column used to drop the
 * pending flex, collapsing to content height — so a flexed strip could not
 * fill its parent (e.g. a bottom-pinned settings button never reached the
 * bottom edge). */
static void test_flex_applies_to_terse_container(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 300}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_flex(ui, 1.0f); /* should stretch the row to fill the column */
    optics_ui_row_begin(ui, nullptr);
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(row != nullptr);
    optics_gfx_rect rr = optics_ui_node_bounds(row);
    OT_EXPECT_NEAR(rr.h, 300.0f, 0.5f); /* grew to the full display height */

    optics_ui_release(ui);
}

static void test_container_width_constraints_bound_intrinsic_size(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {500, 100}, .dt_seconds = 0.016f};
    const float pad = 8.0f;
    const float natural = btn_w(ui, "natural") + 2.0f * pad;

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 5.0f, .cross = OPTICS_UI_STRETCH});
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.min_width = 90.0f}, .pad = pad});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_close(ui);
    optics_ui_column_begin(
        ui, &(optics_ui_layout_opts){.box = {.min_width = 20.0f, .max_width = 240.0f}, .pad = pad});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "natural"});
    optics_ui_close(ui);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.max_width = 100.0f}, .pad = pad});
    (void)optics_ui_button(
        ui,
        &(optics_ui_button_opts){.label = "content that is intentionally much wider than the cap"});
    optics_ui_close(ui);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *minimum = optics_ui_node_first_child(row);
    optics_ui_node *intrinsic = optics_ui_node_next_sibling(minimum);
    optics_ui_node *maximum = optics_ui_node_next_sibling(intrinsic);
    OT_EXPECT_NEAR(optics_ui_node_bounds(minimum).w, 90.0f, 0.5f);
    OT_EXPECT_NEAR(optics_ui_node_bounds(intrinsic).w, natural, 0.5f);
    OT_EXPECT_NEAR(optics_ui_node_bounds(maximum).w, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_flex_redistributes_space_after_max_width(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 100}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.cross = OPTICS_UI_STRETCH});
    optics_ui_column_begin(ui,
                           &(optics_ui_layout_opts){.box = {.flex = 1.0f, .max_width = 100.0f}});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_close(ui);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.flex = 1.0f}});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_close(ui);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *capped = optics_ui_node_first_child(row);
    optics_ui_node *remainder = optics_ui_node_next_sibling(capped);
    OT_EXPECT_NEAR(optics_ui_node_bounds(capped).w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(optics_ui_node_bounds(remainder).w, 200.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_flex_respects_min_width_while_shrinking(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.cross = OPTICS_UI_STRETCH});
    optics_ui_column_begin(
        ui, &(optics_ui_layout_opts){.box = {.flex = 1.0f, .width = 100.0f, .min_width = 80.0f}});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "A"});
    optics_ui_close(ui);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.box = {.flex = 1.0f, .width = 100.0f}});
    (void)optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_close(ui);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *floored = optics_ui_node_first_child(row);
    optics_ui_node *remainder = optics_ui_node_next_sibling(floored);
    OT_EXPECT_NEAR(optics_ui_node_bounds(floored).w, 80.0f, 0.5f);
    OT_EXPECT_NEAR(optics_ui_node_bounds(remainder).w, 20.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_grid_uniform_tracks(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(
        ui, &(optics_ui_grid_opts){.columns = 3, .col_gap = 0, .row_gap = 0, .row_height = 40.0f});
    optics_ui_response b0 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "0"});
    optics_ui_response b1 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "1"});
    optics_ui_response b2 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "2"});
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect r0 = optics_ui_node_bounds(optics_ui_find(ui, b0.id));
    optics_gfx_rect r1 = optics_ui_node_bounds(optics_ui_find(ui, b1.id));
    optics_gfx_rect r2 = optics_ui_node_bounds(optics_ui_find(ui, b2.id));

    OT_EXPECT_NEAR(r0.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r0.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(r1.x, 100.0f, 0.5f);
    OT_EXPECT_NEAR(r1.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(r2.x, 200.0f, 0.5f);
    OT_EXPECT_NEAR(r2.w, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_grid_column_span(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {320, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(
        ui, &(optics_ui_grid_opts){
                .columns = 3, .col_gap = 10.0f, .row_gap = 10.0f, .row_height = 40.0f});
    optics_ui_response ba =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.col_span = 2}, .label = "A"});
    optics_ui_response bb =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.col_span = 1}, .label = "B"});
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect ra = optics_ui_node_bounds(optics_ui_find(ui, ba.id));
    optics_gfx_rect rb = optics_ui_node_bounds(optics_ui_find(ui, bb.id));

    OT_EXPECT_NEAR(ra.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(ra.w, 210.0f, 0.5f);
    OT_EXPECT_NEAR(rb.x, 220.0f, 0.5f);
    OT_EXPECT_NEAR(rb.w, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_grid_row_span_and_bento_packing(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 300}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(ui,
                         &(optics_ui_grid_opts){
                             .columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 50.0f});
    optics_ui_response ba = optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.col_span = 2, .row_span = 1}, .label = "A"});
    optics_ui_response bb = optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.col_span = 1, .row_span = 2}, .label = "B"});
    optics_ui_response bc = optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.col_span = 1, .row_span = 1}, .label = "C"});
    optics_ui_response bd = optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.col_span = 1, .row_span = 1}, .label = "D"});
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect ra = optics_ui_node_bounds(optics_ui_find(ui, ba.id));
    optics_gfx_rect rb = optics_ui_node_bounds(optics_ui_find(ui, bb.id));
    optics_gfx_rect rc = optics_ui_node_bounds(optics_ui_find(ui, bc.id));
    optics_gfx_rect rd = optics_ui_node_bounds(optics_ui_find(ui, bd.id));

    OT_EXPECT_NEAR(ra.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(ra.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(ra.w, 200.0f, 0.5f);
    OT_EXPECT_NEAR(ra.h, 50.0f, 0.5f);

    OT_EXPECT_NEAR(rb.x, 200.0f, 0.5f);
    OT_EXPECT_NEAR(rb.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rb.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rb.h, 100.0f, 0.5f);

    OT_EXPECT_NEAR(rc.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rc.y, 50.0f, 0.5f);
    OT_EXPECT_NEAR(rc.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rc.h, 50.0f, 0.5f);

    OT_EXPECT_NEAR(rd.x, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rd.y, 50.0f, 0.5f);
    OT_EXPECT_NEAR(rd.w, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rd.h, 50.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_grid_explicit_coordinates(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(ui,
                         &(optics_ui_grid_opts){
                             .columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 40.0f});
    optics_ui_response ba = optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.grid_col = 3, .grid_row = 1}, .label = "A"});
    optics_ui_response bb = optics_ui_button(ui, &(optics_ui_button_opts){.label = "B"});
    optics_ui_response bc = optics_ui_button(ui, &(optics_ui_button_opts){.label = "C"});
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect ra = optics_ui_node_bounds(optics_ui_find(ui, ba.id));
    optics_gfx_rect rb = optics_ui_node_bounds(optics_ui_find(ui, bb.id));
    optics_gfx_rect rc = optics_ui_node_bounds(optics_ui_find(ui, bc.id));

    OT_EXPECT_NEAR(ra.x, 200.0f, 0.5f);
    OT_EXPECT_NEAR(rb.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rc.x, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_nested_1d_flex_inside_2d_grid(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(ui,
                         &(optics_ui_grid_opts){
                             .columns = 2, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 80.0f});
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.gap = 10.0f, .cross = OPTICS_UI_STRETCH});
    optics_ui_response b0 =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.height = 30.0f}, .label = "Top"});
    optics_ui_response b1 =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.height = 30.0f}, .label = "Btm"});
    optics_ui_column_end(ui);

    optics_ui_response b2 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Right"});
    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect r0 = optics_ui_node_bounds(optics_ui_find(ui, b0.id));
    optics_gfx_rect r1 = optics_ui_node_bounds(optics_ui_find(ui, b1.id));
    optics_gfx_rect r2 = optics_ui_node_bounds(optics_ui_find(ui, b2.id));

    OT_EXPECT_NEAR(r0.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r0.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r0.w, 150.0f, 0.5f);
    OT_EXPECT_NEAR(r0.h, 30.0f, 0.5f);

    OT_EXPECT_NEAR(r1.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r1.y, 40.0f, 0.5f);
    OT_EXPECT_NEAR(r1.w, 150.0f, 0.5f);
    OT_EXPECT_NEAR(r1.h, 30.0f, 0.5f);

    OT_EXPECT_NEAR(r2.x, 150.0f, 0.5f);
    OT_EXPECT_NEAR(r2.w, 150.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_nested_2d_grid_inside_1d_column(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 300}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_response h =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.height = 40.0f}, .label = "Header"});
    optics_ui_grid_begin(ui,
                         &(optics_ui_grid_opts){
                             .columns = 2, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 30.0f});
    optics_ui_response g0 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "G0"});
    optics_ui_response g1 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "G1"});
    optics_ui_response g2 =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.col_span = 2}, .label = "G2"});
    optics_ui_grid_end(ui);
    optics_ui_response f =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.height = 40.0f}, .label = "Footer"});
    optics_ui_test_end(ui);

    optics_gfx_rect rh = optics_ui_node_bounds(optics_ui_find(ui, h.id));
    optics_gfx_rect rg0 = optics_ui_node_bounds(optics_ui_find(ui, g0.id));
    optics_gfx_rect rg1 = optics_ui_node_bounds(optics_ui_find(ui, g1.id));
    optics_gfx_rect rg2 = optics_ui_node_bounds(optics_ui_find(ui, g2.id));
    optics_gfx_rect rf = optics_ui_node_bounds(optics_ui_find(ui, f.id));

    OT_EXPECT_NEAR(rh.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rh.h, 40.0f, 0.5f);

    float theme_gap = optics_ui_get_theme(ui).gap;
    float grid_top = 40.0f + theme_gap;
    OT_EXPECT_NEAR(rg0.y, grid_top, 0.5f);
    OT_EXPECT_NEAR(rg0.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rg0.w, 100.0f, 0.5f);

    OT_EXPECT_NEAR(rg1.y, grid_top, 0.5f);
    OT_EXPECT_NEAR(rg1.x, 100.0f, 0.5f);
    OT_EXPECT_NEAR(rg1.w, 100.0f, 0.5f);

    OT_EXPECT_NEAR(rg2.y, grid_top + 30.0f, 0.5f);
    OT_EXPECT_NEAR(rg2.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(rg2.w, 200.0f, 0.5f);

    OT_EXPECT_NEAR(rf.y, grid_top + 60.0f + theme_gap, 0.5f);

    optics_ui_release(ui);
}

static void test_grid_positional_hints(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {300, 200}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_grid_begin(ui,
                         &(optics_ui_grid_opts){
                             .columns = 3, .col_gap = 0.0f, .row_gap = 0.0f, .row_height = 40.0f});

    optics_ui_col_span(ui, 2);
    optics_ui_response b0 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "HintSpan2"});

    optics_ui_response b1 = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Col3"});

    optics_ui_grid_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_rect r0 = optics_ui_node_bounds(optics_ui_find(ui, b0.id));
    optics_gfx_rect r1 = optics_ui_node_bounds(optics_ui_find(ui, b1.id));

    OT_EXPECT_NEAR(r0.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r0.w, 200.0f, 0.5f);
    OT_EXPECT_NEAR(r1.x, 200.0f, 0.5f);
    OT_EXPECT_NEAR(r1.w, 100.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_flow_wrap_layout(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    /* Wrap container: fixed width 200, pad 0, gap 10, row_gap 12.
     * Buttons: fixed width 80, height 30.
     * Item 0: x=0, y=0.
     * Item 1: x=90, y=0. (80 + 10 = 90. Next item would be 90 + 80 + 10 = 180 + 80 > 200, breaks!)
     * Item 2: x=0, y=42 (30 + 12 = 42).
     */
    optics_ui_begin(ui, &in);
    optics_ui_wrap_begin(ui, &(optics_ui_layout_opts){
                                 .box = {.id = "wrap_box", .width = 200.0f},
                                 .gap = 10.0f,
                                 .row_gap = 12.0f,
                                 .pad = 0.0f,
                             });
    optics_ui_button(ui, &(optics_ui_button_opts){
                             .box = {.id = "b0", .width = 80.0f, .height = 30.0f}, .label = "0"});
    optics_ui_button(ui, &(optics_ui_button_opts){
                             .box = {.id = "b1", .width = 80.0f, .height = 30.0f}, .label = "1"});
    optics_ui_button(ui, &(optics_ui_button_opts){
                             .box = {.id = "b2", .width = 80.0f, .height = 30.0f}, .label = "2"});
    optics_ui_wrap_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *wrap_node = optics_ui_node_first_child(root);
    OT_EXPECT(wrap_node != nullptr);
    optics_ui_node *n0 = optics_ui_node_first_child(wrap_node);
    optics_ui_node *n1 = n0 ? optics_ui_node_next_sibling(n0) : nullptr;
    optics_ui_node *n2 = n1 ? optics_ui_node_next_sibling(n1) : nullptr;

    OT_EXPECT(n0 != nullptr);
    OT_EXPECT(n1 != nullptr);
    OT_EXPECT(n2 != nullptr);

    optics_gfx_rect r0 = optics_ui_node_bounds(n0);
    optics_gfx_rect r1 = optics_ui_node_bounds(n1);
    optics_gfx_rect r2 = optics_ui_node_bounds(n2);

    OT_EXPECT_NEAR(r0.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r0.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r1.x, 90.0f, 0.5f);
    OT_EXPECT_NEAR(r1.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r2.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r2.y, 42.0f, 0.5f);

    optics_ui_release(ui);
}

static void test_stack_layering_layout(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    /* Stack container: width 100, height 100.
     * Base card: 100 x 100.
     * Top-right badge: 24 x 24 with align = LENS_END, cross = LENS_START.
     * Expected badge x = 76, y = 0.
     */
    optics_ui_begin(ui, &in);
    optics_ui_stack_begin(ui, &(optics_ui_layout_opts){
                                  .box = {.id = "stack_box", .width = 100.0f, .height = 100.0f},
                                  .pad = 0.0f,
                              });
    optics_ui_button(
        ui, &(optics_ui_button_opts){.box = {.id = "base", .width = 100.0f, .height = 100.0f},
                                     .label = "base"});
    optics_ui_button(ui, &(optics_ui_button_opts){.box = {.id = "badge",
                                                          .width = 24.0f,
                                                          .height = 24.0f,
                                                          .align = OPTICS_UI_END,
                                                          .cross = OPTICS_UI_START},
                                                  .label = "1"});
    optics_ui_stack_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *stack_node = optics_ui_node_first_child(root);
    OT_EXPECT(stack_node != nullptr);
    optics_ui_node *n_base = optics_ui_node_first_child(stack_node);
    optics_ui_node *n_badge = n_base ? optics_ui_node_next_sibling(n_base) : nullptr;

    OT_EXPECT(n_base != nullptr);
    OT_EXPECT(n_badge != nullptr);

    optics_gfx_rect r_base = optics_ui_node_bounds(n_base);
    optics_gfx_rect r_badge = optics_ui_node_bounds(n_badge);

    OT_EXPECT_NEAR(r_base.x, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r_base.y, 0.0f, 0.5f);
    OT_EXPECT_NEAR(r_badge.x, 76.0f, 0.5f);
    OT_EXPECT_NEAR(r_badge.y, 0.0f, 0.5f);

    optics_ui_release(ui);
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
    return OT_REPORT();
}
