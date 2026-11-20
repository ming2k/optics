/* test_widgets.c — widget behaviours: scrollbar thumb dragging,
 * label sizing, outlines, and text wrapping. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static void test_scroll_thumb_drag(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: build */
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    /* frame 2: scroll so thumb appears */
    optics_ui_input in2 = in;
    in2.scroll_y = -3.0f;
    optics_ui_begin(ui, &in2);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    optics_ui_node *first =
        optics_ui_node_first_child(optics_ui_node_first_child(optics_ui_root(ui)));
    float y_after_scroll = optics_ui_node_bounds(first).y;
    OT_EXPECT(y_after_scroll < 0.0f);

    /* frame 3: press on scrollbar thumb (right edge) */
    optics_ui_input in3 = in;
    in3.cursor = (optics_gfx_point){197, 30};
    in3.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in3.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in3);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    /* frame 4: drag thumb down */
    optics_ui_input in4 = in;
    in4.cursor = (optics_gfx_point){197, 50};
    in4.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in4);
    optics_ui_size(ui, 0, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = lbl});
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    first = optics_ui_node_first_child(optics_ui_node_first_child(optics_ui_root(ui)));
    float y_after_drag = optics_ui_node_bounds(first).y;
    OT_EXPECT(y_after_drag < y_after_scroll - 5.0f);

    optics_ui_release(ui);
}

static void test_label_sizes(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Title", .size = 24.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Heading", .size = 18.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Body"});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Large", .size = 28.0f});
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *title = optics_ui_node_first_child(root);
    optics_ui_node *heading = optics_ui_node_next_sibling(title);
    optics_ui_node *body = optics_ui_node_next_sibling(heading);
    optics_ui_node *large = optics_ui_node_next_sibling(body);

    OT_EXPECT(title != nullptr);
    OT_EXPECT(heading != nullptr);
    OT_EXPECT(body != nullptr);
    OT_EXPECT(large != nullptr);

    float t_h = optics_ui_node_bounds(title).h;
    float h_h = optics_ui_node_bounds(heading).h;
    float b_h = optics_ui_node_bounds(body).h;
    float l_h = optics_ui_node_bounds(large).h;

    OT_EXPECT(t_h >= h_h);
    OT_EXPECT(h_h >= b_h);
    OT_EXPECT(l_h > b_h);

    optics_ui_release(ui);
}

static void test_compact_outlined_label_preserves_intrinsic_metrics(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 80}, .dt_seconds = 0.016f};

    optics_ui_style outline = optics_ui_style_init();
    outline.fields = OPTICS_UI_STYLE_OUTLINE_COLOR | OPTICS_UI_STYLE_OUTLINE_WIDTH;
    outline.outline_color = optics_gfx_color_rgba_premul(0, 0, 0, 180);
    outline.outline_width = 0.75f;

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_push_id(ui, "plain");
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "12:34", .size = 14.0f});
    optics_ui_pop_id(ui);
    optics_ui_push_id(ui, "outlined");
    optics_ui_push_style(ui, outline);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "12:34", .size = 14.0f});
    optics_ui_pop_style(ui);
    optics_ui_pop_id(ui);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *plain = optics_ui_node_first_child(row);
    optics_ui_node *outlined = optics_ui_node_next_sibling(plain);
    OT_EXPECT_NEAR(optics_ui_node_bounds(plain).w, optics_ui_node_bounds(outlined).w, 0.01f);
    OT_EXPECT_NEAR(optics_ui_node_bounds(plain).h, optics_ui_node_bounds(outlined).h, 0.01f);
    optics_ui_release(ui);
}

static void test_wrapped_label_respects_width_and_grows_height(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};
    optics_ui_theme theme = optics_ui_get_theme(ui);
    float single_line_h =
        optics_ui_text_measure(
            ui, &(optics_ui_text_measure_opts){.utf8 = "Ag", .size_px = theme.font_size})
            .height;

    optics_ui_begin(ui, &in);
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.cross = OPTICS_UI_START});
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = "Paper account cash 100000 available 100000 equity 100000 and a "
                                "very-long-token-without-a-natural-break",
                        .box = {.max_width = 120.0f},
                        .wrap = true,
                    });
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "After"});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *column = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *wrapped = optics_ui_node_first_child(column);
    optics_ui_node *after = optics_ui_node_next_sibling(wrapped);
    optics_gfx_rect rw = optics_ui_node_bounds(wrapped);
    optics_gfx_rect ra = optics_ui_node_bounds(after);

    OT_EXPECT_NEAR(rw.w, 120.0f, 0.5f);
    OT_EXPECT(rw.h > single_line_h * 2.0f);
    OT_EXPECT(ra.y >= rw.y + rw.h);
    OT_EXPECT(rw.x + rw.w <= 200.0f);

    optics_ui_release(ui);
}

static void test_label_zero_padding_by_default(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.cross = OPTICS_UI_START});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Tight Label", .size = 14.0f});
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *label = optics_ui_node_first_child(row);
    OT_EXPECT(label != nullptr);
    optics_gfx_rect r = optics_ui_node_bounds(label);
    optics_ui_text_metrics m = optics_ui_text_measure(
        ui, &(optics_ui_text_measure_opts){.utf8 = "Tight Label", .size_px = 14.0f});
    OT_EXPECT_NEAR(r.w, m.width, 0.5f);
    OT_EXPECT_NEAR(r.h, m.height, 0.5f);

    optics_ui_release(ui);
}

int main(void) {
    test_scroll_thumb_drag();
    test_label_sizes();
    test_compact_outlined_label_preserves_intrinsic_metrics();
    test_wrapped_label_respects_width_and_grows_height();
    test_label_zero_padding_by_default();
    return OT_REPORT();
}
