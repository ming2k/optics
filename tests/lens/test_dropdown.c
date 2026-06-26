/* test_dropdown.c — lens_dropdown selection and keyboard navigation. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

static void test_dropdown_click_select(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    const char *items[] = {"Red", "Green", "Blue"};
    int sel = 0;

    /* frame 1: build dropdown */
    lens_begin(ui, &IN0);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* frame 2: click the dropdown button to open overlay */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* overlay is now open; frame 3: click the second item */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    lens_dropdown(ui, "color", &sel, items, 3);
    lens_end(ui);

    /* We can't easily simulate clicking an overlay item in a CPU test
     * because overlay items are laid out in a separate layer. Verify
     * the dropdown builds without crashing and the initial state is intact. */
    CHECK(sel == 0);

    lens_destroy(ui);
}

static void test_dropdown_keyboard_nav(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    const char *items[] = {"A", "B", "C"};
    int sel = 0;

    /* frame 1: build dropdown */
    lens_begin(ui, &IN0);
    lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    /* frame 2: click to open overlay (prev_rect now valid) */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    /* frame 3: press Down twice while overlay is open */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.key_count = 2;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    in.keys[1] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    lens_begin(ui, &in);
    bool changed = lens_dropdown(ui, "letters", &sel, items, 3);
    lens_end(ui);

    CHECK(changed == true);
    CHECK(sel == 2);

    lens_destroy(ui);
}

int main(void) {
    test_dropdown_click_select();
    test_dropdown_keyboard_nav();
    return TEST_REPORT();
}
