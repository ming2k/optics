/* test_pressable.c — composable single-target interaction containers. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static optics_ui_response build_pressable(optics_ui *ui) {
    optics_ui_response response = optics_ui_pressable_begin(
        ui, &(optics_ui_pressable_opts){
                .box = {.id = "track-7", .width = 240.0f, .height = 76.0f},
                .label = "Play track",
                .layout =
                    {
                        .gap = 8.0f,
                        .pad = 8.0f,
                        .cross = OPTICS_UI_CENTER,
                        .corner_radii = optics_gfx_corner_radii_uniform(12.0f),
                    },
            });
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = OPTICS_UI_ICON_CHEVRON_RIGHT, .size = 24.0f});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "A track title"});
    optics_ui_pressable_end(ui);
    return response;
}

static void test_complete_row_is_one_hit_target(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    OT_EXPECT(!build_pressable(ui).hovered);
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){220.0f, 60.0f}; /* trailing whitespace, past both children */
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_response pressed = build_pressable(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(pressed.hovered);
    OT_EXPECT(pressed.pressed);
    OT_EXPECT(optics_ui_get_cursor_hint(ui) == OPTICS_UI_CURSOR_POINTER);

    in = IN0;
    in.cursor = (optics_gfx_point){220.0f, 60.0f};
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_response released = build_pressable(ui);
    optics_ui_test_end(ui);
    OT_EXPECT(released.clicked);

    optics_ui_release(ui);
}

int main(void) {
    test_complete_row_is_one_hit_target();
    return OT_REPORT();
}
