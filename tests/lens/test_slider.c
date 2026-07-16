/* test_slider.c — slider value change detection. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <math.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_slider_drag(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.0f;

    /* Warm-up frame to establish prev_rect */
    lens_begin(ui, &IN0);
    lens_slider(ui, "Volume", &value, 0.0f, 1.0f);
    lens_end(ui);

    /* Click on right side of slider track to jump value up */
    lens_input in = IN0;
    in.cursor = (flux_point){300, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed = lens_slider(ui, "Volume", &value, 0.0f, 1.0f);
    lens_end(ui);

    /* Value should have increased from the click */
    CHECK(changed || value > 0.1f);

    lens_destroy(ui);
}

static void test_slider_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.5f;

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_slider(ui, "Volume", &value, 0.0f, 1.0f);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){300, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;

    lens_begin(ui, &in);
    bool changed = lens_slider_ex(ui, (lens_slider_opts){.label = "Volume",
                                                         .value = &value,
                                                         .min = 0.0f,
                                                         .max = 1.0f,
                                                         .box = {.disabled = true}})
                       .changed;
    lens_end(ui);
    CHECK(!changed);
    CHECK_NEAR(value, 0.5f, 0.001f);

    lens_destroy(ui);
}

static void test_slider_hover_schedules_knob_animation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.5f;

    lens_begin(ui, &IN0);
    lens_slider(ui, "Volume", &value, 0.0f, 1.0f);
    lens_end(ui);
    CHECK(!lens_anim_pending(ui));

    lens_input hover = IN0;
    hover.cursor = (flux_point){80, 16};
    lens_begin(ui, &hover);
    lens_slider(ui, "Volume", &value, 0.0f, 1.0f);
    lens_end(ui);
    CHECK(lens_anim_pending(ui));

    lens_destroy(ui);
}

static void test_vertical_slider_drag_and_wheel(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.25f;

    lens_begin(ui, &IN0);
    lens_size(ui, 44.0f, 160.0f);
    lens_slider_vertical(ui, "Volume", &value, 0.0f, 1.0f, 0.05f);
    lens_end(ui);

    lens_input drag = IN0;
    drag.cursor = (flux_point){22.0f, 22.0f};
    drag.mouse_pressed[LENS_MOUSE_LEFT] = true;
    drag.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &drag);
    lens_size(ui, 44.0f, 160.0f);
    CHECK(lens_slider_vertical(ui, "Volume", &value, 0.0f, 1.0f, 0.05f));
    lens_end(ui);
    CHECK(value > 0.8f);

    value = 0.5f;
    lens_input wheel = IN0;
    wheel.cursor = (flux_point){22.0f, 80.0f};
    wheel.scroll_y = 1.0f;
    lens_begin(ui, &wheel);
    lens_size(ui, 44.0f, 160.0f);
    CHECK(lens_slider_vertical(ui, "Volume", &value, 0.0f, 1.0f, 0.05f));
    lens_end(ui);
    CHECK_NEAR(value, 0.55f, 0.001f);

    lens_destroy(ui);
}

static void test_hovered_trigger_adjusts_on_wheel(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.5f;

    lens_begin(ui, &IN0);
    (void)lens_button(ui, "Volume trigger");
    lens_end(ui);

    lens_input wheel = IN0;
    wheel.cursor = (flux_point){20.0f, 20.0f};
    wheel.scroll_y = -1.0f;
    lens_begin(ui, &wheel);
    (void)lens_button(ui, "Volume trigger");
    CHECK(lens_adjust_float_on_scroll(ui, &value, 0.0f, 1.0f, 0.05f));
    lens_end(ui);
    CHECK_NEAR(value, 0.45f, 0.001f);

    lens_destroy(ui);
}

int main(void) {
    test_slider_drag();
    test_slider_disabled();
    test_slider_hover_schedules_knob_animation();
    test_vertical_slider_drag_and_wheel();
    test_hovered_trigger_adjusts_on_wheel();
    return TEST_REPORT();
}
