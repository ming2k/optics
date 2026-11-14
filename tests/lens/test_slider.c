/* test_slider.c — slider value change detection. */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>
#include <math.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_slider_drag(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.0f;

    /* Warm-up frame to establish prev_rect */
    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_test_end(ui);

    /* Click on right side of slider track to jump value up */
    lens_input in = IN0;
    in.cursor = (flux_point){120, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed =
        lens_slider(
            ui, &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f})
            .changed;
    lens_test_end(ui);

    /* Value should have increased from the click */
    OT_EXPECT(changed || value > 0.1f);

    lens_release(ui);
}

static void test_slider_disabled(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.5f;

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_test_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){300, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;

    lens_begin(ui, &in);
    bool changed = lens_slider(ui, &(lens_slider_opts){.label = "Volume",
                                                       .value = &value,
                                                       .min = 0.0f,
                                                       .max = 1.0f,
                                                       .box = {.disabled = true}})
                       .changed;
    lens_test_end(ui);
    OT_EXPECT(!changed);
    OT_EXPECT_NEAR(value, 0.5f, 0.001f);

    lens_release(ui);
}

static void test_slider_hover_schedules_feedback_transition(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.5f;

    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_test_end(ui);
    OT_EXPECT(!lens_anim_pending(ui));

    lens_input hover = IN0;
    hover.cursor = (flux_point){80, 16};
    lens_begin(ui, &hover);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    lens_test_end(ui);
    OT_EXPECT(lens_anim_pending(ui));

    lens_release(ui);
}

static void test_slider_geometry_uses_theme_tokens(void) {
    OT_EXPECT_NEAR(lens_theme_default().slider_track_thickness, 6.0f, 0.001f);
    OT_EXPECT_NEAR(lens_theme_default().slider_knob_size, 14.0f, 0.001f);
    OT_EXPECT_NEAR(lens_theme_dark().slider_track_thickness, 6.0f, 0.001f);
    OT_EXPECT_NEAR(lens_theme_dark().slider_knob_size, 14.0f, 0.001f);

    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme theme = lens_theme_dark();
    theme.slider_track_thickness = 2.5f;
    theme.slider_knob_size = 10.0f;
    lens_set_theme(ui, theme);

    float value = 0.5f;
    lens_input hover = IN0;
    hover.cursor = (flux_point){80.0f, 16.0f};
    for (int frame = 0; frame < 90; frame++) {
        lens_begin(ui, &hover);
        lens_slider(
            ui, &(lens_slider_opts){.label = "Compact", .value = &value, .min = 0.0f, .max = 1.0f});
        lens_test_end(ui);
    }

    lens_node *slider = lens_node_first_child(lens_root(ui));
    OT_EXPECT(slider != NULL);
    OT_EXPECT(slider && slider->cmd_count == 3);
    if (slider && slider->cmd_count == 3) {
        const lens_draw_cmd *track = &slider->cmds[0];
        const lens_draw_cmd *fill = &slider->cmds[1];
        const lens_draw_cmd *knob = &slider->cmds[2];
        OT_EXPECT_NEAR(track->rel.h, 2.5f, 0.001f);
        OT_EXPECT_NEAR(fill->rel.h, 2.5f, 0.001f);
        OT_EXPECT_NEAR(track->radius, 1.25f, 0.001f);
        OT_EXPECT_NEAR(knob->rel.w, 10.0f, 0.01f);
        OT_EXPECT_NEAR(knob->rel.h, 10.0f, 0.01f);
    }

    lens_release(ui);
}

static void test_vertical_slider_drag_and_wheel(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    float value = 0.25f;

    lens_begin(ui, &IN0);
    lens_slider(ui, &(lens_slider_opts){.label = "Volume",
                                        .value = &value,
                                        .min = 0.0f,
                                        .max = 1.0f,
                                        .step = 0.05f,
                                        .axis = LENS_COLUMN,
                                        .box = {.width = 44.0f, .height = 160.0f}});
    lens_test_end(ui);

    lens_input drag = IN0;
    drag.cursor = (flux_point){22.0f, 22.0f};
    drag.mouse_pressed[LENS_MOUSE_LEFT] = true;
    drag.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &drag);
    bool changed = lens_slider(ui, &(lens_slider_opts){.label = "Volume",
                                                       .value = &value,
                                                       .min = 0.0f,
                                                       .max = 1.0f,
                                                       .step = 0.05f,
                                                       .axis = LENS_COLUMN,
                                                       .box = {.width = 44.0f, .height = 160.0f}})
                       .changed;
    lens_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(value > 0.8f);

    value = 0.5f;
    lens_input wheel = IN0;
    wheel.cursor = (flux_point){22.0f, 80.0f};
    wheel.scroll_y = 1.0f;
    lens_begin(ui, &wheel);
    changed = lens_slider(ui, &(lens_slider_opts){.label = "Volume",
                                                  .value = &value,
                                                  .min = 0.0f,
                                                  .max = 1.0f,
                                                  .step = 0.05f,
                                                  .axis = LENS_COLUMN,
                                                  .box = {.width = 44.0f, .height = 160.0f}})
                  .changed;
    lens_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT_NEAR(value, 0.55f, 0.001f);

    lens_release(ui);
}

int main(void) {
    test_slider_drag();
    test_slider_disabled();
    test_slider_hover_schedules_feedback_transition();
    test_slider_geometry_uses_theme_tokens();
    test_vertical_slider_drag_and_wheel();
    return OT_REPORT();
}
