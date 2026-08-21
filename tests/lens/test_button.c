/* test_button.c — button click detection. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_link_click_and_intrinsic_size(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_row(ui);
    CHECK(!lens_link(ui, "Playlists"));
    lens_close(ui);
    lens_end(ui);
    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    lens_node *link = lens_node_first_child(row);
    CHECK(link != NULL);
    flux_rect bounds = lens_node_bounds(link);
    CHECK(bounds.w > 0.0f && bounds.w < 100.0f);
    CHECK(bounds.h > 0.0f && bounds.h < 40.0f);

    lens_input in = IN0;
    in.cursor = (flux_point){10.0f, 8.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_row(ui);
    CHECK(!lens_link(ui, "Playlists"));
    CHECK(lens_get_response(ui).hovered);
    lens_close(ui);
    lens_end(ui);

    in = IN0;
    in.cursor = (flux_point){10.0f, 8.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_row(ui);
    CHECK(lens_link(ui, "Playlists"));
    lens_close(ui);
    lens_end(ui);

    lens_destroy(ui);
}

static void test_button_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish prev_rect */
    lens_begin(ui, &IN0);
    bool clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 2: mouse press */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(!clicked); /* clicked only on release */

    /* Frame 3: mouse release */
    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

static void test_button_no_click_when_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;

    lens_begin(ui, &in);
    bool clicked =
        lens_button_ex(ui, (lens_button_opts){.label = "OK", .box = {.disabled = true}}).clicked;
    lens_end(ui);
    CHECK(!clicked);

    lens_destroy(ui);
}

static void test_badged_icon_button_respects_tile_size(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_row(ui);
    lens_size(ui, 48.0f, 44.0f);
    CHECK(!lens_icon_button_badged(ui, LENS_ICON_REPEAT, "1", 26.0f, true));
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    lens_node *tile = lens_node_first_child(row);
    CHECK(tile != NULL);
    flux_rect bounds = lens_node_bounds(tile);
    CHECK_NEAR(bounds.w, 48.0f, 0.01f);
    CHECK_NEAR(bounds.h, 44.0f, 0.01f);

    lens_destroy(ui);
}

static size_t rendered_icon_pixels_with_outline(lens_icon_id icon, bool *center_lit,
                                                bool with_outline, size_t *outline_pixels) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme theme = lens_theme_dark();
    theme.color_fg = flux_color_rgba_premul(255, 255, 255, 255);
    lens_set_theme(ui, theme);

    lens_input input = {.display_size = {24, 24}, .dt_seconds = 0.016f};
    lens_begin(ui, &input);
    if (with_outline) {
        /* The contour is a style atom now (ADR-0061): a scope reaches the
         * bare lens_icon call. */
        lens_style outline = lens_style_init();
        outline.fields = LENS_STYLE_OUTLINE_COLOR | LENS_STYLE_OUTLINE_WIDTH;
        outline.outline_color = flux_color_rgba_premul(255, 0, 0, 255);
        outline.outline_width = 2.0f;
        lens_push_style(ui, outline);
        lens_icon(ui, icon, 24.0f);
        lens_pop_style(ui);
    } else {
        lens_icon(ui, icon, 24.0f);
    }
    lens_end(ui);

    flux_canvas *canvas = NULL;
    CHECK(flux_canvas_create_cpu(24, 24, 1.0f, &canvas) == FLUX_OK);
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(canvas, &black) == FLUX_OK);
    CHECK(lens_render(ui, canvas) == FLUX_OK);
    flux_canvas_cpu_end(canvas);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
    CHECK(fb != NULL && width == 24 && height == 24);
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

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
    if (outline_pixels)
        *outline_pixels = contour;
    return lit;
}

static size_t rendered_icon_pixels(lens_icon_id icon, bool *center_lit) {
    return rendered_icon_pixels_with_outline(icon, center_lit, false, NULL);
}

static void test_material_rounded_star_pair_uses_filled_svg_paths(void) {
    bool outline_center = false;
    bool filled_center = false;
    size_t outline_pixels = rendered_icon_pixels(LENS_ICON_STAR_ROUNDED, &outline_center);
    size_t filled_pixels = rendered_icon_pixels(LENS_ICON_STAR_ROUNDED_FILLED, &filled_center);
    CHECK(outline_pixels > 0);
    CHECK(filled_pixels > 0);
    CHECK(!outline_center);
    CHECK(filled_center);
}

