/* test_audit_fixes.c — regression coverage for the 2026-08 audit round:
 * dismissal ownership (C1), table column clamp (R1), modal `pinned`
 * (C2, see test_modal.c), central keyboard activation (R4), a11y roles
 * (C5), split-handle occlusion (R2), prev-band overflow (R3), key_count
 * clamp (R6), paste target/frame binding (R5), store reconciliation
 * (O5), and grace-window re-entry (O7). */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ---- C1: menu close-all must not take a pinned modal down ----------- */

static void test_close_all_spares_pinned_modal(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_place_open(ui, "pop");
    lens_modal_open(ui, "m");
    if (lens_modal_begin(ui, "m", (lens_modal_opts){.pinned = true}))
        lens_modal_end(ui);
    CHECK(lens_place_is_open(ui, "pop"));
    CHECK(lens_modal_is_open(ui, "m"));
    /* A menu item firing closes the menu's transients; the pinned modal is
     * not menu-owned and survives. */
    lens_menubar_close_all_open(ui);
    CHECK(!lens_place_is_open(ui, "pop"));
    CHECK(lens_modal_is_open(ui, "m"));
    lens_end(ui);

    lens_destroy(ui);
}

/* ---- R1: a table with more than 32 columns flags overflow ----------- */

static const char *many_cell(void *user, int row, int col) {
    (void)user;
    (void)row;
    (void)col;
    return "x";
}

static void test_table_columns_clamped_not_silent(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    static lens_table_column cols[40];
    for (int i = 0; i < 40; i++)
        cols[i] = (lens_table_column){.title = "c", .width = 30, .align = LENS_START};

    lens_begin(ui, &IN0);
    lens_size(ui, 400, 200);
    lens_table(ui, "wide", cols, 40, 3, many_cell, NULL, (lens_table_opts){.row_height = 20});
    lens_end(ui);
    CHECK(lens_overflowed(ui)); /* surfaced, never a silent OOB read */

    lens_destroy(ui);
}

/* ---- R4: central keyboard activation -------------------------------- */

static void test_return_activates_focused_button(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_set_focus(ui, lens_current_id(ui, "OK"));
    lens_end(ui);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, "OK");
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

static void test_space_toggles_focused_checkbox(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    bool on = false;

    lens_begin(ui, &IN0);
    lens_checkbox(ui, "Flag", &on);
    lens_set_focus(ui, lens_current_id(ui, "Flag"));
    lens_end(ui);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = ' ', .pressed = true};
    lens_begin(ui, &in);
    lens_checkbox(ui, "Flag", &on);
    lens_end(ui);
    CHECK(on == true);

    lens_destroy(ui);
}

/* ---- R4: menu arrow-key navigation ---------------------------------- */

static lens_id g_item_new, g_item_open;

static void build_bar(lens *ui, bool focus_new) {
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item(ui, "New", NULL)) {
        }
        g_item_new = lens_current_id(ui, "New");
        if (focus_new)
            lens_set_focus(ui, g_item_new);
        lens_menu_item(ui, "Open", NULL);
        g_item_open = lens_current_id(ui, "Open");
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
}

static void test_menu_arrow_nav_and_return_activation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* settle + open the menu (press then release the trigger) */
    lens_begin(ui, &IN0);
    build_bar(ui, false);
    lens_end(ui);
    lens_input in = IN0;
    in.cursor = (flux_point){20, 10};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_bar(ui, false);
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    build_bar(ui, false);
    lens_end(ui);

    /* focus "New" programmatically, then Down moves focus to "Open" */
    in = IN0;
    lens_begin(ui, &in);
    build_bar(ui, true);
    lens_end(ui);
    CHECK(lens_focused(ui, g_item_new));

    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
    lens_begin(ui, &in);
    build_bar(ui, false);
    lens_end(ui);
    CHECK(lens_focused(ui, g_item_open));

    /* Return activates the focused item: it fires and the stack closes. */
    in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    bool fired = false;
    lens_begin(ui, &in);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        if (lens_menu_item(ui, "New", NULL)) {
        }
        if (lens_menu_item(ui, "Open", NULL))
            fired = true;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(fired);

    int body_runs = 0;
    lens_begin(ui, &IN0);
    lens_menubar_begin(ui, "mb");
    if (lens_menu_begin(ui, "File")) {
        body_runs++;
        lens_menu_end(ui);
    }
    lens_menubar_end(ui);
    lens_end(ui);
    CHECK(body_runs == 0); /* menu closed after activation */

    lens_destroy(ui);
}

