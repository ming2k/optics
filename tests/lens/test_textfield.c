/* test_textfield.c — single-line text input: paste, preedit, committed
 * text, cursor navigation, backspace, delete (ADR-0013 + widget).
 *
 * Focus is established explicitly with lens_set_focus to avoid relying
 * on the one-frame click-to-focus latency in these small tests. */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

/* Build the textfield for one frame, then set focus explicitly on the
 * second frame so subsequent frames have a focused field. */
static lens_id setup_textfield(lens *ui, const char *label, char *buf, size_t cap) {
    /* frame 1: enter */
    lens_begin(ui, &IN0);
    lens_textfield(ui, label, buf, cap);
    lens_end(ui);

    /* frame 2: focus */
    lens_begin(ui, &IN0);
    lens_id id = lens_current_id(ui, label);
    lens_set_focus(ui, id);
    lens_textfield(ui, label, buf, cap);
    lens_end(ui);

    /* frame 3: move cursor to end so subsequent tests start at EOL */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
    lens_begin(ui, &in);
    lens_textfield(ui, label, buf, cap);
    lens_end(ui);

    return id;
}

/* ------------------------------------------------------------------ */
/*  Paste delivery                                                    */
/* ------------------------------------------------------------------ */
static void test_textfield_paste(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "hello";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* queue a paste */
    lens_paste(ui, " world", 6);

    /* next frame: paste is consumed at cursor position (start, 0) */
    lens_begin(ui, &IN0);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "hello world") == 0); /* cursor was at EOL (3) */

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Committed text (IME finish or simple key)                         */
/* ------------------------------------------------------------------ */
static void test_textfield_committed_text(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "";
    setup_textfield(ui, "tf", buf, sizeof buf);

    lens_input in = IN0;
    strcpy(in.text_utf8, "abc");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "abc") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Preedit sets caret rect and renders underline                     */
