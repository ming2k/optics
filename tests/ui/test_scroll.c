/* test_scroll.c — scroll container offset handling. */

#include "lens_test.h"
#include "optics_test.h"
#include <math.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 400}, .dt_seconds = 0.016f};

static void test_scroll_offset(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Frame 1: empty scroll with cursor inside */
    optics_ui_input in1 = IN0;
    in1.cursor = (optics_gfx_point){50, 50};
    optics_ui_begin(ui, &in1);
    optics_ui_size(ui, 200, 200);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "log"}});
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    /* Frame 2: scroll down */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50, 50};
    in.scroll_y = -2.0f; /* 2 units down */
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200, 200);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "log"}});
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    float sx = 0, sy = 0;
    OT_EXPECT(optics_ui_scroll_offset(
        ui, &(optics_ui_scroll_offset_opts){.id = "log", .out_x = &sx, .out_y = &sy}));
    OT_EXPECT(sy > 0.0f); /* scrolled down -> positive offset */

    optics_ui_release(ui);
}

static void test_precise_scroll_uses_pixel_distance(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Warm-up frame: place the pointer inside the scroll container */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50, 50};
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200, 200);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "log"}});
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    /* Frame 2: 12 px touchpad scroll */
    in.scroll_y = 0.0f;
    in.scroll_pixels_y = -12.0f;
    optics_ui_begin(ui, &in);
    optics_ui_size(ui, 200, 200);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "log"}});
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    float sx = 0, sy = 0;
    OT_EXPECT(optics_ui_scroll_offset(
        ui, &(optics_ui_scroll_offset_opts){.id = "log", .out_x = &sx, .out_y = &sy}));
    OT_EXPECT_NEAR(sy, 12.0f, 0.001f);

    optics_ui_release(ui);
}

static void test_programmatic_scroll_is_applied_and_clamped(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input input = {.display_size = {200, 160}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &input);
    optics_ui_size(ui, 200, 80);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "jump-scroll"}});
    for (int i = 0; i < 10; i++) {
        optics_ui_push_id_int(ui, i);
        optics_ui_size(ui, 0, 20);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Item"});
        optics_ui_pop_id(ui);
    }
    optics_ui_scroll_end(ui);
    optics_ui_scroll_to(ui, &(optics_ui_scroll_to_opts){.id = "jump-scroll", .x = 0, .y = 60});
    optics_ui_test_end(ui);

    optics_ui_node *scroll = optics_ui_node_first_child(optics_ui_root(ui));
    optics_ui_node *first = optics_ui_node_first_child(scroll);
    OT_EXPECT(scroll != nullptr && first != nullptr);
    OT_EXPECT(fabsf(optics_ui_node_bounds(first).y + 60.0f) < 0.01f);

    optics_ui_release(ui);
}

static void test_scrollbar_gutter_clips_overflowing_descendants(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_theme theme = optics_ui_theme_dark();
    theme.gap = 0.0f;
    theme.scrollbar_width = 8.0f;
    theme.scrollbar_radius = 0.0f;
    theme.color_scrollbar_track = optics_gfx_color_rgba_premul(0, 255, 0, 255);
    theme.color_scrollbar_thumb = theme.color_scrollbar_track;
    theme.color_scrollbar_thumb_hover = theme.color_scrollbar_track;
    theme.color_scrollbar_thumb_active = theme.color_scrollbar_track;
    optics_ui_set_theme(ui, theme);

    optics_ui_input input = {.display_size = {100, 40}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &input);
    optics_ui_size(ui, 100.0f, 40.0f);
    optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "scroll-gutter"}});
    for (int i = 0; i < 2; i++) {
        optics_ui_size(ui, 100.0f, 30.0f);
        optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 0.0f, .cross = OPTICS_UI_STRETCH});
        optics_ui_size(ui, 100.0f, 30.0f);
        optics_ui_column_begin(
            ui, &(optics_ui_layout_opts){.bg = optics_gfx_color_rgba_premul(255, 255, 255, 255)});
        optics_ui_close(ui);
        optics_ui_close(ui);
    }
    optics_ui_scroll_end(ui);
    optics_ui_test_end(ui);

    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 100,
                                                           .height = 40},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(canvas, &width, &height, &stride);
    OT_EXPECT(fb != nullptr && width == 100 && height == 40);
    bool gutter_is_clear = true;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 92; x < width; x++) {
            const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
            gutter_is_clear &= p[0] == 0 && p[1] == 255 && p[2] == 0 && p[3] == 255;
        }
    }
    OT_EXPECT(gutter_is_clear);

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

int main(void) {
    test_scroll_offset();
    test_precise_scroll_uses_pixel_distance();
    test_programmatic_scroll_is_applied_and_clamped();
    test_scrollbar_gutter_clips_overflowing_descendants();
    return OT_REPORT();
}