/* ---- C5: a11y roles -------------------------------------------------- */

typedef struct role_seen {
    bool progress, table, row, row_selected, menuitem, link;
} role_seen;

static void collect_roles(const lens_semantics *sem, flux_rect bounds, lens_id id, lens_id parent,
                          void *user) {
    (void)bounds;
    (void)id;
    (void)parent;
    role_seen *seen = user;
    if (sem->role == LENS_ROLE_PROGRESS)
        seen->progress = true;
    if (sem->role == LENS_ROLE_TABLE)
        seen->table = true;
    if (sem->role == LENS_ROLE_ROW) {
        seen->row = true;
        if (sem->flags & LENS_A11Y_SELECTED)
            seen->row_selected = true;
    }
    if (sem->role == LENS_ROLE_MENUITEM)
        seen->menuitem = true;
    if (sem->role == LENS_ROLE_LINK)
        seen->link = true;
}

static void test_roles_are_wired(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    role_seen seen;

    /* progress + link */
    lens_begin(ui, &IN0);
    lens_progress(ui, "load", 0.5f);
    lens_link(ui, "More");
    lens_end(ui);
    seen = (role_seen){0};
    lens_accessibility_walk(ui, collect_roles, &seen);
    CHECK(seen.progress);
    CHECK(seen.link);

    /* table: settle, click a row, settle — then walk the SAME frame set */
    static const lens_table_column cols[1] = {{.title = "T", .width = 0, .align = LENS_START}};
    lens_begin(ui, &IN0);
    lens_size(ui, 200, 100);
    lens_table(ui, "grid", cols, 1, 3, many_cell, NULL,
               (lens_table_opts){.row_height = 20, .selectable = true});
    lens_end(ui);

    lens_input in = IN0;
    in.cursor = (flux_point){20, 45};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_size(ui, 200, 100);
    lens_table(ui, "grid", cols, 1, 3, many_cell, NULL,
               (lens_table_opts){.row_height = 20, .selectable = true});
    lens_end(ui);

    lens_begin(ui, &IN0);
    lens_size(ui, 200, 100);
    lens_table(ui, "grid", cols, 1, 3, many_cell, NULL,
               (lens_table_opts){.row_height = 20, .selectable = true});
    lens_end(ui);
    seen = (role_seen){0};
    lens_accessibility_walk(ui, collect_roles, &seen);
    CHECK(seen.table);
    CHECK(seen.row);
    CHECK(seen.row_selected); /* the row clicked above carries SELECTED */

    /* menu item: an open context menu's row reports MENUITEM */
    lens_begin(ui, &IN0);
    lens_context_menu_open(ui, "ctx", (flux_rect){10, 10, 1, 1});
    if (lens_context_menu_begin(ui, "ctx")) {
        lens_menu_item(ui, "Copy", NULL);
        lens_context_menu_end(ui);
    }
    lens_end(ui);
    seen = (role_seen){0};
    lens_accessibility_walk(ui, collect_roles, &seen);
    CHECK(seen.menuitem);

    lens_destroy(ui);
}

/* ---- R2: split handle is occluded by a higher band ------------------- */

