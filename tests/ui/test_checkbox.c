/* test_checkbox.c — checkbox toggle detection. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_checkbox_toggle(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    bool value = false;

    /* Warm-up frame */
    optics_ui_begin(ui, &IN0);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Enable", .value = &value});
    optics_ui_test_end(ui);

    /* Press */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Enable", .value = &value})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(!changed); /* clicked only on release */
    OT_EXPECT(!value);

    /* Release */
    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    changed = optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Enable", .value = &value})
                  .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(value);

    optics_ui_release(ui);
}

static void test_checkbox_no_toggle_when_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    bool value = false;

    /* Warm-up frame */
    optics_ui_begin(ui, &IN0);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Enable", .value = &value});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;

    optics_ui_begin(ui, &in);
    bool changed = optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Enable",
                                                                     .value = &value,
                                                                     .box = {.disabled = true}})
                       .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(!changed);
    OT_EXPECT(!value);

    optics_ui_release(ui);
}

static void test_switch_toggle_and_disabled(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    bool value = false;

    optics_ui_begin(ui, &IN0);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Tap to click",
                                                      .appearance = OPTICS_UI_CHECKBOX_SWITCH,
                                                      .value = &value});
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Tap to click",
                                                      .appearance = OPTICS_UI_CHECKBOX_SWITCH,
                                                      .value = &value});
    optics_ui_test_end(ui);

    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_response r = optics_ui_checkbox(
        ui, &(optics_ui_checkbox_opts){
                .label = "Tap to click", .appearance = OPTICS_UI_CHECKBOX_SWITCH, .value = &value});
    optics_ui_test_end(ui);
    OT_EXPECT(r.changed);
    OT_EXPECT(value);

    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.box = {.disabled = true},
                                                      .label = "Tap to click",
                                                      .appearance = OPTICS_UI_CHECKBOX_SWITCH,
                                                      .value = &value});
    optics_ui_test_end(ui);
    in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    r = optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.box = {.disabled = true},
                                                          .label = "Tap to click",
                                                          .appearance = OPTICS_UI_CHECKBOX_SWITCH,
                                                          .value = &value});
    optics_ui_test_end(ui);
    OT_EXPECT(!r.changed);
    OT_EXPECT(value);

    optics_ui_release(ui);
}

int main(void) {
    test_checkbox_toggle();
    test_checkbox_no_toggle_when_disabled();
    test_switch_toggle_and_disabled();
    return OT_REPORT();
}
