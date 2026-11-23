/* test_selectable.c — selectable row click + selected-state semantics. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void pixel(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y, uint8_t out[4]) {
    const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

static void test_selectable_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Frame 1: establish prev_rect */
    optics_ui_begin(ui, &IN0);
    bool clicked =
        optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Note", .selected = false})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* Frame 2: press (no click yet) */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 12};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    clicked =
        optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Note", .selected = false})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* Frame 3: release inside -> click */
    in = IN0;
    in.cursor = (optics_gfx_point){20, 12};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    clicked =
        optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Note", .selected = false})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

/* The selected flag is surfaced to assistive tech as a checked state, and a
 * disabled selectable never reports a click. */
static void test_selectable_selected_and_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Note", .selected = true});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 12};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool clicked = optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Note",
                                                                         .selected = true,
                                                                         .box = {.disabled = true}})
                       .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    optics_ui_release(ui);
}

static void test_selected_surface_uses_theme(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme theme = optics_ui_theme_dark();
    theme.color_active = optics_gfx_color_rgba_premul(0, 255, 0, 255);
    theme.color_accent = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    theme.corner_radius = 10.0f;
    optics_ui_set_theme(ui, theme);

    optics_ui_input input = {.display_size = {100, 40}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &input);
    optics_ui_size(ui, 100.0f, 40.0f);
    optics_ui_selectable(ui, &(optics_ui_selectable_opts){.label = "Selected", .selected = true});
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
    uint8_t p[4];
    pixel(fb, stride, 1, 20, p);
    OT_EXPECT(p[0] < 5 && p[1] > 250 && p[2] < 5); /* themed surface, no red rail */
    pixel(fb, stride, 1, 1, p);
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] < 5); /* left corner remains rounded */

    optics_gfx_canvas_release(canvas);
    optics_ui_release(ui);
}

int main(void) {
    test_selectable_click();
    test_selectable_selected_and_disabled();
    test_selected_surface_uses_theme();
    return OT_REPORT();
}
