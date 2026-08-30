/* test_widgets.c — widget behaviours: scrollbar thumb dragging,
 * label sizing, outlines, and text wrapping. */

#include "test_helpers.h"
#include <lens/lens.h>

static void test_scroll_thumb_drag(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: build */
    lens_begin(ui, &in);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* frame 2: scroll so thumb appears */
    lens_input in2 = in;
    in2.scroll_y = -3.0f;
    lens_begin(ui, &in2);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_scroll_end(ui);
    lens_end(ui);

    lens_node *first = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y_after_scroll = lens_node_bounds(first).y;
    CHECK(y_after_scroll < 0.0f);

    /* frame 3: press on scrollbar thumb (right edge) */
    lens_input in3 = in;
    in3.cursor = (flux_point){197, 30};
    in3.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in3.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* frame 4: drag thumb down */
    lens_input in4 = in;
    in4.cursor = (flux_point){197, 50};
    in4.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in4);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, &(lens_scroll_opts){.box = {.id = "sc"}});
    for (int i = 0; i < 30; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, &(lens_label_opts){.text = lbl});
    }
    lens_scroll_end(ui);
    lens_end(ui);

    first = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y_after_drag = lens_node_bounds(first).y;
    CHECK(y_after_drag < y_after_scroll - 5.0f);

    lens_destroy(ui);
}

static void test_label_sizes(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_label(ui, &(lens_label_opts){.text = "Title", .size = 24.0f});
    lens_label(ui, &(lens_label_opts){.text = "Heading", .size = 18.0f});
    lens_label(ui, &(lens_label_opts){.text = "Body"});
    lens_label(ui, &(lens_label_opts){.text = "Large", .size = 28.0f});
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *title = lens_node_first_child(root);
    lens_node *heading = lens_node_next_sibling(title);
    lens_node *body = lens_node_next_sibling(heading);
    lens_node *large = lens_node_next_sibling(body);

    CHECK(title != NULL);
    CHECK(heading != NULL);
    CHECK(body != NULL);
    CHECK(large != NULL);

    float t_h = lens_node_bounds(title).h;
    float h_h = lens_node_bounds(heading).h;
    float b_h = lens_node_bounds(body).h;
    float l_h = lens_node_bounds(large).h;

    CHECK(t_h >= h_h);
    CHECK(h_h >= b_h);
    CHECK(l_h > b_h);

    lens_destroy(ui);
}

static void test_compact_outlined_label_preserves_intrinsic_metrics(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 80}, .dt_seconds = 0.016f};

    lens_style outline = lens_style_init();
    outline.fields = LENS_STYLE_OUTLINE_COLOR | LENS_STYLE_OUTLINE_WIDTH;
    outline.outline_color = flux_color_rgba_premul(0, 0, 0, 180);
    outline.outline_width = 0.75f;

    lens_begin(ui, &in);
    lens_row_begin(ui, NULL);
    lens_push_id(ui, "plain");
    lens_label(ui, &(lens_label_opts){.text = "12:34", .size = 14.0f});
    lens_pop_id(ui);
    lens_push_id(ui, "outlined");
    lens_push_style(ui, outline);
    lens_label(ui, &(lens_label_opts){.text = "12:34", .size = 14.0f});
    lens_pop_style(ui);
    lens_pop_id(ui);
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *plain = lens_node_first_child(row);
    lens_node *outlined = lens_node_next_sibling(plain);
    CHECK_NEAR(lens_node_bounds(plain).w, lens_node_bounds(outlined).w, 0.01f);
    CHECK_NEAR(lens_node_bounds(plain).h, lens_node_bounds(outlined).h, 0.01f);
    lens_destroy(ui);
}

static void test_wrapped_label_respects_width_and_grows_height(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};
    lens_theme theme = lens_get_theme(ui);
    float single_line_h =
        lens_text_measure(ui, theme.font, "Ag", theme.font_size).height;

    lens_begin(ui, &in);
    lens_column_begin(ui, &(lens_layout_opts){.cross = LENS_START});
    lens_label(ui, &(lens_label_opts){
                       .text = "Paper account cash 100000 available 100000 equity 100000 and a "
                               "very-long-token-without-a-natural-break",
                       .box = {.max_width = 120.0f},
                       .wrap = true,
                   });
    lens_label(ui, &(lens_label_opts){.text = "After"});
    lens_close(ui);
    lens_end(ui);

    lens_node *column = lens_node_first_child(lens_root(ui));
    lens_node *wrapped = lens_node_first_child(column);
    lens_node *after = lens_node_next_sibling(wrapped);
    flux_rect rw = lens_node_bounds(wrapped);
    flux_rect ra = lens_node_bounds(after);

    CHECK_NEAR(rw.w, 120.0f, 0.5f);
    CHECK(rw.h > single_line_h * 2.0f);
    CHECK(ra.y >= rw.y + rw.h);
    CHECK(rw.x + rw.w <= 200.0f);

    lens_destroy(ui);
}


static void test_label_zero_padding_by_default(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};
    lens_theme theme = lens_get_theme(ui);

    lens_begin(ui, &in);
    lens_row_begin(ui, &(lens_layout_opts){.cross = LENS_START});
    lens_label(ui, &(lens_label_opts){.text = "Tight Label", .size = 14.0f});
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    lens_node *label = lens_node_first_child(row);
    CHECK(label != NULL);
    flux_rect r = lens_node_bounds(label);
    lens_text_metrics m = lens_text_measure(ui, theme.font, "Tight Label", 14.0f);
    CHECK_NEAR(r.w, m.width, 0.5f);
    CHECK_NEAR(r.h, m.height, 0.5f);

    lens_destroy(ui);
}

int main(void) {
    test_scroll_thumb_drag();
    test_label_sizes();
    test_compact_outlined_label_preserves_intrinsic_metrics();
    test_wrapped_label_respects_width_and_grows_height();
    test_label_zero_padding_by_default();
    return TEST_REPORT();
}