static void test_split_handle_occluded_by_popup(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_place_opts card = {
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_EXACT,
        .rect = {0, 0, 400, 300},
        .layout = {.bg = 0xFF202020u},
    };

    /* frame 1: settle both (the card covers the divider). */
    lens_begin(ui, &IN0);
    if (lens_split_begin(ui, "sp", LENS_SPLIT_VERTICAL, NULL)) {
        if (lens_split_pane(ui))
            lens_label(ui, "a");
        if (lens_split_pane(ui))
            lens_label(ui, "b");
        lens_split_end(ui);
    }
    if (lens_place_begin(ui, "card", card)) {
        lens_label(ui, "cover");
        lens_place_end(ui);
    }
    lens_end(ui);

    /* frame 2: press on the divider (~x=197), drag right next frame. The
     * card swallows the press: no capture, no ratio change. */
    lens_input in = IN0;
    in.cursor = (flux_point){197, 150};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    if (lens_split_begin(ui, "sp", LENS_SPLIT_VERTICAL, NULL)) {
        if (lens_split_pane(ui))
            lens_label(ui, "a");
        if (lens_split_pane(ui))
            lens_label(ui, "b");
        lens_split_end(ui);
    }
    if (lens_place_begin(ui, "card", card)) {
        lens_label(ui, "cover");
        lens_place_end(ui);
    }
    lens_end(ui);

    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.cursor = (flux_point){250, 150};
    lens_begin(ui, &in);
    if (lens_split_begin(ui, "sp", LENS_SPLIT_VERTICAL, NULL)) {
        if (lens_split_pane(ui))
            lens_label(ui, "a");
        if (lens_split_pane(ui))
            lens_label(ui, "b");
        lens_split_end(ui);
    }
    if (lens_place_begin(ui, "card", card)) {
        lens_label(ui, "cover");
        lens_place_end(ui);
    }
    lens_end(ui);

    float ratio = lens_split_ratio(ui, "sp");
    CHECK_NEAR(ratio, 0.5f, 0.001f);

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
        if (lens_place_begin(ui, "panel",
                             (lens_place_opts){.band = LENS_BAND_CHROME,
                                               .mode = LENS_PLACE_EXACT,
                                               .rect = {(float)(i * 20), 0, 10, 10}}))
            lens_place_end(ui);
        lens_pop_id(ui);
    }
    lens_end(ui);
    CHECK(!lens_overflowed(ui)); /* first frame: buckets are fine */

    lens_begin(ui, &IN0);
    lens_label(ui, "idle");
    lens_end(ui);
    CHECK(lens_overflowed(ui)); /* snapshot of 17 ids truncated + flagged */

    lens_destroy(ui);
}

/* ---- R6: key_count is clamped to the keys[] capacity ----------------- */

static void test_key_count_clamped(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_set_focus(ui, lens_current_id(ui, "OK"));
    lens_end(ui);

    /* A hostile count with the activating key past the array end must not
     * be read (and must not crash). */
    lens_input in = IN0;
    in.key_count = 64;
    in.keys[3] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, "OK");
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
    lens_textfield(ui, "fld", buf, cap);
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
    lens_button(ui, "Hold");
    lens_end(ui);
    lens_input in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, "Hold");
    lens_id btn = lens_current_id(ui, "Hold");
    lens_end(ui);
    CHECK(lens_active(ui) == btn);
    lens_set_focus(ui, btn);

    /* stop declaring the button; after the grace window the node reaps and
     * both capture and focus must release */
    for (int f = 0; f < 12; f++) {
        lens_begin(ui, &in); /* mouse still held down */
        lens_label(ui, "other");
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
        lens_button(ui, "Blink");
        lens_end(ui);
    }
    /* one frame absent (inside the grace window) */
    lens_begin(ui, &IN0);
    lens_label(ui, "gap");
    lens_end(ui);

    /* re-appears the same frame a press lands on its old rect: the stale
     * prev_rect must not make it pressable (first-frame rule). */
    lens_input in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool pressed = false;
    if (lens_button(ui, "Blink"))
        pressed = true;
    lens_end(ui);
    CHECK(!pressed);

    /* control: a frame later it is interactive again */
    lens_begin(ui, &IN0);
    lens_button(ui, "Blink");
    lens_end(ui);
    in = IN0;
    in.cursor = (flux_point){20, 16};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_button(ui, "Blink");
    lens_end(ui);
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    in.mouse_released[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    bool clicked = lens_button(ui, "Blink");
    lens_end(ui);
    CHECK(clicked);

    lens_destroy(ui);
}

/* ---- C6: empty-label ids unify --------------------------------------- */

static void test_empty_label_current_id_matches(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "");
    lens_end(ui);
    /* lens_current_id with an empty label must resolve the node the build
     * produced (the sentinel hash), not the raw scope id. */
    CHECK(lens_find(ui, lens_current_id(ui, "")) != NULL);

    lens_destroy(ui);
}

int main(void) {
    test_close_all_spares_pinned_modal();
    test_table_columns_clamped_not_silent();
    test_return_activates_focused_button();
    test_space_toggles_focused_checkbox();
    test_menu_arrow_nav_and_return_activation();
    test_roles_are_wired();
    test_split_handle_occluded_by_popup();
    test_band_overflow_is_flagged();
    test_key_count_clamped();
    test_paste_bound_to_requester();
    test_reap_reconciles_focus_and_capture();
    test_grace_reentry_not_interactive();
    test_empty_label_current_id_matches();
    return TEST_REPORT();
}
