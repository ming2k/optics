/* test_disabled.c — disabled widget state (lens_box.disabled).
 *
 * Verifies that disabled widgets do not respond to clicks, do not
 * receive focus, and report LENS_A11Y_DISABLED in semantics. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_disabled_button_no_click(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* frame 1: build disabled button at (0,0) 100x30 */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "Click me", .box = {.disabled = true}});
    lens_end(ui);

    /* frame 2: click inside the button */
    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked =
        lens_button(ui, &(lens_button_opts){.label = "Click me", .box = {.disabled = true}})
            .clicked;
    lens_end(ui);

    CHECK(clicked == false);

    lens_destroy(ui);
}

static void test_disabled_slider_no_change(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    float val = 0.5f;

    /* frame 1 */
    lens_begin(ui, &IN0);
    lens_slider(
        ui, &(lens_slider_opts){
                .label = "s", .value = &val, .min = 0.0f, .max = 1.0f, .box = {.disabled = true}});
    lens_end(ui);

    /* frame 2: drag inside slider */
    lens_input in = IN0;
    in.cursor = (flux_point){80, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool changed = lens_slider(ui, &(lens_slider_opts){.label = "s",
                                                       .value = &val,
                                                       .min = 0.0f,
                                                       .max = 1.0f,
                                                       .box = {.disabled = true}})
                       .changed;
    lens_end(ui);

    CHECK(changed == false);
    CHECK_NEAR(val, 0.5f, 0.001f);

    lens_destroy(ui);
}

static void test_disabled_checkbox_no_toggle(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    bool on = false;

    lens_input in = IN0;
    in.cursor = (flux_point){50, 15};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_checkbox(ui,
                  &(lens_checkbox_opts){.label = "cb", .value = &on, .box = {.disabled = true}});
    lens_end(ui);

    CHECK(on == false);

    lens_destroy(ui);
}

static void test_disabled_textfield_no_input(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "hello";

    /* frame 1: build disabled textfield */
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){
                          .box = {.id = "tf", .disabled = true}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);

    /* frame 2: send keys — should be ignored */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = 'X', .pressed = true};
    in.text_utf8[0] = 'X';
    in.text_utf8[1] = '\0';
    lens_begin(ui, &in);
    bool changed = lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .disabled = true},
                                                           .buf = buf,
                                                           .cap = sizeof buf})
                       .changed;
    lens_end(ui);

    CHECK(changed == false);
    CHECK(strcmp(buf, "hello") == 0);

    lens_destroy(ui);
}

static void a11y_cb(const lens_semantics *s, flux_rect bounds, lens_id id, lens_id parent,
                    void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    if (s->flags & LENS_A11Y_DISABLED)
        *(bool *)user = true;
}

static void test_disabled_a11y_flag(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "disabled", .box = {.disabled = true}});
    lens_end(ui);

    bool found = false;
    lens_accessibility_walk(ui, a11y_cb, &found);

    CHECK(found == true);

    lens_destroy(ui);
}

int main(void) {
    test_disabled_button_no_click();
    test_disabled_slider_no_change();
    test_disabled_checkbox_no_toggle();
    test_disabled_textfield_no_input();
    test_disabled_a11y_flag();
    return TEST_REPORT();
}
