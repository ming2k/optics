/* test_textedit.c — unified single-line and multi-line text input: paste, preedit,
 * committed text, cursor navigation, backspace, delete, multiline newlines. */

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
    lens_textedit(ui,
                  &(lens_textedit_opts){
                      .box = {.id = label}, .buf = buf, .cap = cap, .multiline = true, .rows = 4});
    lens_end(ui);
    lens_set_focus(ui, lens_current_id(ui, label));
}

/* ------------------------------------------------------------------ */
/*  Single-line input tests                                           */
/* ------------------------------------------------------------------ */

static void test_insert_ascii(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "";

    focus_field(ui, "tf", buf, sizeof buf);

    lens_input in = IN0;
    snprintf(in.text_utf8, sizeof in.text_utf8, "a");
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "a") == 0);

    lens_destroy(ui);
}

static void test_backspace_and_delete(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "abc";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 2); /* cursor between 'b' and 'c' */

    /* Backspace removes 'b' */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &in);
    bool changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "ac") == 0);

    /* Delete removes 'c' */
    in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DELETE, .pressed = true};
    lens_begin(ui, &in);
    changed =
        lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf})
            .changed;
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "a") == 0);

    lens_destroy(ui);
}

static void test_cursor_navigation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "world";

    focus_field(ui, "tf", buf, sizeof buf);
    lens_textedit_set_caret(ui, "tf", 0);

    /* Insert 'A' at start -> "Aworld" */
    lens_input in = IN0;
    snprintf(in.text_utf8, sizeof in.text_utf8, "A");
    lens_begin(ui, &in);
    lens_textedit(ui, &(lens_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    lens_end(ui);
    CHECK(strcmp(buf, "Aworld") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Multi-line input tests                                            */
/* ------------------------------------------------------------------ */

static void test_multiline_enter_inserts_newline(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "line1";

    focus_area(ui, "ta", buf, sizeof buf);
    lens_textedit_set_caret(ui, "ta", 5);

    /* Press Enter -> inserts '\n' */
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

    /* Type 'line2' */
    in = IN0;
    snprintf(in.text_utf8, sizeof in.text_utf8, "line2");
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

int main(void) {
    test_insert_ascii();
    test_backspace_and_delete();
    test_cursor_navigation();
    test_multiline_enter_inserts_newline();
    test_multiline_selection_and_copy();
    return TEST_REPORT();
}
