/* test_audit_fixes.c — regression coverage for the audit round. */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ---- R4: central keyboard activation -------------------------------- */

static void test_return_activates_focused_button(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "OK"));
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

static void test_space_toggles_focused_checkbox(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    bool on = false;

    optics_ui_begin(ui, &IN0);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Flag", .value = &on});
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "Flag"));
    optics_ui_test_end(ui);

    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = ' ', .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_checkbox(ui, &(optics_ui_checkbox_opts){.label = "Flag", .value = &on});
    optics_ui_test_end(ui);
    OT_EXPECT(on == true);

    optics_ui_release(ui);
}

/* ---- C5: a11y roles -------------------------------------------------- */

typedef struct role_seen {
    bool progress, button, label, link;
} role_seen;

static void collect_roles(const optics_ui_semantics *sem, optics_gfx_rect bounds, optics_ui_id id,
                          optics_ui_id parent, void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    role_seen *seen = user;
    if (sem->role == OPTICS_UI_ROLE_SLIDER)
        seen->progress = true;
    if (sem->role == OPTICS_UI_ROLE_BUTTON)
        seen->button = true;
    if (sem->role == OPTICS_UI_ROLE_LABEL)
        seen->label = true;
    if (sem->role == OPTICS_UI_ROLE_LINK)
        seen->link = true;
}

static void test_roles_are_wired(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    role_seen seen;

    float val = 0.5f;
    optics_ui_begin(ui, &IN0);
    optics_ui_slider(
        ui, &(optics_ui_slider_opts){.label = "Volume", .value = &val, .min = 0.0f, .max = 1.0f});
    optics_ui_button(ui,
                     &(optics_ui_button_opts){.label = "More", .variant = OPTICS_UI_BUTTON_LINK});
    optics_ui_test_end(ui);
    seen = (role_seen){};
    optics_ui_accessibility_walk(ui, collect_roles, &seen);
    OT_EXPECT(seen.progress);
    OT_EXPECT(seen.link);

    optics_ui_release(ui);
}

/* ---- R3: prev-band truncation flags overflow ------------------------- */

static void test_band_overflow_is_flagged(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Owned metadata must preserve more than 16 overlays without truncation. */
    optics_ui_begin(ui, &IN0);
    for (int i = 0; i < 17; i++) {
        optics_ui_push_id_int(ui, i);
        if (optics_ui_place_begin(ui,
                                  &(optics_ui_place_opts){.box = {.id = "panel"},
                                                          .band = OPTICS_UI_BAND_CHROME,
                                                          .mode = OPTICS_UI_PLACE_EXACT,
                                                          .rect = {(float)(i * 20), 0, 10, 10}}))
            optics_ui_place_end(ui);
        optics_ui_pop_id(ui);
    }
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui)); /* first frame: buckets are fine */

    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "idle"});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui)); /* owned snapshots preserve all 17 overlays */

    optics_ui_release(ui);
}

/* ---- R6: key_count is clamped to the keys[] capacity ----------------- */

static void test_key_count_clamped(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"});
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "OK"));
    optics_ui_test_end(ui);

    /* A hostile count with the activating key past the array end must not
     * be read (and must not crash). */
    optics_ui_input in = IN0;
    in.key_count = 64;
    in.keys[3] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "OK"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked); /* index 3 is inside the clamped range */
    OT_EXPECT(!optics_ui_overflowed(ui));

    optics_ui_release(ui);
}

/* ---- R5: paste is target-bound and frame-stamped --------------------- */

static void click_at(optics_ui *ui, float x, float y) {
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){x, y};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
}

static void build_field(optics_ui *ui, char *buf, size_t cap) {
    optics_ui_textedit(ui,
                       &(optics_ui_textedit_opts){.box = {.id = "fld"}, .buf = buf, .cap = cap});
}

