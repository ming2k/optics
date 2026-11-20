/* test_textedit.c — unified single-line and multi-line text input: paste, preedit,
 * committed text, cursor navigation, backspace, delete, multiline newlines,
 * cursor hints, and vertical centering. */

#include "../../libs/ui/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

static const optics_ui_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void focus_field(optics_ui *ui, const char *label, char *buf, size_t cap) {
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(ui,
                       &(optics_ui_textedit_opts){.box = {.id = label}, .buf = buf, .cap = cap});
    optics_ui_test_end(ui);
    optics_ui_set_focus(ui, optics_ui_current_id(ui, label));
}

static void focus_area(optics_ui *ui, const char *label, char *buf, size_t cap) {
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = label}, .buf = buf, .cap = cap, .multiline = true});
    optics_ui_test_end(ui);
    optics_ui_set_focus(ui, optics_ui_current_id(ui, label));
}

static void test_insert_ascii(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = {};

    focus_field(ui, "tf", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 6});

    optics_ui_input in = IN0;
    strncpy(in.text_utf8, "hello", sizeof in.text_utf8 - 1);
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    optics_ui_release(ui);
}

static void test_backspace_and_delete(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = "abc";

    focus_field(ui, "tf", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 6});
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 3});

    /* Backspace removes 'c' */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_BACKSPACE, .pressed = true};
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "ab") == 0);

    /* Move caret to 0 and Delete removes 'a' */
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 0});
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_DELETE, .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "b") == 0);

    optics_ui_release(ui);
}

static void test_cursor_navigation(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = "world";

    focus_field(ui, "tf", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 6});
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 5});

    /* Home moves to 0 */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_HOME, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* Insert 'a' at 0 */
    in.keys[0] = (optics_ui_key_event){};
    in.key_count = 0;
    strncpy(in.text_utf8, "a", sizeof in.text_utf8 - 1);
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "aworld") == 0);

    optics_ui_release(ui);
}

static void test_multiline_enter_inserts_newline(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "line1";

    focus_area(ui, "ta", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "ta", .caret = 5});

    /* Press Enter in multiline */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RETURN, .pressed = true};
    optics_ui_begin(ui, &in);
    bool changed = optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "ta"},
                                                                     .buf = buf,
                                                                     .cap = sizeof buf,
                                                                     .multiline = true})
                       .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "line1\n") == 0);

    /* Add "line2" */
    in.keys[0] = (optics_ui_key_event){};
    in.key_count = 0;
    strncpy(in.text_utf8, "line2", sizeof in.text_utf8 - 1);
    optics_ui_begin(ui, &in);
    changed = optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "ta"},
                                                                .buf = buf,
                                                                .cap = sizeof buf,
                                                                .multiline = true})
                  .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "line1\nline2") == 0);

    optics_ui_release(ui);
}

static void test_multiline_selection_and_copy(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "hello\nworld";

    focus_area(ui, "ta", buf, sizeof buf);
    optics_ui_textedit_set_selection(
        ui, &(optics_ui_textedit_set_selection_opts){
                .label = "ta", .sel_start = 0, .sel_end = 5}); /* select "hello" */

    /* Backspace deletes selection */
    optics_ui_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_BACKSPACE, .pressed = true};
    optics_ui_begin(ui, &in);
    bool changed = optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "ta"},
                                                                     .buf = buf,
                                                                     .cap = sizeof buf,
                                                                     .multiline = true})
                       .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "\nworld") == 0);

    optics_ui_release(ui);
}

static void test_textedit_cursor_hint(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = "search";

    /* Frame 1: render textedit at known position */
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* Frame 2: hover cursor over the textedit */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){50.0f, 18.0f};
    optics_ui_begin(ui, &in);
    optics_ui_response r = optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    OT_EXPECT(r.hovered);
    OT_EXPECT(optics_ui_get_cursor_hint(ui) == OPTICS_UI_CURSOR_TEXT);

    optics_ui_release(ui);
}

