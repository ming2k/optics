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

static void test_badged_icon_button_respects_tile_size(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_row(ui);
    lens_size(ui, 48.0f, 44.0f);
    CHECK(!lens_icon_button_badged(ui, LENS_ICON_REPEAT, "1", 26.0f, true));
    lens_close(ui);
    lens_end(ui);

    lens_node *row = lens_node_first_child(lens_root(ui));
    CHECK(row != NULL);
    lens_node *tile = lens_node_first_child(row);
    CHECK(tile != NULL);
    flux_rect bounds = lens_node_bounds(tile);
    CHECK_NEAR(bounds.w, 48.0f, 0.01f);
    CHECK_NEAR(bounds.h, 44.0f, 0.01f);

    lens_destroy(ui);
}

int main(void) {
    test_button_click();
    test_button_no_click_when_disabled();
    test_badged_icon_button_respects_tile_size();
    return TEST_REPORT();
}