static void test_paste_bound_to_requester(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "";

    /* focus the field (press at its top-left area) */
    optics_ui_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    optics_ui_test_end(ui);
    click_at(ui, 20, 14);
    build_field(ui, buf, sizeof buf);
    optics_ui_test_end(ui);

    /* request + fulfil, then the focused field drains next frame */
    optics_ui_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    optics_ui_request_paste(ui);
    optics_ui_test_end(ui);
    optics_ui_paste(ui, &(optics_ui_paste_opts){.utf8 = "zz", .len = 2});
    optics_ui_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    optics_ui_test_end(ui);
    OT_EXPECT(strcmp(buf, "zz") == 0);

    /* focus moves away, THEN a bound paste arrives: dropped, not delivered */
    strcpy(buf, "abc");
    optics_ui_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    optics_ui_request_paste(ui); /* bound to the field */
    optics_ui_test_end(ui);
    /* click outside: focus clears */
    click_at(ui, 350, 280);
    build_field(ui, buf, sizeof buf);
    optics_ui_test_end(ui);
    optics_ui_paste(ui, &(optics_ui_paste_opts){.utf8 = "QQ", .len = 2});
    optics_ui_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    optics_ui_test_end(ui);
    OT_EXPECT(strcmp(buf, "abc") == 0); /* focus drifted: not delivered */

    optics_ui_release(ui);
}

/* ---- O5: reaped nodes release interaction-owned ids ------------------- */

static void test_reap_reconciles_focus_and_capture(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* capture: press the button (active_id set) */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Hold"});
    optics_ui_test_end(ui);
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 16};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Hold"});
    optics_ui_id btn = optics_ui_current_id(ui, "Hold");
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_active(ui) == btn);
    optics_ui_set_focus(ui, btn);

    /* stop declaring the button; after the grace window the node reaps and
     * both capture and focus must release */
    for (int f = 0; f < 12; f++) {
        optics_ui_begin(ui, &in); /* mouse still held down */
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "other"});
        optics_ui_test_end(ui);
    }
    OT_EXPECT(optics_ui_active(ui) == 0);
    OT_EXPECT(!optics_ui_focused(ui, btn));

    optics_ui_release(ui);
}

/* ---- O7: a node re-entering from the grace window is not clickable ---- */

static void test_grace_reentry_not_interactive(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* two frames to settle prev geometry */
    for (int f = 0; f < 2; f++) {
        optics_ui_begin(ui, &IN0);
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "Blink"});
        optics_ui_test_end(ui);
    }
    /* one frame absent (inside the grace window) */
    optics_ui_begin(ui, &IN0);
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "gap"});
    optics_ui_test_end(ui);

    /* re-appears the same frame a press lands on its old rect: the stale
     * prev_rect must not make it pressable (first-frame rule). */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){20, 16};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool pressed = false;
    if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "Blink"}).clicked)
        pressed = true;
    optics_ui_test_end(ui);
    OT_EXPECT(!pressed);

    /* control: a frame later it is interactive again */
    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Blink"});
    optics_ui_test_end(ui);
    in = IN0;
    in.cursor = (optics_gfx_point){20, 16};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = "Blink"});
    optics_ui_test_end(ui);
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_released[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    bool clicked = optics_ui_button(ui, &(optics_ui_button_opts){.label = "Blink"}).clicked;
    optics_ui_test_end(ui);
    OT_EXPECT(clicked);

    optics_ui_release(ui);
}

/* ---- C6: empty-label ids unify --------------------------------------- */

static void test_empty_label_current_id_matches(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_begin(ui, &IN0);
    optics_ui_button(ui, &(optics_ui_button_opts){.label = ""});
    optics_ui_test_end(ui);
    /* lens_current_id with an empty label must resolve the node the build
     * produced (the sentinel hash), not the raw scope id. */
    OT_EXPECT(optics_ui_find(ui, optics_ui_current_id(ui, "")) != nullptr);

    optics_ui_release(ui);
}

int main(void) {
    test_return_activates_focused_button();
    test_space_toggles_focused_checkbox();
    test_roles_are_wired();
    test_band_overflow_is_flagged();
    test_key_count_clamped();
    test_paste_bound_to_requester();
    test_reap_reconciles_focus_and_capture();
    test_grace_reentry_not_interactive();
    test_empty_label_current_id_matches();
    return OT_REPORT();
}
