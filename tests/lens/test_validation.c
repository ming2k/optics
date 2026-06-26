/* test_validation.c — error state (lens_box.error) on textfield and slider. */

#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_textfield_error_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "test";
    lens_begin(ui, &IN0);
    lens_textfield_ex(
        ui, (lens_textfield_opts){
                .label = "tf", .buf = buf, .buf_cap = sizeof buf, .box = {.error = true}});
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

static void test_slider_error_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    float val = 0.5f;
    lens_begin(ui, &IN0);
    lens_slider_ex(
        ui, (lens_slider_opts){
                .label = "s", .value = &val, .min = 0.0f, .max = 1.0f, .box = {.error = true}});
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

static void test_error_scoped_to_its_own_widget(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Error is carried by the descriptor, so it can only apply to the one
     * widget it is set on — there is no "leaks to the next call" hazard. */
    char buf[64] = "hello";
    lens_begin(ui, &IN0);
    lens_textfield_ex(
        ui, (lens_textfield_opts){
                .label = "tf1", .buf = buf, .buf_cap = sizeof buf, .box = {.error = true}});
    lens_textfield(ui, "tf2", buf, sizeof buf); /* no error */
    lens_end(ui);

    CHECK(lens_overflowed(ui) == false);
    lens_destroy(ui);
}

int main(void) {
    test_textfield_error_no_crash();
    test_slider_error_no_crash();
    test_error_scoped_to_its_own_widget();
    return TEST_REPORT();
}
