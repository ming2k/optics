/* test_tooltip.c — lens_box.tooltip records a tooltip for hovered widgets. */

#include "../../libs/ui/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <optics/gfx/canvas_cpu.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_tooltip_no_hover_no_crash(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_overflowed(ui) == false);
    optics_ui_release(ui);
}

static void test_tooltip_hover_does_not_crash(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: build button */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn"});
    optics_ui_test_end(ui);

    /* frame 2: hover over button, attach tooltip */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 15};
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn", .box = {.tooltip = "Hovered!"}});
    optics_ui_test_end(ui);

    OT_EXPECT(ui->tooltip.active);
    OT_EXPECT(strcmp(ui->tooltip.text, "Hovered!") == 0);
    OT_EXPECT(optics_ui_overflowed(ui) == false);
    optics_ui_release(ui);
}

static void test_tooltip_render_path(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Two frames so prev_rect exists for hover hit-testing. */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 15};
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    optics_ui_test_end(ui);

    OT_EXPECT(ui->tooltip.active);
    OT_EXPECT(strcmp(ui->tooltip.text, "Tip text") == 0);

    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = 200,
                                                           .height = 100},
                                 &canvas) == OPTICS_GFX_OK);
    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(canvas, &width, &height, &stride);
    OT_EXPECT(fb != nullptr && width == 200 && height == 100);

    size_t lit_pixels = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
            if (p[0] > 0 || p[1] > 0 || p[2] > 0)
                lit_pixels++;
        }
    }
    OT_EXPECT(lit_pixels > 0);

    optics_gfx_canvas_release(canvas);
    OT_EXPECT(optics_ui_overflowed(ui) == false);
    optics_ui_release(ui);
}

int main(void) {
    test_tooltip_no_hover_no_crash();
    test_tooltip_hover_does_not_crash();
    test_tooltip_render_path();
    return OT_REPORT();
}
