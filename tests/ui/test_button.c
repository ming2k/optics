/* test_button.c — button click detection. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_link_click_and_intrinsic_size(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_row_begin(ui, nullptr);
    OT_EXPECT(!optics_ui_button(ui, &(optics_ui_button_opts){.label = "Playlists",
                                                             .variant = OPTICS_UI_BUTTON_LINK})
                   .clicked);
    optics_ui_close(ui);
    optics_ui_test_end(ui);
    optics_ui_node *row = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(row != nullptr);
    optics_ui_node *link = optics_ui_node_first_child(row);
    OT_EXPECT(link != nullptr);
    optics_gfx_rect bounds = optics_ui_node_bounds(link);
    OT_EXPECT(bounds.w > 0.0f && bounds.w < 100.0f);
    OT_EXPECT(bounds.h > 0.0f && bounds.h < 40.0f);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){10.0f, 8.0f};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    OT_EXPECT(!optics_ui_button(ui, &(optics_ui_button_opts){.label = "Playlists",
                                                             .variant = OPTICS_UI_BUTTON_LINK})
                   .clicked);
    OT_EXPECT(optics_ui_get_response(ui).hovered);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    in = IN0;
    in.cursor = (optics_gfx_point){10.0f, 8.0f};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_row_begin(ui, nullptr);
    OT_EXPECT(optics_ui_button(ui, &(optics_ui_button_opts){.label = "Playlists",
                                                            .variant = OPTICS_UI_BUTTON_LINK})
                  .clicked);
    optics_ui_close(ui);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
}

static void test_button_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Frame 1: establish prev_rect */
    optics_ui_begin(ui, &IN0);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* Frame 2: mouse press */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked); /* clicked only on release */

    /* Frame 3: mouse release */
    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

static void test_button_no_click_when_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Warm-up frame */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;

    optics_ui_begin(ui, &in);
    bool clicked =
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK", .box = {.disabled = true}})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    optics_ui_release(ui);
}

static size_t rendered_icon_pixels_with_outline(optics_ui_icon_id icon, bool *center_lit,
                                                bool with_outline, size_t *outline_pixels) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme theme = optics_ui_theme_dark();
    theme.color_fg = optics_gfx_color_rgba_premul(255, 255, 255, 255);
    optics_ui_set_theme(ui, theme);

    optics_ui_input input = {.display_size = {24, 24}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &input);
    if (with_outline) {
        optics_ui_style outline = optics_ui_style_init();
        outline.fields = OPTICS_UI_STYLE_OUTLINE_COLOR | OPTICS_UI_STYLE_OUTLINE_WIDTH;
        outline.outline_color = optics_gfx_color_rgba_premul(255, 0, 0, 255);
        outline.outline_width = 2.0f;
        optics_ui_push_style(ui, outline);
        optics_ui_icon(ui, &(optics_ui_icon_opts){.id = icon, .size = 24.0f});
        optics_ui_pop_style(ui);
    } else {
        optics_ui_icon(ui, &(optics_ui_icon_opts){.id = icon, .size = 24.0f});
    }
    optics_ui_test_end(ui);

    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 24,
                                                           .height = 24},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(canvas, &width, &height, &stride);
    OT_EXPECT(fb != nullptr && width == 24 && height == 24);
    const uint8_t *center = fb + (size_t)12 * stride + (size_t)12 * 4;
    *center_lit = center[0] > 16 || center[1] > 16 || center[2] > 16;
    size_t lit = 0;
    size_t contour = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *pixel = fb + (size_t)y * stride + (size_t)x * 4;
            if (pixel[0] > 16 || pixel[1] > 16 || pixel[2] > 16)
                lit++;
            if (pixel[0] > 80 && pixel[0] > pixel[1] * 2 && pixel[0] > pixel[2] * 2)
                contour++;
        }
    }

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
    if (outline_pixels)
        *outline_pixels = contour;
    return lit;
}

