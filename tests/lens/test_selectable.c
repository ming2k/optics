/* test_selectable.c — selectable row click + selected-state semantics. */

#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void pixel(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y, uint8_t out[4]) {
    const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

static void test_selectable_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish prev_rect */
    lens_begin(ui, &IN0);
    bool clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 2: press (no click yet) */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 3: release inside -> click */
    in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

/* The selected flag is surfaced to assistive tech as a checked state, and a
 * disabled selectable never reports a click. */
static void test_selectable_selected_and_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_selectable(ui, "Note", true);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked = lens_selectable_ex(ui, (lens_selectable_opts){.label = "Note",
                                                                 .selected = true,
                                                                 .box = {.disabled = true}})
                       .clicked;
    lens_end(ui);
    CHECK(!clicked);

    lens_destroy(ui);
}

static void test_selected_surface_uses_theme(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme theme = lens_theme_dark();
    theme.color_active = flux_color_rgba_premul(0, 255, 0, 255);
    theme.color_accent = flux_color_rgba_premul(255, 0, 0, 255);
    theme.corner_radius = 10.0f;
    lens_set_theme(ui, theme);

    lens_input input = {.display_size = {100, 40}, .dt_seconds = 0.016f};
    lens_begin(ui, &input);
    lens_size(ui, 100.0f, 40.0f);
    lens_selectable(ui, "Selected", true);
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
    uint8_t p[4];
    pixel(fb, stride, 1, 20, p);
    CHECK(p[0] < 5 && p[1] > 250 && p[2] < 5); /* themed surface, no red rail */
    pixel(fb, stride, 1, 1, p);
    CHECK(p[0] < 5 && p[1] < 5 && p[2] < 5); /* left corner remains rounded */

    flux_canvas_destroy(canvas);
    lens_destroy(ui);
}

int main(void) {
    test_selectable_click();
    test_selectable_selected_and_disabled();
    test_selected_surface_uses_theme();
    return TEST_REPORT();
}
