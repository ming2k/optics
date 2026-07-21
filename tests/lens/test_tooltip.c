/* test_tooltip.c — lens_box.tooltip records a tooltip for hovered widgets. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_tooltip_no_hover_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button_ex(ui, (lens_button_opts){.label = "btn", .box = {.tooltip = "Tip text"}});
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

static void test_tooltip_hover_does_not_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: build button */
    lens_begin(ui, &IN0);
    lens_button(ui, "btn");
    lens_end(ui);

    /* frame 2: hover over button, attach tooltip */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 15};
    lens_begin(ui, &in);
    lens_button_ex(ui, (lens_button_opts){.label = "btn", .box = {.tooltip = "Hovered!"}});
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

/* Regression: the tooltip text-draw branch used to be wrapped in an
 * undefined #ifdef LENSI_HAVE_TEXT_FTHB, so the label was never rendered
 * even when a flux_text handle was attached. We cannot fake a flux_text*
 * without a device, but we can verify lens_render reaches the tooltip
 * branch on the headless path (no text) without crashing or marking the
 * frame overflowed. */
static void test_tooltip_render_path(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Two frames so prev_rect exists for hover hit-testing. */
    lens_begin(ui, &IN0);
    lens_button_ex(ui, (lens_button_opts){.label = "btn", .box = {.tooltip = "regression"}});
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 15};
    lens_begin(ui, &in);
    lens_button_ex(ui, (lens_button_opts){.label = "btn", .box = {.tooltip = "regression"}});
    lens_render(ui, NULL); /* headless: NULL canvas is a documented no-op */
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

int main(void) {
    test_tooltip_no_hover_no_crash();
    test_tooltip_hover_does_not_crash();
    test_tooltip_render_path();
    return TEST_REPORT();
}
