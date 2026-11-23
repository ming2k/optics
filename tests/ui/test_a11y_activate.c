/* test_a11y_activate.c — AT activation seam (ADR-0062).
 *
 * optics_ui_a11y_activate records a pending id; the next frame's optics_ui__interact
 * reports clicked for that node (focusable + not disabled), moves focus,
 * and the request is single-shot. Pointer occlusion does not block it;
 * disabled does.
 */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Helper: one frame with a single button; returns the button's id and
 * whether it reported clicked. */
static optics_ui_id build_button_frame(optics_ui *ui, bool *clicked) {
    optics_ui_begin(ui, &IN0);
    *clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_id id = optics_ui_get_response(ui).id;
    optics_ui_test_end(ui);
    return id;
}

/* Activation fires on the next frame, moves focus, and is single-shot. */
static void test_activate_fires_and_focuses(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool clicked = false;
    optics_ui_id id = build_button_frame(ui, &clicked);
    OT_EXPECT(id != 0);
    OT_EXPECT(!clicked);
    OT_EXPECT(!optics_ui_focused(ui, id));

    /* The request lands between frames (the AT-SPI pump runs after
     * optics_ui_end); the upcoming build must report the click. */
    optics_ui_a11y_activate(ui, id);
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(clicked);
    OT_EXPECT(optics_ui_focused(ui, id));

    /* Single-shot: the following frames see no further clicks. */
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(!clicked);
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(!clicked);

    optics_ui_release(ui);
}

/* A request for a nonexistent id is dropped at frame end and never fires. */
static void test_activate_unknown_id_dropped(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool clicked = false;
    optics_ui_id id = build_button_frame(ui, &clicked);
    (void)id;

    optics_ui_a11y_activate(ui, 0xDEADBEEF);
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(!clicked);
    /* …and does not linger into a later frame either. */
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(!clicked);

    optics_ui_release(ui);
}

/* A newer request replaces an unconsumed older one (single pending slot). */
static void test_activate_replaces_pending(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool clicked = false;
    optics_ui_id id = build_button_frame(ui, &clicked);

    optics_ui_a11y_activate(ui, 0xDEADBEEF); /* stale, then replaced */
    optics_ui_a11y_activate(ui, id);
    clicked = false;
    build_button_frame(ui, &clicked);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

/* Disabled widgets do not activate, and the request is consumed/dropped so
 * it cannot fire later if the widget becomes enabled again mid-frame. */
static void test_activate_disabled_blocked(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    bool disabled = true;
    optics_ui_id id = 0;
    bool clicked = false;

    /* frame 1: disabled button */
    optics_ui_begin(ui, &IN0);
    clicked =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.disabled = disabled}, .label = "No"})
            .clicked;
    id = optics_ui_get_response(ui).id;
    optics_ui_test_end(ui);
    OT_EXPECT(id != 0);
    OT_EXPECT(!clicked);

    /* activation request → blocked by disabled; no focus move */
    optics_ui_a11y_activate(ui, id);
    optics_ui_begin(ui, &IN0);
    clicked =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.disabled = disabled}, .label = "No"})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);
    OT_EXPECT(!optics_ui_focused(ui, id));

    /* enabled again: the dropped request must NOT fire retroactively */
    disabled = false;
    optics_ui_begin(ui, &IN0);
    clicked =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.disabled = disabled}, .label = "No"})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* fresh request now fires */
    optics_ui_a11y_activate(ui, id);
    optics_ui_begin(ui, &IN0);
    clicked =
        optics_ui_button(ui, &(optics_ui_button_opts){.box = {.disabled = disabled}, .label = "No"})
            .clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);
    OT_EXPECT(optics_ui_focused(ui, id));

    optics_ui_release(ui);
}

/* Occlusion does not block AT activation: a transient popup over the
 * button swallows its pointer interaction but not the a11y request. */
static void test_activate_bypasses_occlusion(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* frame 1: button under an open full-window popup (POPUP band
     * occludes; a BACKDROP would be hit-transparent) */
    bool clicked = false;
    optics_ui_begin(ui, &IN0);
    clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_id id = optics_ui_get_response(ui).id;
    optics_ui_place_open(ui, "cover");
    if (optics_ui_place_begin(ui, &(optics_ui_place_opts){.box = {.id = "cover"},
                                                          .band = OPTICS_UI_BAND_POPUP,
                                                          .mode = OPTICS_UI_PLACE_EXACT,
                                                          .rect = {0, 0, 400, 200},
                                                          .transient = true})) {
        optics_ui_size(ui, 400, 200);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "cover"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);

    /* frame 2: pointer press over the covered button — occluded, and the
     * press is inside the popup so it does not even dismiss it */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 20};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    bool pressed = optics_ui_get_response(ui).pressed;
    if (optics_ui_place_begin(ui, &(optics_ui_place_opts){.box = {.id = "cover"},
                                                          .band = OPTICS_UI_BAND_POPUP,
                                                          .mode = OPTICS_UI_PLACE_EXACT,
                                                          .rect = {0, 0, 400, 200},
                                                          .transient = true})) {
        optics_ui_size(ui, 400, 200);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "cover"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(!clicked);
    OT_EXPECT(!pressed);

    /* frame 3: AT activation fires anyway */
    optics_ui_a11y_activate(ui, id);
    optics_ui_begin(ui, &IN0);
    clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    if (optics_ui_place_begin(ui, &(optics_ui_place_opts){.box = {.id = "cover"},
                                                          .band = OPTICS_UI_BAND_POPUP,
                                                          .mode = OPTICS_UI_PLACE_EXACT,
                                                          .rect = {0, 0, 400, 200},
                                                          .transient = true})) {
        optics_ui_size(ui, 400, 200);
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "cover"});
        optics_ui_place_end(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

int main(void) {
    test_activate_fires_and_focuses();
    test_activate_unknown_id_dropped();
    test_activate_replaces_pending();
    test_activate_disabled_blocked();
    test_activate_bypasses_occlusion();
    return OT_REPORT();
}
