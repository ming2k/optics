/* test_button.c — button click detection. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_button_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish prev_rect */
    lens_begin(ui, &IN0);
    bool clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 2: mouse press */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(!clicked); /* clicked only on release */

    /* Frame 3: mouse release */
    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

static void test_button_no_click_when_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;

    lens_begin(ui, &in);
    bool clicked =
        lens_button_ex(ui, (lens_button_opts){.label = "OK", .box = {.disabled = true}}).clicked;
    lens_end(ui);
    CHECK(!clicked);

    lens_destroy(ui);
}

int main(void) {
    test_button_click();
    test_button_no_click_when_disabled();
    return TEST_REPORT();
}
