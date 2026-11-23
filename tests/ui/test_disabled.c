/* test_disabled.c — disabled widget state (optics_ui_box.disabled).
 *
 * Verifies that disabled widgets do not respond to clicks, do not
 * receive focus, and report OPTICS_UI_A11Y_DISABLED in semantics. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_disabled_button_no_click(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: build disabled button at (0,0) 100x30 */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Click me", .box = {.disabled = true}});
    optics_ui_test_end(ui);

    /* frame 2: click inside the button */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50, 15};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool clicked = optics_ui_button(
                       ui, &(optics_ui_button_opts){.label = "Click me", .box = {.disabled = true}})
                       .clicked;
    optics_ui_test_end(ui);

    OT_EXPECT(clicked == false);

    optics_ui_release(ui);
}

static void test_disabled_slider_no_change(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    float val = 0.5f;

    /* frame 1 */
    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){
                .label = "s", .value = &val, .min = 0.0f, .max = 1.0f, .box = {.disabled = true}});
    optics_ui_test_end(ui);

    /* frame 2: drag inside slider */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){80, 15};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool changed = optics_ui_slider(ui, &(optics_ui_slider_opts){.label = "s",
                                                                 .value = &val,
                                                                 .min = 0.0f,
                                                                 .max = 1.0f,
                                                                 .box = {.disabled = true}})
                       .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed == false);
    OT_EXPECT_NEAR(val, 0.5f, 0.001f);

    optics_ui_release(ui);
}

static void test_disabled_checkbox_no_toggle(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool on = false;

    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50, 15};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_checkbox(
        ui, &(optics_ui_checkbox_opts){.label = "cb", .value = &on, .box = {.disabled = true}});
    optics_ui_test_end(ui);

    OT_EXPECT(on == false);

    optics_ui_release(ui);
}

static void test_disabled_textfield_no_input(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    char buf[64] = "hello";

    /* frame 1: build disabled textfield */
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .disabled = true},
                                                      .buf = buf,
                                                      .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* frame 2: send keys — should be ignored */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'X', .pressed = true};
    in.text_utf8[0] = 'X';
    in.text_utf8[1] = '\0';
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .disabled = true},
                                                          .buf = buf,
                                                          .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed == false);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    optics_ui_release(ui);
}

static void a11y_cb(const optics_ui_semantics *s, optics_gfx_rect bounds, optics_ui_id id,
                    optics_ui_id parent, void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    if (s->flags & OPTICS_UI_A11Y_DISABLED)
        *(bool *)user = true;
}

static void test_disabled_a11y_flag(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "disabled", .box = {.disabled = true}});
    optics_ui_test_end(ui);

    bool found = false;
    optics_ui_accessibility_walk(ui, a11y_cb, &found);

    OT_EXPECT(found == true);

    optics_ui_release(ui);
}

int main(void) {
    test_disabled_button_no_click();
    test_disabled_slider_no_change();
    test_disabled_checkbox_no_toggle();
    test_disabled_textfield_no_input();
    test_disabled_a11y_flag();
    return OT_REPORT();
}