static size_t rendered_icon_pixels(optics_ui_icon_id icon, bool *center_lit) {
    return rendered_icon_pixels_with_outline(icon, center_lit, false, nullptr);
}

static void test_material_rounded_star_pair_uses_filled_svg_paths(void) {
    bool outline_center = false;
    bool filled_center = false;
    size_t outline_pixels = rendered_icon_pixels(OPTICS_UI_ICON_STAR_ROUNDED, &outline_center);
    size_t filled_pixels = rendered_icon_pixels(OPTICS_UI_ICON_STAR_ROUNDED_FILLED, &filled_center);
    OT_EXPECT(outline_pixels > 0);
    OT_EXPECT(filled_pixels > 0);
    OT_EXPECT(!outline_center);
    OT_EXPECT(filled_center);
}

static void test_icon_outline_adds_a_contour_without_changing_intrinsic_size(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_style outline = optics_ui_style_init();
    outline.fields = OPTICS_UI_STYLE_OUTLINE_COLOR | OPTICS_UI_STYLE_OUTLINE_WIDTH;
    outline.outline_color = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    outline.outline_width = 2.0f;

    optics_ui_begin(ui, &IN0);
    optics_ui_row_begin(ui, nullptr);
    optics_ui_push_id(ui, "plain");
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_GLOBE, .size = 24.0f});
    optics_ui_pop_id(ui);
    optics_ui_push_id(ui, "outlined");
    optics_ui_push_style(ui, outline);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_GLOBE, .size = 24.0f});
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

    bool center_lit = false;
    size_t contour = 0;
    size_t lit =
        rendered_icon_pixels_with_outline(OPTICS_UI_ICON_GLOBE, &center_lit, true, &contour);
    OT_EXPECT(lit > 0);
    OT_EXPECT(contour > 0);
}

static void test_icon_button_requests_pointer_cursor(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.icon = OPTICS_UI_ICON_PLAY});
    optics_ui_test_end(ui);

    optics_ui_input hover = IN0;
    hover.cursor = (optics_gfx_point){10.0f, 10.0f};
    optics_ui_begin(ui, &hover);
    optics_ui_button(ui, &(optics_ui_button_opts){.icon = OPTICS_UI_ICON_PLAY});
    OT_EXPECT(optics_ui_get_cursor_hint(ui) == OPTICS_UI_CURSOR_POINTER);
    optics_ui_test_end(ui);

    optics_ui_input away = IN0;
    away.cursor = (optics_gfx_point){300.0f, 150.0f};
    optics_ui_begin(ui, &away);
    optics_ui_button(ui, &(optics_ui_button_opts){.icon = OPTICS_UI_ICON_PLAY});
    OT_EXPECT(optics_ui_get_cursor_hint(ui) == OPTICS_UI_CURSOR_DEFAULT);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
}

static void test_button_mouse_secondary(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Establish geometry. */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "sw"});
    optics_ui_test_end(ui);

    /* Right-press then right-release inside: lens_button(mouse_button=RIGHT)
     * reports it; lens_button (left) does not. */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_down[OPTICS_UI_MOUSE_RIGHT] = true;
    in.mouse_pressed[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "sw"});
    optics_ui_test_end(ui);

    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_RIGHT] = true;
    optics_ui_begin(ui, &in);
    bool right =
        optics_ui_button(
            ui, &(optics_ui_button_opts){.label = "sw", .mouse_button = OPTICS_UI_MOUSE_RIGHT})
            .clicked;
    bool left = optics_ui_button(ui, &(optics_ui_button_opts){.label = "sw"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(right);
    OT_EXPECT(!left);

    optics_ui_release(ui);
}

int main(void) {
    test_link_click_and_intrinsic_size();
    test_button_click();
    test_button_mouse_secondary();
    test_button_no_click_when_disabled();
    test_material_rounded_star_pair_uses_filled_svg_paths();
    test_icon_outline_adds_a_contour_without_changing_intrinsic_size();
    test_icon_button_requests_pointer_cursor();
    return OT_REPORT();
}