static void test_textedit_vertical_centering(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = "centered";

    /* Render textedit with 40px height */
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 200.0f, .height = 40.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, "tf"));
    OT_EXPECT(n != nullptr);

    /* Find text draw command and verify vertical centering */
    const optics_ui_draw_cmd *text_cmd = nullptr;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == OPTICS_UI_DRAW_TEXT) {
            text_cmd = &n->cmds[i];
            break;
        }
    }
    OT_EXPECT(text_cmd != nullptr);
    /* In a 40px box, text should be centered: (40 - fm.height) * 0.5f */
    OT_EXPECT(text_cmd->rel.y > 0.0f);
    OT_EXPECT(text_cmd->rel.y < 20.0f);

    optics_ui_release(ui);
}

static void test_textedit_caret_coverage(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[32] = "/home/ming";

    focus_field(ui, "tf", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 6});

    /* Frame 2: render focused field to emit caret */
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);

    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, "tf"));
    OT_EXPECT(n != nullptr);

    /* Find caret draw command */
    const optics_ui_draw_cmd *caret_cmd = nullptr;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        /* Caret is drawn as LENS_DRAW_RECT with width 1.5f */
        if (n->cmds[i].kind == OPTICS_UI_DRAW_RECT && n->cmds[i].rel.w == 1.5f) {
            caret_cmd = &n->cmds[i];
            break;
        }
    }
    OT_EXPECT(caret_cmd != nullptr);
    if (caret_cmd) {
        /* Caret height must span full ascent + descent (>= 14.0f for standard 13px font) */
        OT_EXPECT(caret_cmd->rel.h >= 14.0f);
    }

    optics_ui_release(ui);
}

static void test_textedit_ime_preedit_and_commit(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "hello ";

    focus_field(ui, "tf", buf, sizeof buf);
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 6});

    /* Frame 1: IME sends preedit "nihao" */
    optics_ui_input in = IN0;
    strncpy(in.preedit_utf8, "nihao", sizeof in.preedit_utf8 - 1);
    in.preedit_cursor = 5;
    in.preedit_sel_lo = 0;
    in.preedit_sel_hi = 5;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);

    optics_ui_node *n = optics_ui_find(ui, optics_ui_current_id(ui, "tf"));
    OT_EXPECT(n != nullptr);
    /* Node must have preedit underline draw command */
    bool found_preedit_underline = false;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        if (n->cmds[i].kind == OPTICS_UI_DRAW_RECT && n->cmds[i].rel.h == 1.5f &&
            n->cmds[i].rel.w > 0.0f) {
            found_preedit_underline = true;
            break;
        }
    }
    OT_EXPECT(found_preedit_underline);

    /* Frame 2: IME commits "你好" */
    in = IN0;
    strncpy(in.text_utf8, "你好", sizeof in.text_utf8 - 1);
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);

    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello 你好") == 0);

    optics_ui_release(ui);
}

static void test_double_click_selects_word(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "hello world /path/test";

    /* Frame 1: layout text field at known position */
    optics_ui_begin(ui, &IN0);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* Click 1: single click on "world" (around x = 55px, y = 18px) */
    optics_ui_input in = IN0;
    in.cursor = (optics_gfx_point){55.0f, 18.0f};
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* Release mouse */
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = false;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = false;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* Click 2 (double-click): click again at the same spot within 400ms */
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    uint32_t sel_lo = 0, sel_hi = 0;
    OT_EXPECT(optics_ui_textedit_get_selection(
        ui, &(optics_ui_textedit_get_selection_opts){
                .label = "tf", .out_sel_start = &sel_lo, .out_sel_end = &sel_hi}));
    /* "world" is at index 6..11 */
    OT_EXPECT(sel_lo == 6);
    OT_EXPECT(sel_hi == 11);

    /* Click 3 (triple-click): click third time to select entire line */
    in.mouse_pressed[OPTICS_UI_MOUSE_LEFT] = true;
    in.mouse_down[OPTICS_UI_MOUSE_LEFT] = true;
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf", .width = 300.0f, .height = 36.0f},
                                       .buf = buf,
                                       .cap = sizeof buf});
    optics_ui_test_end(ui);

    OT_EXPECT(optics_ui_textedit_get_selection(
        ui, &(optics_ui_textedit_get_selection_opts){
                .label = "tf", .out_sel_start = &sel_lo, .out_sel_end = &sel_hi}));
    OT_EXPECT(sel_lo == 0);
    OT_EXPECT(sel_hi == (uint32_t)strlen(buf));

    optics_ui_release(ui);
}

