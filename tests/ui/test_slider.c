/* test_slider.c — slider value change detection. */

#include "../../libs/ui/src/internal.h"
#include "optics_test.h"
#include "ui_test.h"
#include <math.h>
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_slider_drag(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    float value = 0.0f;

    /* Warm-up frame to establish prev_rect */
    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);

    /* Click on right side of slider track to jump value up */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){120, 15};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_slider(
            ui,
            &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f})
            .changed;
    optics_ui_test_end(ui);

    /* Value should have increased from the click */
    OT_EXPECT(changed || value > 0.1f);

    optics_ui_release(ui);
}

static void test_slider_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    float value = 0.5f;

    /* Warm-up frame */
    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){300, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;

    optics_ui_begin(ui, &in);
    bool changed = optics_ui_slider(ui, &(optics_ui_slider_opts){.label = "Volume",
                                                                 .value = &value,
                                                                 .min = 0.0f,
                                                                 .max = 1.0f,
                                                                 .box = {.disabled = true}})
                       .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(!changed);
    OT_EXPECT_NEAR(value, 0.5f, 0.001f);

    optics_ui_release(ui);
}

static void test_slider_hover_schedules_feedback_transition(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    float value = 0.5f;

    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_anim_pending(ui));

    optics_ui_input hover = IN0;
    hover.cursor = (optics_gfx_point){80, 16};
    optics_ui_begin(ui, &hover);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &value, .min = 0.0f, .max = 1.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_anim_pending(ui));

    optics_ui_release(ui);
}

static void test_slider_geometry_uses_theme_tokens(void) {
    OT_EXPECT_NEAR(optics_ui_theme_default().slider_track_thickness, 6.0f, 0.001f);
    OT_EXPECT_NEAR(optics_ui_theme_default().slider_knob_size, 14.0f, 0.001f);
    OT_EXPECT_NEAR(optics_ui_theme_dark().slider_track_thickness, 6.0f, 0.001f);
    OT_EXPECT_NEAR(optics_ui_theme_dark().slider_knob_size, 14.0f, 0.001f);

    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_theme theme = optics_ui_theme_dark();
    theme.slider_track_thickness = 2.5f;
    theme.slider_knob_size = 10.0f;
    optics_ui_set_theme(ui, theme);

    float value = 0.5f;
    optics_ui_input hover = IN0;
    hover.cursor = (optics_gfx_point){80.0f, 16.0f};
    for (int frame = 0; frame < 90; frame++) {
        optics_ui_begin(ui, &hover);
        optics_ui_slider(ui, &(optics_ui_slider_opts){
                                 .label = "Compact", .value = &value, .min = 0.0f, .max = 1.0f});
        optics_ui_test_end(ui);
    }

    optics_ui_node *slider = optics_ui_node_first_child(optics_ui_root(ui));
    OT_EXPECT(slider != nullptr);
    OT_EXPECT(slider && slider->cmd_count == 3);
    if (slider && slider->cmd_count == 3) {
        const optics_ui_draw_cmd *track = &slider->cmds[0];
        const optics_ui_draw_cmd *fill = &slider->cmds[1];
        const optics_ui_draw_cmd *knob = &slider->cmds[2];
        OT_EXPECT_NEAR(track->rel.h, 2.5f, 0.001f);
        OT_EXPECT_NEAR(fill->rel.h, 2.5f, 0.001f);
        OT_EXPECT_NEAR(track->radius, 1.25f, 0.001f);
        OT_EXPECT_NEAR(knob->rel.w, 10.0f, 0.01f);
        OT_EXPECT_NEAR(knob->rel.h, 10.0f, 0.01f);
    }

    optics_ui_release(ui);
}

static void test_vertical_slider_drag_and_wheel(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    float value = 0.25f;

    optics_ui_begin(ui, &IN0);
    optics_ui_slider(ui, &(optics_ui_slider_opts){.label = "Volume",
                                                  .value = &value,
                                                  .min = 0.0f,
                                                  .max = 1.0f,
                                                  .step = 0.05f,
                                                  .axis = OPTICS_UI_COLUMN,
                                                  .box = {.width = 44.0f, .height = 160.0f}});
    optics_ui_test_end(ui);

    optics_ui_input drag = IN0;
    drag.cursor = (optics_gfx_point){22.0f, 22.0f};
    drag.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    drag.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &drag);
    bool changed =
        optics_ui_slider(ui, &(optics_ui_slider_opts){.label = "Volume",
                                                      .value = &value,
                                                      .min = 0.0f,
                                                      .max = 1.0f,
                                                      .step = 0.05f,
                                                      .axis = OPTICS_UI_COLUMN,
                                                      .box = {.width = 44.0f, .height = 160.0f}})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(value > 0.8f);

    value = 0.5f;
    optics_ui_input wheel = IN0;
    wheel.cursor = (optics_gfx_point){22.0f, 80.0f};
    wheel.scroll_y = 1.0f;
    optics_ui_begin(ui, &wheel);
    changed =
        optics_ui_slider(ui, &(optics_ui_slider_opts){.label = "Volume",
                                                      .value = &value,
                                                      .min = 0.0f,
                                                      .max = 1.0f,
                                                      .step = 0.05f,
                                                      .axis = OPTICS_UI_COLUMN,
                                                      .box = {.width = 44.0f, .height = 160.0f}})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT_NEAR(value, 0.55f, 0.001f);

    optics_ui_release(ui);
}

int main(void) {
    test_slider_drag();
    test_slider_disabled();
    test_slider_hover_schedules_feedback_transition();
    test_slider_geometry_uses_theme_tokens();
    test_vertical_slider_drag_and_wheel();
    return OT_REPORT();
}