static void test_icon_outline_adds_a_contour_without_changing_intrinsic_size(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_style outline = lens_style_init();
    outline.fields = LENS_STYLE_OUTLINE_COLOR | LENS_STYLE_OUTLINE_WIDTH;
    outline.outline_color = flux_color_rgba_premul(255, 0, 0, 255);
    outline.outline_width = 2.0f;

    lens_begin(ui, &IN0);
    lens_row(ui);
    lens_push_id(ui, "plain");
    lens_icon(ui, LENS_ICON_GLOBE, 24.0f);
    lens_pop_id(ui);
    lens_push_id(ui, "outlined");
    lens_push_style(ui, outline);
    lens_icon(ui, LENS_ICON_GLOBE, 24.0f);
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

    bool center_lit = false;
    size_t contour = 0;
    size_t lit = rendered_icon_pixels_with_outline(LENS_ICON_GLOBE, &center_lit, true, &contour);
    CHECK(lit > 0);
    CHECK(contour > 0);
}

static void test_icon_toggle_swaps_glyph_without_selected_surface(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme theme = lens_theme_dark();
    theme.color_active = flux_color_rgba_premul(255, 0, 0, 255);
    theme.color_accent = flux_color_rgba_premul(0, 255, 0, 255);
    lens_set_theme(ui, theme);

    lens_begin(ui, &IN0);
    lens_row(ui);
    lens_size(ui, 44.0f, 40.0f);
    CHECK(!lens_icon_toggle_button(ui, LENS_ICON_STAR_ROUNDED, LENS_ICON_STAR_ROUNDED_FILLED, 26.0f,
                                   true));
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    lens_node *toggle = lens_node_first_child(row);
    CHECK(toggle != NULL);
    flux_rect bounds = lens_node_bounds(toggle);
    CHECK_NEAR(bounds.w, 44.0f, 0.01f);
    CHECK_NEAR(bounds.h, 40.0f, 0.01f);

    flux_canvas *canvas = NULL;
    CHECK(flux_canvas_create_cpu(44, 40, 1.0f, &canvas) == FLUX_OK);
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_cpu_begin(canvas, &black) == FLUX_OK);
    CHECK(lens_render(ui, canvas) == FLUX_OK);
    flux_canvas_cpu_end(canvas);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
    CHECK(fb != NULL && width == 44 && height == 40);
    const uint8_t *corner = fb + (size_t)1 * stride + (size_t)1 * 4;
    CHECK(corner[0] < 5 && corner[1] < 5 && corner[2] < 5);
    const uint8_t *center = fb + (size_t)20 * stride + (size_t)22 * 4;
    CHECK(center[0] < 5 && center[1] > 250 && center[2] < 5);

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

static void test_icon_button_requests_pointer_cursor(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_icon_button(ui, LENS_ICON_PLAY);
    lens_end(ui);

    lens_input hover = IN0;
    hover.cursor = (flux_point){10.0f, 10.0f};
    lens_begin(ui, &hover);
    lens_icon_button(ui, LENS_ICON_PLAY);
    CHECK(lens_get_cursor_hint(ui) == LENS_CURSOR_POINTER);
    lens_end(ui);

    lens_input away = IN0;
    away.cursor = (flux_point){300.0f, 150.0f};
    lens_begin(ui, &away);
    lens_icon_button(ui, LENS_ICON_PLAY);
    CHECK(lens_get_cursor_hint(ui) == LENS_CURSOR_DEFAULT);
    lens_end(ui);

    lens_destroy(ui);
}

static void test_button_mouse_secondary(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Establish geometry. */
    lens_begin(ui, &IN0);
    lens_button(ui, "sw");
    lens_end(ui);

    /* Right-press then right-release inside: lens_button_mouse(RIGHT)
     * reports it; lens_button (left) does not. */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_down[LENS_MOUSE_RIGHT] = true;
    in.mouse_pressed[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in);
    lens_button(ui, "sw");
    lens_end(ui);

    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_RIGHT] = true;
    lens_begin(ui, &in);
    bool right = lens_button_mouse(ui, "sw", LENS_MOUSE_RIGHT);
    bool left = lens_button(ui, "sw");
    lens_end(ui);
    CHECK(right);
    CHECK(!left);

    lens_destroy(ui);
}

int main(void) {
    test_link_click_and_intrinsic_size();
    test_button_click();
    test_button_mouse_secondary();
    test_button_no_click_when_disabled();
    test_badged_icon_button_respects_tile_size();
    test_material_rounded_star_pair_uses_filled_svg_paths();
    test_icon_outline_adds_a_contour_without_changing_intrinsic_size();
    test_icon_toggle_swaps_glyph_without_selected_surface();
    test_icon_button_requests_pointer_cursor();
    return TEST_REPORT();
}