static void test_ctrl_arrow_word_navigation(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = "/home/ming/test";

    focus_field(ui, "tf", buf, sizeof buf);

    /* Start at end: cursor = 15 */
    optics_ui_textedit_set_caret(ui,
                                 &(optics_ui_textedit_set_caret_opts){.label = "tf", .caret = 15});
    uint32_t cur = 0;
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 15);

    /* Ctrl+Left moves to start of "test" (11) */
    optics_ui_input in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_LEFT, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 11);

    /* Ctrl+Left moves across '/' to start of "ming" (6) */
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 6);

    /* Ctrl+Left moves across '/' to start of "home" (1) */
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 1);

    /* Ctrl+Left moves to 0 */
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 0);

    /* Ctrl+Shift+Right selects "home" (0 to 5) */
    in.mods = OPTICS_UI_MOD_CTRL | OPTICS_UI_MOD_SHIFT;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_RIGHT, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    uint32_t sel_lo = 0, sel_hi = 0;
    OT_EXPECT(optics_ui_textedit_get_selection(
        ui, &(optics_ui_textedit_get_selection_opts){
                .label = "tf", .out_sel_start = &sel_lo, .out_sel_end = &sel_hi}));
    OT_EXPECT(sel_lo == 0);
    OT_EXPECT(sel_hi == 5);

    /* Shift+End selects to end */
    in.mods = OPTICS_UI_MOD_SHIFT;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_END, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_selection(
        ui, &(optics_ui_textedit_get_selection_opts){
                .label = "tf", .out_sel_start = &sel_lo, .out_sel_end = &sel_hi}));
    OT_EXPECT(sel_lo == 0);
    OT_EXPECT(sel_hi == 15);

    /* Shift+Home selects back to 0 */
    in.mods = OPTICS_UI_MOD_SHIFT;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_HOME, .pressed = true};
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    OT_EXPECT(optics_ui_textedit_get_caret(
        ui, &(optics_ui_textedit_get_caret_opts){.label = "tf", .out_caret = &cur}));
    OT_EXPECT(cur == 0);

    optics_ui_release(ui);
}

static void test_textedit_undo_redo(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    char buf[64] = {};

    focus_field(ui, "tf", buf, sizeof buf);

    /* 1. Type "hello" */
    optics_ui_input in = IN0;
    strncpy(in.text_utf8, "hello", sizeof in.text_utf8 - 1);
    optics_ui_begin(ui, &in);
    bool changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    /* 2. Undo with Ctrl+Z */
    in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'z', .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "") == 0);

    /* 3. Redo with Ctrl+Y */
    in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'y', .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    /* 4. Undo with Ctrl+Z again */
    in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'z', .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "") == 0);

    /* 5. Redo with Ctrl+Shift+Z */
    in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL | OPTICS_UI_MOD_SHIFT;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'z', .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    /* 6. Backspace removes 'o' */
    in = IN0;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_BACKSPACE, .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hell") == 0);

    /* Undo backspace restores 'hello' */
    in = IN0;
    in.mods = OPTICS_UI_MOD_CTRL;
    in.key_count = 1;
    in.keys[0] = (optics_ui_key_event){.key = 'z', .pressed = true};
    optics_ui_begin(ui, &in);
    changed =
        optics_ui_textedit(
            ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    optics_ui_test_end(ui);
    OT_EXPECT(changed);
    OT_EXPECT(strcmp(buf, "hello") == 0);

    optics_ui_release(ui);
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
    test_textedit_undo_redo();
    return OT_REPORT();
}
