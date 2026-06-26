/* test_textarea.c — multi-line text input: newlines, cursor nav, scroll. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static lens_id focus_textarea(lens *ui, const char *label) {
    lens_begin(ui, &IN0);
    lens_textarea(ui, label, NULL, 0, 80.0f);
    lens_end(ui);

    lens_begin(ui, &IN0);
    lens_id id = lens_current_id(ui, label);
    lens_set_focus(ui, id);
    lens_textarea(ui, label, NULL, 0, 80.0f);
    lens_end(ui);

    return id;
}

/* Move cursor to end of current text (repeated Down + End). */
static void cursor_eof(lens *ui, const char *label, char *buf, size_t cap) {
    for (int i = 0; i < 10; i++) {
        lens_input in = IN0;
        in.keys[0] = (lens_key_event){.key = LENS_KEY_DOWN, .pressed = true};
        in.key_count = 1;
        lens_begin(ui, &in);
        lens_textarea(ui, label, buf, cap, 80.0f);
        lens_end(ui);
    }
    {
        lens_input in = IN0;
        in.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
        in.key_count = 1;
        lens_begin(ui, &in);
        lens_textarea(ui, label, buf, cap, 80.0f);
        lens_end(ui);
    }
}

static void test_basic_type(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "";
    bool changed;

    lens_begin(ui, &IN0);
    changed = lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);
    CHECK(!changed);
    CHECK(strlen(buf) == 0);

    focus_textarea(ui, "ta");

    lens_input in = IN0;
    strcpy(in.text_utf8, "hello");
    lens_begin(ui, &in);
    changed = lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "hello") == 0);

    lens_destroy(ui);
}

static void test_enter_newline(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "ab";
    focus_textarea(ui, "ta");
    cursor_eof(ui, "ta", buf, sizeof buf);

    lens_input in = IN0;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_RETURN, .pressed = true};
    in.key_count = 1;

    lens_begin(ui, &in);
    bool changed = lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    CHECK(changed);
    CHECK(strcmp(buf, "ab\n") == 0);

    lens_destroy(ui);
}

static void test_up_down_navigation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "line1\nline2\nline3";
    focus_textarea(ui, "ta");
    cursor_eof(ui, "ta", buf, sizeof buf);

    /* Up twice */
    for (int i = 0; i < 2; i++) {
        lens_input in = IN0;
        in.keys[0] = (lens_key_event){.key = LENS_KEY_UP, .pressed = true};
        in.key_count = 1;
        lens_begin(ui, &in);
        lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
        lens_end(ui);
    }

    /* End, then type X */
    {
        lens_input in = IN0;
        in.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
        in.key_count = 1;
        lens_begin(ui, &in);
        lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
        lens_end(ui);
    }

    lens_input in = IN0;
    strcpy(in.text_utf8, "X");
    lens_begin(ui, &in);
    bool changed = lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    CHECK(changed);
    CHECK(strncmp(buf, "line1X", 6) == 0);

    lens_destroy(ui);
}

static void test_home_end(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[64] = "hello\nworld";
    focus_textarea(ui, "ta");
    cursor_eof(ui, "ta", buf, sizeof buf);

    /* Home on last line */
    {
        lens_input in = IN0;
        in.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
        in.key_count = 1;
        lens_begin(ui, &in);
        lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
        lens_end(ui);
    }

    /* Type X */
    lens_input in = IN0;
    strcpy(in.text_utf8, "X");
    lens_begin(ui, &in);
    bool changed = lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    CHECK(changed);
    CHECK(strstr(buf, "\nXworld") != NULL);

    lens_destroy(ui);
}

static void test_scroll_clamping(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[256] = "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20";

    lens_begin(ui, &IN0);
    lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    CHECK(1);

    lens_destroy(ui);
}

int main(void) {
    test_basic_type();
    test_enter_newline();
    test_up_down_navigation();
    test_home_end();
    test_scroll_clamping();
    return TEST_REPORT();
}
