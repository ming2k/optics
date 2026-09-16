/* test_tooltip.c — lens_box.tooltip records a tooltip for hovered widgets. */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <flux/canvas_cpu.h>
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_tooltip_no_hover_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    test_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_release(ui);
}

static void test_tooltip_hover_does_not_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: build button */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "btn"});
    test_end(ui);

    /* frame 2: hover over button, attach tooltip */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 15};
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "btn", .box = {.tooltip = "Hovered!"}});
    test_end(ui);

    CHECK(ui->tooltip.active);
    CHECK(strcmp(ui->tooltip.text, "Hovered!") == 0);
    CHECK(lens_overflowed(ui) == false);
    lens_release(ui);
}

static void test_tooltip_render_path(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Two frames so prev_rect exists for hover hit-testing. */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    test_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 15};
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    test_end(ui);

    CHECK(ui->tooltip.active);
    CHECK(strcmp(ui->tooltip.text, "Tip text") == 0);

    flux_canvas *canvas = NULL;
    CHECK(flux_canvas_create_cpu(200, 100, 1.0f, &canvas) == FLUX_OK);
    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    CHECK(flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                             .clear_color = &black}) == FLUX_OK);
    CHECK(test_snapshot_render(ui, canvas) == FLUX_OK);
    CHECK(flux_canvas_end(canvas) == FLUX_OK);

    uint32_t width = 0, height = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(canvas, &width, &height, &stride);
    CHECK(fb != NULL && width == 200 && height == 100);

    size_t lit_pixels = 0;
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
            if (p[0] > 0 || p[1] > 0 || p[2] > 0)
                lit_pixels++;
        }
    }
    CHECK(lit_pixels > 0);

    flux_canvas_release(canvas);
    CHECK(lens_overflowed(ui) == false);
    lens_release(ui);
}

int main(void) {
    test_tooltip_no_hover_no_crash();
    test_tooltip_hover_does_not_crash();
    test_tooltip_render_path();
    return TEST_REPORT();
}