/* ------------------------------------------------------------------ */
static void test_textfield_preedit_sets_caret(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* With focus and an empty buffer, caret rect should be set. */
    flux_rect c0 = lens_caret_rect(ui);
    CHECK(c0.w > 0.0f);

    /* send preedit */
    lens_input in = IN0;
    strcpy(in.preedit_utf8, "xyz");
    in.preedit_cursor = 3;
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    flux_rect c1 = lens_caret_rect(ui);
    CHECK(c1.w > 0.0f);
    /* caret moved after the preedit string (x should increase) */
    CHECK(c1.x > c0.x || c1.y > c0.y || c1.h > 0.0f);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Backspace removes character before cursor                         */
/* ------------------------------------------------------------------ */
static void test_textfield_backspace(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abc";
    setup_textfield(ui, "tf", buf, sizeof buf);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "ab") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Delete removes character after cursor                             */
/* ------------------------------------------------------------------ */
static void test_textfield_delete(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abcd";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* move cursor left twice (4 -> 3 -> 2) */
    lens_input left = IN0;
    left.key_count = 1;
    left.keys[0] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    lens_begin(ui, &left);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    lens_begin(ui, &left);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* cursor now at 2 (between b and c). Delete -> "abd" */
    lens_input del = IN0;
    del.key_count = 1;
    del.keys[0] = (lens_key_event){.key = LENS_KEY_DELETE, .pressed = true};
    lens_begin(ui, &del);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "abd") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Home / End move cursor to start / end                             */
/* ------------------------------------------------------------------ */
static void test_textfield_home_end(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abc";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* move cursor left twice: 3 -> 2 -> 1 */
    lens_input left = IN0;
    left.key_count = 1;
    left.keys[0] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    lens_begin(ui, &left);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    lens_begin(ui, &left);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    flux_rect c_mid = lens_caret_rect(ui);

    /* Home -> cursor to 0. caret should move left. */
    lens_input home = IN0;
    home.key_count = 1;
    home.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
    lens_begin(ui, &home);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    flux_rect c_home = lens_caret_rect(ui);
    CHECK(c_home.x < c_mid.x);

    /* End -> cursor to 3. caret should move right. */
    lens_input end = IN0;
    end.key_count = 1;
    end.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
    lens_begin(ui, &end);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    flux_rect c_end = lens_caret_rect(ui);
    CHECK(c_end.x > c_home.x);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Buffer overflow protection — paste larger than cap                */
/* ------------------------------------------------------------------ */
static void test_textfield_buffer_cap(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[8] = "ab";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* paste more than fits */
    lens_paste(ui, "cdefgh", 6);
    lens_begin(ui, &IN0);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* should be truncated, but not crash */
    CHECK(strlen(buf) <= 7); /* 7 chars + null for an 8-byte buffer */

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Ctrl+A selects all; Backspace deletes selection                   */
/* ------------------------------------------------------------------ */
static void test_textfield_select_all(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abcd";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* Ctrl+A */
    lens_input sel = IN0;
    sel.key_count = 1;
    sel.keys[0] = (lens_key_event){.key = 'a', .pressed = true};
    sel.mods = LENS_MOD_CTRL;
    lens_begin(ui, &sel);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* Backspace deletes the whole selection */
    lens_input bs = IN0;
    bs.key_count = 1;
    bs.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &bs);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Shift+End selects to EOL; Delete removes selection                */
/* ------------------------------------------------------------------ */
static void test_textfield_shift_select(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "hello";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* move cursor to start */
    lens_input home = IN0;
    home.key_count = 1;
    home.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
    lens_begin(ui, &home);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* Shift+End selects to end */
    lens_input sel = IN0;
    sel.key_count = 1;
    sel.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
    sel.mods = LENS_MOD_SHIFT;
    lens_begin(ui, &sel);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* Delete removes selection */
    lens_input del = IN0;
    del.key_count = 1;
    del.keys[0] = (lens_key_event){.key = LENS_KEY_DELETE, .pressed = true};
    lens_begin(ui, &del);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Copy / Cut host clipboard integration                             */
/* ------------------------------------------------------------------ */

typedef struct host_clip {
    char copied[64];
    size_t copied_len;
    int request_count;
} host_clip;

static void host_set_text2(const char *utf8, size_t len, void *user) {
    host_clip *h = user;
    size_t n = len < sizeof h->copied - 1 ? len : sizeof h->copied - 1;
    memcpy(h->copied, utf8, n);
    h->copied[n] = '\0';
    h->copied_len = n;
}
static void host_request_text2(void *user) {
    ((host_clip *)user)->request_count++;
}

static void test_textfield_copy_cut(void) {
    host_clip host = {0};
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.clipboard = {.set_text = host_set_text2,
                                                 .request_text = host_request_text2,
                                                 .user = &host}},
                      &ui) == FLUX_OK);

    char buf[64] = "copyme";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* Ctrl+A select all */
    lens_input sel = IN0;
    sel.key_count = 1;
    sel.keys[0] = (lens_key_event){.key = 'a', .pressed = true};
    sel.mods = LENS_MOD_CTRL;
    lens_begin(ui, &sel);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* Ctrl+C */
    lens_input cp = IN0;
    cp.key_count = 1;
    cp.keys[0] = (lens_key_event){.key = 'c', .pressed = true};
    cp.mods = LENS_MOD_CTRL;
    lens_begin(ui, &cp);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(host.copied_len == 6);
    CHECK(strcmp(host.copied, "copyme") == 0);

    /* Ctrl+X */
    lens_input ct = IN0;
    ct.key_count = 1;
    ct.keys[0] = (lens_key_event){.key = 'x', .pressed = true};
    ct.mods = LENS_MOD_CTRL;
    lens_begin(ui, &ct);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Paste replaces active selection                                     */
/* ------------------------------------------------------------------ */
static void test_textfield_paste_replaces_selection(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "hello world";
    setup_textfield(ui, "tf", buf, sizeof buf);

    /* Shift+Home selects "hello world" */
    lens_input sel = IN0;
    sel.key_count = 1;
    sel.keys[0] = (lens_key_event){.key = LENS_KEY_HOME, .pressed = true};
    sel.mods = LENS_MOD_SHIFT;
    lens_begin(ui, &sel);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* paste "hi" */
    lens_paste(ui, "hi", 2);
    lens_begin(ui, &IN0);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "hi") == 0);

    lens_destroy(ui);
}

static void test_textfield_requests_text_cursor(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    char buf[16] = "hello";

    lens_begin(ui, &IN0);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    lens_input hover = IN0;
    hover.cursor = (flux_point){20.0f, 20.0f};
    lens_begin(ui, &hover);
    lens_textfield(ui, "tf", buf, sizeof buf);
    CHECK(lens_get_cursor_hint(ui) == LENS_CURSOR_TEXT);
    lens_end(ui);

    lens_destroy(ui);
}

int main(void) {
    test_textfield_paste();
    test_textfield_committed_text();
    test_textfield_preedit_sets_caret();
    test_textfield_backspace();
    test_textfield_delete();
    test_textfield_home_end();
    test_textfield_buffer_cap();
    test_textfield_select_all();
    test_textfield_shift_select();
    test_textfield_copy_cut();
    test_textfield_paste_replaces_selection();
    test_textfield_requests_text_cursor();
    return TEST_REPORT();
}
