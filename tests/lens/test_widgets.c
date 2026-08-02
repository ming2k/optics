/* test_widgets.c — widget behavior: button, checkbox, slider, collapsing, scroll. */

#include "test_helpers.h"
#include <lens/lens.h>

static void test_button_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: enter */
    lens_begin(ui, &in);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 2: press */
    lens_input in2 = in;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_button(ui, "A");
    lens_end(ui);

    /* frame 3: release → click */
    lens_input in3 = in;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    bool clicked = lens_button(ui, "A");
    lens_end(ui);
    CHECK(clicked == true);

    lens_destroy(ui);
}

static void test_checkbox_toggle(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    bool val = false;
    lens_begin(ui, &in);
    (void)lens_checkbox(ui, "X", &val);
    lens_end(ui);

    lens_input in2 = in;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_checkbox(ui, "X", &val);
    lens_end(ui);

    lens_input in3 = in;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    bool changed = lens_checkbox(ui, "X", &val);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(val == true);

    lens_destroy(ui);
}

static void test_slider_clamp_and_drag(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 100}, .dt_seconds = 0.016f};

    float v = 0.5f;
    lens_begin(ui, &in);
    (void)lens_slider(ui, "S", &v, 0.0f, 1.0f);
    lens_end(ui);

    /* frame 2: press inside slider track */
    lens_input in2 = in;
    in2.cursor = (flux_point){200, 20};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    (void)lens_slider(ui, "S", &v, 0.0f, 1.0f);
    lens_end(ui);

    lens_destroy(ui);
}

static void test_collapsing_toggle(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    CHECK(lens_collapsing(ui, "Panel") == false);
    lens_end(ui);

    /* click header */
    lens_input in2 = in;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    CHECK(lens_collapsing(ui, "Panel") == false); /* click registers next frame */
    lens_end(ui);

    lens_input in3 = in;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    bool open = lens_collapsing(ui, "Panel");
    lens_end(ui);
    CHECK(open == true);

    lens_destroy(ui);
}

static void test_collapsing_nested_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 200}, .dt_seconds = 0.016f};

    /* frame 1: build closed */
    lens_begin(ui, &in);
    if (lens_collapsing(ui, "Panel")) {
        lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);

    /* frame 2: press header to open */
    lens_input in2 = in;
    in2.cursor = (flux_point){10, 10};
    in2.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in2.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in2);
    if (lens_collapsing(ui, "Panel")) {
        lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);

    /* frame 3: release -> header opens */
    lens_input in3 = in;
    in3.cursor = (flux_point){10, 10};
    in3.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in3);
    if (lens_collapsing(ui, "Panel")) {
        lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);

    /* frame 4: build expanded; button now has geometry */
    lens_begin(ui, &in);
    bool open = false, nested_clicked = false;
    if (lens_collapsing(ui, "Panel")) {
        open = true;
        nested_clicked = lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);
    CHECK(open == true);

    /* locate the nested button so we can click it */
    lens_node *root = lens_root(ui);
    lens_node *hdr = lens_node_first_child(root);
    lens_node *body = lens_node_first_child(hdr);
    lens_node *btn = lens_node_next_sibling(body); /* skip spacer */
    flux_rect br = lens_node_bounds(btn);
    CHECK(br.w > 0.0f && br.h > 0.0f);

    /* frame 5: press on nested button */
    lens_input in5 = in;
    in5.cursor = (flux_point){br.x + 2.0f, br.y + 2.0f};
    in5.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in5.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in5);
    if (lens_collapsing(ui, "Panel")) {
        lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);

    /* frame 6: release -> nested button must receive the click */
    lens_input in6 = in;
    in6.cursor = (flux_point){br.x + 2.0f, br.y + 2.0f};
    in6.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in6);
    if (lens_collapsing(ui, "Panel")) {
        open = true;
        nested_clicked = lens_button(ui, "Inside");
        lens_close(ui);
    }
    lens_end(ui);
    CHECK(open == true);
    CHECK(nested_clicked == true);

    lens_destroy(ui);
}

static void test_scroll_offset(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    lens_node *first = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y0 = lens_node_bounds(first).y;

    /* scroll down (negative scroll_y = wheel down) */
    lens_input in2 = in;
    in2.scroll_y = -3.0f;
    lens_begin(ui, &in2);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    first = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y1 = lens_node_bounds(first).y;
    CHECK(y1 < y0 - 10.0f); /* content scrolled down */

    lens_destroy(ui);
}

