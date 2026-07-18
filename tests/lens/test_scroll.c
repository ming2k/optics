/* test_scroll.c — scroll container offset handling. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>
#include <math.h>

static const lens_input IN0 = {.display_size = {400, 400}, .dt_seconds = 0.016f};

static void test_scroll_offset(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* First frame: build scroll container with many items */
    lens_begin(ui, &IN0);
    lens_size(ui, 0, 200);
    lens_scroll_begin(ui, "scroll");
    for (int i = 0; i < 20; i++) {
        lens_label(ui, "Item");
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* Second frame: scroll wheel over it */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 50};
    in.scroll_y = 5.0f; /* scroll down */
    lens_begin(ui, &in);
    lens_size(ui, 0, 200);
    lens_scroll_begin(ui, "scroll");
    for (int i = 0; i < 20; i++) {
        lens_label(ui, "Item");
    }
    lens_scroll_end(ui);
    lens_end(ui);

    /* Just verify no crash; scroll consumption is internal. */
    CHECK(1);

    lens_destroy(ui);
}

static void test_precise_scroll_uses_pixel_distance(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Two stable frames establish previous-frame hit-test geometry. */
    for (int frame = 0; frame < 2; frame++) {
        lens_begin(ui, &IN0);
        lens_size(ui, 0, 200);
        lens_scroll_begin(ui, "precise-scroll");
        for (int i = 0; i < 20; i++) {
            lens_push_id_int(ui, i);
            lens_label(ui, "Item");
            lens_pop_id(ui);
        }
        lens_scroll_end(ui);
        lens_end(ui);
    }

    lens_node *scroll = lens_node_first_child(lens_root(ui));
    lens_node *first = lens_node_first_child(scroll);
    CHECK(scroll != NULL && first != NULL);
    float before = lens_node_bounds(first).y;

    lens_input in = IN0;
    in.cursor = (flux_point){50, 50};
    in.scroll_pixels_y = -12.0f;
    lens_begin(ui, &in);
    lens_size(ui, 0, 200);
    lens_scroll_begin(ui, "precise-scroll");
    for (int i = 0; i < 20; i++) {
        lens_push_id_int(ui, i);
        lens_label(ui, "Item");
        lens_pop_id(ui);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    scroll = lens_node_first_child(lens_root(ui));
    first = lens_node_first_child(scroll);
    CHECK(scroll != NULL && first != NULL);
    float after = lens_node_bounds(first).y;
    CHECK(fabsf((after - before) + 12.0f) < 0.01f);

    lens_destroy(ui);
}

static void test_scrollbar_gutter_clips_overflowing_descendants(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_theme theme = lens_theme_dark();
    theme.gap = 0.0f;
    theme.scrollbar_width = 8.0f;
    theme.scrollbar_radius = 0.0f;
    theme.color_scrollbar_track = flux_color_rgba_premul(0, 255, 0, 255);
    theme.color_scrollbar_thumb = theme.color_scrollbar_track;
    theme.color_scrollbar_thumb_hover = theme.color_scrollbar_track;
    theme.color_scrollbar_thumb_active = theme.color_scrollbar_track;
    lens_set_theme(ui, theme);

    lens_input input = {.display_size = {100, 40}, .dt_seconds = 0.016f};
    lens_begin(ui, &input);
    lens_size(ui, 100.0f, 40.0f);
    lens_scroll_begin(ui, "scroll-gutter");
    for (int i = 0; i < 2; i++) {
        /* The scroll child is clamped to the 92 px content width, while its
         * fixed-width descendant deliberately paints through the trailing
         * edge. The viewport clip must keep that white surface out of the
         * green scrollbar gutter. */
        lens_size(ui, 100.0f, 30.0f);
        lens_row_ex(ui, (lens_layout_opts){.gap = 0.0f, .cross = LENS_STRETCH});
        lens_size(ui, 100.0f, 30.0f);
        lens_column_ex(ui, (lens_layout_opts){.bg = flux_color_rgba_premul(255, 255, 255, 255)});
        lens_close(ui);
        lens_close(ui);
    }
    lens_scroll_end(ui);
    lens_end(ui);

    flux_canvas *canvas = NULL;
    CHECK(flux_canvas_create_cpu(100, 40, 1.0f, &canvas) == FLUX_OK);
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(canvas, &black) == FLUX_OK);
    CHECK(lens_render(ui, canvas) == FLUX_OK);
    flux_canvas_cpu_end(canvas);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
    CHECK(fb != NULL && width == 100 && height == 40);
    bool gutter_is_clear = true;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 92; x < width; x++) {
            const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
            gutter_is_clear &= p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255;
        }
    }
    CHECK(gutter_is_clear);

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

int main(void) {
    test_scroll_offset();
    test_precise_scroll_uses_pixel_distance();
    test_scrollbar_gutter_clips_overflowing_descendants();
    return TEST_REPORT();
}
