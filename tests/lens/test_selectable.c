/* test_selectable.c — selectable row click + selected-state semantics. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_selectable_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Frame 1: establish prev_rect */
    lens_begin(ui, &IN0);
    bool clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 2: press (no click yet) */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(!clicked);

    /* Frame 3: release inside -> click */
    in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    clicked = lens_selectable(ui, "Note", false);
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

/* The selected flag is surfaced to assistive tech as a checked state, and a
 * disabled selectable never reports a click. */
static void test_selectable_selected_and_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_selectable(ui, "Note", true);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 12};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked = lens_selectable_ex(ui, (lens_selectable_opts){.label = "Note",
                                                                 .selected = true,
                                                                 .box = {.disabled = true}})
                       .clicked;
    lens_end(ui);
    CHECK(!clicked);

    lens_destroy(ui);
}

int main(void) {
    test_selectable_click();
    test_selectable_selected_and_disabled();
    return TEST_REPORT();
}
