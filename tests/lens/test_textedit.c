/* test_textedit.c — unified single-line and multi-line text input: paste, preedit,
 * committed text, cursor navigation, backspace, delete, multiline newlines,
 * cursor hints, and vertical centering. */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void focus_field(lens *ui, const char *label, char *buf, size_t cap) {
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = label}, .buf = buf, .cap = cap});
    lens_end(ui);
    lens_set_focus(ui, lens_current_id(ui, label));
}

static void focus_area(lens *ui, const char *label, char *buf, size_t cap) {
    lens_begin(ui, &IN0);
    lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = label}, .buf = buf, .cap = cap, .multiline = true});
    lens_end(ui);
    lens_set_focus(ui, lens_current_id(ui, label));
}

static void test_insert_ascii(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = {0};

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 6);

    lens_input in = IN0;
    strncpy(in.text_utf8, "hello", sizeof in.text_utf8 - 1);
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "hello") == 0);

    lens_destroy(ui);
}

static void test_backspace_and_delete(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = "abc";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 6);
    lens_textedit_set_caret(ui, "tf", 3);

    /* Backspace removes 'c' */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "ab") == 0);

    /* Move caret to 0 and Delete removes 'a' */
    lens_textedit_set_caret(ui, "tf", 0);
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DELETE, .pressed = true};
    lens_begin(ui, &in);
    changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "b") == 0);

    lens_destroy(ui);
}

static void test_cursor_navigation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = "world";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 6);
    lens_textedit_set_caret(ui, "tf", 5);

    /* Home moves to 0 */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);

    /* Insert 'a' at 0 */
    in.keys[0] = (lens_key_event){0};
    in.key_count = 0;
    strncpy(in.text_utf8, "a", sizeof in.text_utf8 - 1);
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "aworld") == 0);

    lens_destroy(ui);
}

static void test_multiline_enter_inserts_newline(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "line1";

    focus_area(ui, "ta", buf, sizeof buf);
    lens_textedit_set_caret(ui, "ta", 5);

    /* Press Enter in multiline */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui,
                      &(lens_textedit_opts){
                          .box = {.id = "ta"}, .buf = buf, .cap = sizeof buf, .multiline = true})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "line1\n") == 0);

    /* Add "line2" */
    in.keys[0] = (lens_key_event){0};
    in.key_count = 0;
    strncpy(in.text_utf8, "line2", sizeof in.text_utf8 - 1);
    lens_begin(ui, &in);
    changed =
        lens_textedit(ui,
                      &(lens_textedit_opts){
                          .box = {.id = "ta"}, .buf = buf, .cap = sizeof buf, .multiline = true})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "line1\nline2") == 0);

    lens_destroy(ui);
}

static void test_multiline_selection_and_copy(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "hello\nworld";

    focus_area(ui, "ta", buf, sizeof buf);
    lens_textedit_set_selection(ui, "ta", 0, 5); /* select "hello" */

    /* Backspace deletes selection */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui,
                      &(lens_textedit_opts){
                          .box = {.id = "ta"}, .buf = buf, .cap = sizeof buf, .multiline = true})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "\nworld") == 0);

    lens_destroy(ui);
}

static void test_textedit_cursor_hint(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = "search";

    /* Frame 1: render textedit at known position */
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    /* Frame 2: hover cursor over the textedit */
    lens_input in = IN0;
    in.cursor = (flux_point){50.0f, 18.0f};
    lens_begin(ui, &in);
    lens_response r = lens_textedit(
        ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 36.0f},
                                  .buf = buf,
                                  .cap = sizeof buf});
    lens_end(ui);

    CHECK(r.hovered);
    CHECK(lens_get_cursor_hint(ui) == LENS_CURSOR_TEXT);

    lens_destroy(ui);
}

static void test_textedit_vertical_centering(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = "centered";

    /* Render textedit with 40px height */
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 40.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    lens_node *n = lens_find(ui, lens_current_id(ui, "tf"));
    CHECK(n != NULL);

    /* Find text draw command and verify vertical centering */
    const lens_draw_cmd *text_cmd = NULL;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == LENS_DRAW_TEXT) {
            text_cmd = &n->cmds[i];
            break;
        }
    }
    CHECK(text_cmd != NULL);
    /* In a 40px box, text should be centered: (40 - fm.height) * 0.5f */
    CHECK(text_cmd->rel.y > 0.0f);
    CHECK(text_cmd->rel.y < 20.0f);

    lens_destroy(ui);
}

