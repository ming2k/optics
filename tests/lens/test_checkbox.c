/* test_checkbox.c — checkbox toggle detection. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_checkbox_toggle(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    bool value = false;

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_checkbox(ui, "Enable", &value);
    lens_end(ui);

    /* Press */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed = lens_checkbox(ui, "Enable", &value);
    lens_end(ui);
    CHECK(!changed); /* clicked only on release */
    CHECK(!value);

    /* Release */
    in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    changed = lens_checkbox(ui, "Enable", &value);
    lens_end(ui);

    CHECK(changed);
    CHECK(value);

    lens_destroy(ui);
}

static void test_checkbox_no_toggle_when_disabled(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    bool value = false;

    /* Warm-up frame */
    lens_begin(ui, &IN0);
    lens_checkbox(ui, "Enable", &value);
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 20};
    in.mouse_released[LENS_MOUSE_LEFT] = true;

    lens_begin(ui, &in);
    bool changed =
        lens_checkbox_ex(
            ui, (lens_checkbox_opts){.label = "Enable", .value = &value, .box = {.disabled = true}})
            .changed;
    lens_end(ui);
    CHECK(!changed);
    CHECK(!value);

    lens_destroy(ui);
}

int main(void) {
    test_checkbox_toggle();
    test_checkbox_no_toggle_when_disabled();
    return TEST_REPORT();
}
