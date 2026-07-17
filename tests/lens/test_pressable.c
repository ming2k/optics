/* test_pressable.c — composable single-target interaction containers. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static lens_response build_pressable(lens *ui) {
    lens_response response =
        lens_pressable_begin(ui, "track-7", "Play track",
                             (lens_layout_opts){.box = {.width = 240.0f, .height = 76.0f},
                                                .gap = 8.0f,
                                                .pad = 8.0f,
                                                .cross = LENS_CENTER,
                                                .radius = 12.0f});
    lens_icon(ui, LENS_ICON_PLAY, 24.0f);
    lens_label(ui, "A track title");
    lens_pressable_end(ui);
    return response;
}

static void test_complete_row_is_one_hit_target(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    CHECK(!build_pressable(ui).hovered);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){220.0f, 60.0f}; /* trailing whitespace, past both children */
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_response pressed = build_pressable(ui);
    lens_end(ui);
    CHECK(pressed.hovered);
    CHECK(pressed.pressed);
    CHECK(lens_get_cursor_hint(ui) == LENS_CURSOR_POINTER);

    in = IN0;
    in.cursor = (flux_point){220.0f, 60.0f};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_response released = build_pressable(ui);
    lens_end(ui);
    CHECK(released.clicked);

    lens_destroy(ui);
}

int main(void) {
    test_complete_row_is_one_hit_target();
    return TEST_REPORT();
}