static void test_textedit_caret_coverage(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[32] = "/home/ming";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 6);

    /* Frame 2: render focused field to emit caret */
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);

    lens_node *n = lens_find(ui, lens_current_id(ui, "tf"));
    CHECK(n != NULL);

    /* Find caret draw command */
    const lens_draw_cmd *caret_cmd = NULL;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        /* Caret is drawn as LENS_DRAW_RECT with width 1.5f */
        if (n->cmds[i].kind == LENS_DRAW_RECT && n->cmds[i].rel.w == 1.5f) {
            caret_cmd = &n->cmds[i];
            break;
        }
    }
    CHECK(caret_cmd != NULL);
    if (caret_cmd) {
        /* Caret height must span full ascent + descent (>= 14.0f for standard 13px font) */
        CHECK(caret_cmd->rel.h >= 14.0f);
    }

    lens_destroy(ui);
}

static void test_textedit_ime_preedit_and_commit(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "hello ";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 6);

    /* Frame 1: IME sends preedit "nihao" */
    lens_input in = IN0;
    strncpy(in.preedit_utf8, "nihao", sizeof in.preedit_utf8 - 1);
    in.preedit_cursor = 5;
    in.preedit_sel_lo = 0;
    in.preedit_sel_hi = 5;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);

    lens_node *n = lens_find(ui, lens_current_id(ui, "tf"));
    CHECK(n != NULL);
    /* Node must have preedit underline draw command */
    bool found_preedit_underline = false;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == LENS_DRAW_RECT && n->cmds[i].rel.h == 1.5f &&
            n->cmds[i].rel.w > 0.0f) {
            found_preedit_underline = true;
            break;
        }
    }
    CHECK(found_preedit_underline);

    /* Frame 2: IME commits "你好" */
    in = IN0;
    strncpy(in.text_utf8, "你好", sizeof in.text_utf8 - 1);
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "hello 你好") == 0);

    lens_destroy(ui);
}

static void test_double_click_selects_word(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "hello world /path/test";

    /* Frame 1: layout text field at known position */
    lens_begin(ui, &IN0);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    /* Click 1: single click on "world" (around x = 55px, y = 18px) */
    lens_input in = IN0;
    in.cursor = (flux_point){55.0f, 18.0f};
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    /* Release mouse */
    in.mouse_pressed[LENS_MOUSE_LEFT] = false;
    in.mouse_down[LENS_MOUSE_LEFT] = false;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    /* Click 2 (double-click): click again at the same spot within 400ms */
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    uint32_t sel_lo = 0, sel_hi = 0;
    CHECK(lens_textedit_get_selection(ui, "tf", &sel_lo, &sel_hi));
    /* "world" is at index 6..11 */
    CHECK(sel_lo == 6);
    CHECK(sel_hi == 11);

    /* Click 3 (triple-click): click third time to select entire line */
    in.mouse_pressed[LENS_MOUSE_LEFT] = true;
    in.mouse_down[LENS_MOUSE_LEFT] = true;
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                            .buf = buf,
                                            .cap = sizeof buf});
    lens_end(ui);

    CHECK(lens_textedit_get_selection(ui, "tf", &sel_lo, &sel_hi));
    CHECK(sel_lo == 0);
    CHECK(sel_hi == (uint32_t)strlen(buf));

    lens_destroy(ui);
}

static void test_ctrl_arrow_word_navigation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "/home/ming/test";

    focus_field(ui, "tf", buf, sizeof buf);

    /* Start at end: cursor = 15 */
    lens_textedit_set_caret(ui, "tf", 15);
    uint32_t cur = 0;
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 15);

    /* Ctrl+Left moves to start of "test" (11) */
    lens_input in = IN0;
    in.mods = LENS_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 11);

    /* Ctrl+Left moves across '/' to start of "ming" (6) */
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 6);

    /* Ctrl+Left moves across '/' to start of "home" (1) */
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 1);

    /* Ctrl+Left moves to 0 */
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 0);

    /* Ctrl+Shift+Right selects "home" (0 to 5) */
    in.mods = LENS_MOD_CTRL | LENS_MOD_SHIFT;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RIGHT, .pressed = true};
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    uint32_t sel_lo = 0, sel_hi = 0;
    CHECK(lens_textedit_get_selection(ui, "tf", &sel_lo, &sel_hi));
    CHECK(sel_lo == 0);
    CHECK(sel_hi == 5);

    /* Shift+End selects to end */
    in.mods = LENS_MOD_SHIFT;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_selection(ui, "tf", &sel_lo, &sel_hi));
    CHECK(sel_lo == 0);
    CHECK(sel_hi == 15);

    /* Shift+Home selects back to 0 */
    in.mods = LENS_MOD_SHIFT;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(lens_textedit_get_caret(ui, "tf", &cur));
    CHECK(cur == 0);

    lens_destroy(ui);
}

int main(void) {
    test_insert_ascii();
    test_backspace_and_delete();
    test_cursor_navigation();
    test_multiline_enter_inserts_newline();
    test_multiline_selection_and_copy();
    test_textedit_cursor_hint();
    test_textedit_vertical_centering();
    test_textedit_caret_coverage();
    test_textedit_ime_preedit_and_commit();
    test_double_click_selects_word();
    test_ctrl_arrow_word_navigation();
    return TEST_REPORT();
}