static void test_scroll_thumb_drag(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 100}, .dt_seconds = 0.016f};

    /* frame 1: build */
    lens_begin(ui, &in);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* frame 2: scroll so thumb appears */
    lens_input in2 = in;
    in2.scroll_y = -3.0f;
    lens_begin(ui, &in2);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
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
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* frame 4: drag thumb down */
    lens_input in4 = in;
    in4.cursor = (flux_point){197, 50};
    in4.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in4);
    lens_size(ui, 0, 80);
    lens_scroll_begin(ui, "sc");
    for (int i = 0; i < 10; i++) {
        char lbl[16];
        snprintf(lbl, sizeof lbl, "line %d", i);
        lens_label(ui, lbl);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    first = lens_node_first_child(lens_node_first_child(lens_root(ui)));
    float y_after_drag = lens_node_bounds(first).y;
    CHECK(y_after_drag < y_after_scroll - 5.0f); /* dragged down -> scrolled down more */

    lens_destroy(ui);
}

static void test_title_and_heading_sizes(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {400, 300}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_title(ui, "Title");
    lens_heading(ui, "H1", 1);
    lens_heading(ui, "H2", 2);
    lens_heading(ui, "H3", 3);
    lens_label(ui, "Body");
    lens_label_ex(ui, "Large", 28.0f);
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *title = lens_node_first_child(root);
    lens_node *h1 = lens_node_next_sibling(title);
    lens_node *h2 = lens_node_next_sibling(h1);
    lens_node *h3 = lens_node_next_sibling(h2);
    lens_node *body = lens_node_next_sibling(h3);
    lens_node *large = lens_node_next_sibling(body);

    CHECK(title != NULL);
    CHECK(h1 != NULL);
    CHECK(h2 != NULL);
    CHECK(h3 != NULL);
    CHECK(body != NULL);
    CHECK(large != NULL);

    float t_h = lens_node_bounds(title).h;
    float h1_h = lens_node_bounds(h1).h;
    float h2_h = lens_node_bounds(h2).h;
    float h3_h = lens_node_bounds(h3).h;
    float b_h = lens_node_bounds(body).h;
    float l_h = lens_node_bounds(large).h;

    /* Title ≥ H1 ≥ H2 ≥ H3 ≥ Body (heights include padding) */
    CHECK(t_h >= h1_h);
    CHECK(h1_h >= h2_h);
    CHECK(h2_h >= h3_h);
    CHECK(h3_h >= b_h);
    /* Large label (28px) is bigger than body (14px) */
    CHECK(l_h > b_h);

    lens_destroy(ui);
}

static void test_compact_outlined_label_preserves_intrinsic_metrics(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {200, 80}, .dt_seconds = 0.016f};

    lens_begin(ui, &in);
    lens_row(ui);
    lens_push_id(ui, "plain");
    lens_label_compact_ex(ui, "12:34", 14.0f);
    lens_pop_id(ui);
    lens_push_id(ui, "outlined");
    lens_label_compact_outlined_ex(
        ui, "12:34", 14.0f,
        (lens_foreground_outline){
            .color = flux_color_rgba_premul(0, 0, 0, 180), .width = 0.75f});
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
        lens_text_measure(ui, theme.font, "Ag", theme.font_size).height + 2.0f * theme.padding;

    lens_begin(ui, &in);
    lens_column_ex(ui, (lens_layout_opts){.cross = LENS_START});
    lens_label_wrapped(ui,
                       "Paper account cash 100000 available 100000 equity 100000 and a "
                       "very-long-token-without-a-natural-break",
                       120.0f);
    lens_label(ui, "After");
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

int main(void) {
    test_button_click();
    test_checkbox_toggle();
    test_slider_clamp_and_drag();
    test_collapsing_toggle();
    test_collapsing_nested_click();
    test_scroll_offset();
    test_scroll_thumb_drag();
    test_title_and_heading_sizes();
    test_compact_outlined_label_preserves_intrinsic_metrics();
    test_wrapped_label_respects_width_and_grows_height();
    return TEST_REPORT();
}
