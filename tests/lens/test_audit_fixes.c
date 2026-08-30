/* test_audit_fixes.c — regression coverage for the audit round. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ---- R4: central keyboard activation -------------------------------- */

static void test_return_activates_focused_button(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_set_focus(ui, lens_current_id(ui, "OK"));
    lens_end(ui);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

static void test_space_toggles_focused_checkbox(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    bool on = false;

    lens_begin(ui, &IN0);
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "Flag", .value = &on});
    lens_set_focus(ui, lens_current_id(ui, "Flag"));
    lens_end(ui);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = ' ', .pressed = true};
    lens_begin(ui, &in);
    lens_checkbox(ui, &(lens_checkbox_opts){.label = "Flag", .value = &on});
    lens_end(ui);
    CHECK(on == true);

    lens_destroy(ui);
}

/* ---- C5: a11y roles -------------------------------------------------- */

typedef struct role_seen {
    bool progress, button, label, link;
} role_seen;

static void collect_roles(const lens_semantics *sem, flux_rect bounds, lens_id id, lens_id parent,
                          void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    role_seen *seen = user;
    if (sem->role == LENS_ROLE_SLIDER)
        seen->progress = true;
    if (sem->role == LENS_ROLE_BUTTON)
        seen->button = true;
    if (sem->role == LENS_ROLE_LABEL)
        seen->label = true;
    if (sem->role == LENS_ROLE_LINK)
        seen->link = true;
}

static void test_roles_are_wired(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    role_seen seen;

    float val = 0.5f;
    lens_begin(ui, &IN0);
    lens_slider(ui,
                &(lens_slider_opts){.label = "Volume", .value = &val, .min = 0.0f, .max = 1.0f});
    lens_button(ui, &(lens_button_opts){.label = "More", .variant = LENS_BUTTON_LINK});
    lens_end(ui);
    seen = (role_seen){0};
    lens_accessibility_walk(ui, collect_roles, &seen);
    CHECK(seen.progress);
    CHECK(seen.link);

    lens_destroy(ui);
}

/* ---- R3: prev-band truncation flags overflow ------------------------- */

static void test_band_overflow_is_flagged(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* 17 CHROME nodes exceed LENSI_BAND_PREV_MAX (16): the next frame's
     * prev-band snapshot must flag the truncation. */
    lens_begin(ui, &IN0);
    for (int i = 0; i < 17; i++) {
        lens_push_id_int(ui, i);
        if (lens_place_begin(ui, &(lens_place_opts){.box = {.id = "panel"},
                                                    .band = LENS_BAND_CHROME,
                                                    .mode = LENS_PLACE_EXACT,
                                                    .rect = {(float)(i * 20), 0, 10, 10}}))
            lens_place_end(ui);
        lens_pop_id(ui);
    }
    lens_end(ui);
    CHECK(!lens_overflowed(ui)); /* first frame: buckets are fine */

    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "idle"});
    lens_end(ui);
    CHECK(lens_overflowed(ui)); /* snapshot of 17 ids truncated + flagged */

    lens_destroy(ui);
}

/* ---- R6: key_count is clamped to the keys[] capacity ----------------- */

static void test_key_count_clamped(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_set_focus(ui, lens_current_id(ui, "OK"));
    lens_end(ui);

    /* A hostile count with the activating key past the array end must not
     * be read (and must not crash). */
    lens_input in = IN0;
    in.key_count = 64;
    in.keys[3] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, &(lens_button_opts){.label = "OK"}).clicked;
    lens_end(ui);
    CHECK(clicked); /* index 3 is inside the clamped range */
    CHECK(!lens_overflowed(ui));

    lens_destroy(ui);
}

/* ---- R5: paste is target-bound and frame-stamped --------------------- */

static void click_at(lens *ui, float x, float y) {
    lens_input in = IN0;
    in.cursor = (flux_point){x, y};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
}

static void build_field(lens *ui, char *buf, size_t cap) {
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "fld"}, .buf = buf, .cap = cap});
}

static void test_paste_bound_to_requester(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "";

    /* focus the field (press at its top-left area) */
    lens_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    lens_end(ui);
    click_at(ui, 20, 14);
    build_field(ui, buf, sizeof buf);
    lens_end(ui);

    /* request + fulfil, then the focused field drains next frame */
    lens_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    lens_request_paste(ui);
    lens_end(ui);
    lens_paste(ui, "zz", 2);
    lens_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    lens_end(ui);
    CHECK(strcmp(buf, "zz") == 0);

    /* focus moves away, THEN a bound paste arrives: dropped, not delivered */
    strcpy(buf, "abc");
    lens_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    lens_request_paste(ui); /* bound to the field */
    lens_end(ui);
    /* click outside: focus clears */
    click_at(ui, 350, 280);
    build_field(ui, buf, sizeof buf);
    lens_end(ui);
    lens_paste(ui, "QQ", 2);
    lens_begin(ui, &IN0);
    build_field(ui, buf, sizeof buf);
    lens_end(ui);
    CHECK(strcmp(buf, "abc") == 0); /* focus drifted: not delivered */

    lens_destroy(ui);
}

/* ---- O5: reaped nodes release interaction-owned ids ------------------- */

static void test_reap_reconciles_focus_and_capture(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* capture: press the button (active_id set) */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "Hold"});
    lens_end(ui);
    lens_input in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "Hold"});
    lens_id btn = lens_current_id(ui, "Hold");
    lens_end(ui);
    CHECK(lens_active(ui) == btn);
    lens_set_focus(ui, btn);

    /* stop declaring the button; after the grace window the node reaps and
     * both capture and focus must release */
    for (int f = 0; f < 12; f++) {
        lens_begin(ui, &in); /* mouse still held down */
        lens_label(ui, &(lens_label_opts){.text = "other"});
        lens_end(ui);
    }
    CHECK(lens_active(ui) == 0);
    CHECK(!lens_focused(ui, btn));

    lens_destroy(ui);
}

/* ---- O7: a node re-entering from the grace window is not clickable ---- */

static void test_grace_reentry_not_interactive(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* two frames to settle prev geometry */
    for (int f = 0; f < 2; f++) {
        lens_begin(ui, &IN0);
        lens_button(ui, &(lens_button_opts){.label = "Blink"});
        lens_end(ui);
    }
    /* one frame absent (inside the grace window) */
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "gap"});
    lens_end(ui);

    /* re-appears the same frame a press lands on its old rect: the stale
     * prev_rect must not make it pressable (first-frame rule). */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool pressed = false;
    if (lens_button(ui, &(lens_button_opts){.label = "Blink"}).clicked)
        pressed = true;
    lens_end(ui);
    CHECK(!pressed);

    /* control: a frame later it is interactive again */
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "Blink"});
    lens_end(ui);
    in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, &(lens_button_opts){.label = "Blink"});
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, &(lens_button_opts){.label = "Blink"}).clicked;
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

/* ---- C6: empty-label ids unify --------------------------------------- */

static void test_empty_label_current_id_matches(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = ""});
    lens_end(ui);
    /* lens_current_id with an empty label must resolve the node the build
     * produced (the sentinel hash), not the raw scope id. */
    CHECK(lens_find(ui, lens_current_id(ui, "")) != NULL);

    lens_destroy(ui);
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
    return TEST_REPORT();
}
