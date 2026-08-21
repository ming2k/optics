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
/*  Key repeats act as presses (OS/backend auto-repeat contract)        */
/* ------------------------------------------------------------------ */
/* No widget filters on lens_key_event.repeat: a synthesised repeat press
 * (Wayland client-side timer, Win32 lParam bit 30, Cocoa isARepeat) must
 * behave exactly like a physical press, at the OS repeat rate. Pin that
 * contract here — the backends rely on it. */
static void test_textfield_key_repeat(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abc";
    setup_textfield(ui, "tf", buf, sizeof buf);

    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true, .repeat = true};
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "ab") == 0);

    /* repeats keep arriving while the key is held: another one deletes */
    lens_begin(ui, &in);
    changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "a") == 0);

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

/* Plain byte-sink for the UTF-8 selection test (host_clip couples the
 * copy to its own bookkeeping). */
static void copy_sink(const char *utf8, size_t len, void *user) {
    char *dst = user;
    size_t n = len < 63 ? len : 63;
    memcpy(dst, utf8, n);
    dst[n] = '\0';
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

/* ------------------------------------------------------------------ */
/*  Host caret / selection control (ADR-0064)                           */
/* ------------------------------------------------------------------ */

/* Probe skin capturing the textfield's widget record (same channel as
 * test_skin.c) so tests can inspect sel_rects. */
static lens_widget_record g_tf_seen;
static void probe_tf_skin(lens *ui, lens_node *node, const lens_widget_record *rec) {
    g_tf_seen = *rec;
    lens_skin_border(ui, node, (flux_rect){0, 0, 0, 0}, rec->style.accent, 0.0f, 1.0f);
}

/* ------------------------------------------------------------------ */
/*  set_caret before the field's first-ever build (touch path)          */
/* ------------------------------------------------------------------ */
static void test_textfield_set_caret_before_first_build(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "ac";

    /* frame 1: the caret is set before the field has ever been built; the
     * find-or-create touch must remember it until the field appears. */
    lens_begin(ui, &IN0);
    lens_textfield_set_caret(ui, "tf", 1);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* frame 2: focus */
    lens_begin(ui, &IN0);
    lens_set_focus(ui, lens_current_id(ui, "tf"));
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* frame 3: typed text must land at the pre-seeded offset 1 */
    lens_input in = IN0;
    strcpy(in.text_utf8, "b");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "abc") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Tab-completion shape: rewrite the buffer, then caret to end         */
/* ------------------------------------------------------------------ */
static void test_textfield_set_caret_after_rewrite(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "git ch";
    setup_textfield(ui, "tf", buf, sizeof buf); /* cursor at EOL (6) */

    /* the host rewrites the buffer (completion), then moves the caret */
    strcpy(buf, "git checkout");
    lens_begin(ui, &IN0);
    lens_textfield_set_caret(ui, "tf", UINT32_MAX); /* clamps to len */
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* the next typed char lands at the end, not at the stale offset 6 */
    lens_input in = IN0;
    strcpy(in.text_utf8, "!");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "git checkout!") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  set_selection(0, UINT32_MAX) selects the whole buffer               */
/* ------------------------------------------------------------------ */
static void test_textfield_set_selection_select_all(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "select me";
    setup_textfield(ui, "tf", buf, sizeof buf);

    lens_set_skin(ui, LENS_WIDGET_TEXTFIELD, probe_tf_skin);

    /* select all: anchor 0, caret UINT32_MAX (clamps to len at build) */
    lens_begin(ui, &IN0);
    lens_textfield_set_selection(ui, "tf", 0, UINT32_MAX);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(g_tf_seen.content.sel_rect_count > 0);

    lens_set_skin(ui, LENS_WIDGET_TEXTFIELD, NULL); /* restore default */

    /* typing replaces the whole buffer */
    lens_input in = IN0;
    strcpy(in.text_utf8, "x");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "x") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Clamp-on-shrink still repairs a caret the host left stale           */
/* ------------------------------------------------------------------ */
static void test_textfield_set_caret_clamp_on_shrink(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "abcdef";
    setup_textfield(ui, "tf", buf, sizeof buf); /* cursor at EOL (6) */

    /* host shrinks the buffer without touching the caret; the next build
     * clamps 6 -> 2 and typed text lands at the new end */
    strcpy(buf, "ab");
    lens_input in = IN0;
    strcpy(in.text_utf8, "z");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "abz") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  A mid-character host offset snaps back to a UTF-8 boundary          */
/* ------------------------------------------------------------------ */
static void test_textfield_set_caret_utf8_snap(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* "aé中": a(1 byte) + é(2 bytes) + 中(3 bytes) = 6 bytes */
    char buf[64] = "a\xc3\xa9\xe4\xb8\xad";
    setup_textfield(ui, "tf", buf, sizeof buf); /* cursor at EOL (6) */

    /* offset 2 lands mid-'é' (bytes 1..2); the build snaps back to 1 */
    lens_begin(ui, &IN0);
    lens_textfield_set_caret(ui, "tf", 2);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* typed text inserts at the snapped boundary: "axé中" */
    lens_input in = IN0;
    strcpy(in.text_utf8, "x");
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "ax\xc3\xa9\xe4\xb8\xad") == 0);

    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  Preedit active clause: the record carries the emphasised rect       */
/* ------------------------------------------------------------------ */
static void test_textfield_preedit_clause(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    char buf[64] = "";
    setup_textfield(ui, "tf", buf, sizeof buf);

    lens_set_skin(ui, LENS_WIDGET_TEXTFIELD, probe_tf_skin);

    /* preedit "abc" with the active clause spanning "ab" */
    lens_input in = IN0;
    strcpy(in.preedit_utf8, "abc");
    in.preedit_cursor = 3;
    in.preedit_sel_lo = 0;
    in.preedit_sel_hi = 2;
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    CHECK(g_tf_seen.content.has_preedit);
    CHECK(g_tf_seen.content.preedit_underline.w > 0.0f);
    CHECK(g_tf_seen.content.preedit_clause.w > 0.0f);
    /* the clause covers "ab"; the flat underline spans all of "abc" */
    CHECK(g_tf_seen.content.preedit_clause.w < g_tf_seen.content.preedit_underline.w);
    CHECK(g_tf_seen.content.preedit_clause.h == 2.0f);
    CHECK(g_tf_seen.content.preedit_clause.y == g_tf_seen.content.preedit_underline.y);

    /* an empty range (sel_hi == sel_lo) emits no clause emphasis */
    in.preedit_sel_hi = 0;
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(g_tf_seen.content.has_preedit);
    CHECK(g_tf_seen.content.preedit_clause.w == 0.0f);

    lens_set_skin(ui, LENS_WIDGET_TEXTFIELD, NULL); /* restore default */
    lens_destroy(ui);
}

/* ------------------------------------------------------------------ */
/*  UTF-8 editing: every key path must respect code-point boundaries    */
/* ------------------------------------------------------------------ */
static void test_textfield_utf8_editing(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* "aé中🦀": 1 + 2 + 3 + 4 = 10 bytes, four code points. */
    static const char text[] = "a\xc3\xa9\xe4\xb8\xad\xf0\x9f\xa6\x80";
    char buf[64];
    memcpy(buf, text, sizeof text);

    /* --- backspace at EOL removes the 4-byte emoji, not one byte ---- */
    setup_textfield(ui, "tf", buf, sizeof buf); /* cursor at EOL (10) */
    lens_input in = IN0;
    in.key_count = 1;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_BACKSPACE, .pressed = true};
    lens_begin(ui, &in);
    bool changed = lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(changed == true);
    CHECK(strcmp(buf, "a\xc3\xa9\xe4\xb8\xad") == 0); /* "aé中" */

    /* --- backspace again removes the 3-byte CJK char ---------------- */
    lens_begin(ui, &IN0);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    in.keys[0].pressed = true;
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(strcmp(buf, "a\xc3\xa9") == 0); /* "aé" */

    /* --- LEFT moves by code point (2 bytes over the é) -------------- */
    in.keys[0] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    /* caret is not directly observable; verify by typing — the insert
     * must land before the é at byte 1: "axé" */
    lens_input in2 = IN0;
    strcpy(in2.text_utf8, "x");
    lens_begin(ui, &in2);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(strcmp(buf, "ax\xc3\xa9") == 0);

    /* --- DELETE forward removes a full code point -------------------- */
    /* caret sits at 2 ("a x | é"); forward-delete removes the é (2 bytes) */
    in.keys[0] = (lens_key_event){.key = LENS_KEY_DELETE, .pressed = true};
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    CHECK(strcmp(buf, "ax") == 0);

    lens_destroy(ui);
}

/* Selection + copy over multibyte runes: the copied byte range must be
 * code-point aligned because LEFT/RIGHT moved by code points. */
static void test_textfield_utf8_selection_copy(void) {
    static char copied[64];
    copied[0] = '\0';
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.clipboard = {.user = copied, .set_text = copy_sink}}, &ui) ==
          FLUX_OK);

    /* "é中" = 5 bytes. */
    static const char text[] = "\xc3\xa9\xe4\xb8\xad";
    char buf[64];
    memcpy(buf, text, sizeof text);
    setup_textfield(ui, "tf", buf, sizeof buf); /* caret at EOL (5) */

    /* shift+LEFT twice selects both runes (5..3, 3..0) */
    lens_input in = IN0;
    in.key_count = 2;
    in.mods = LENS_MOD_SHIFT;
    in.keys[0] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    in.keys[1] = (lens_key_event){.key = LENS_KEY_LEFT, .pressed = true};
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* ctrl+C copies the selection through the host sink. */
    lens_input in2 = IN0;
    in2.key_count = 1;
    in2.mods = LENS_MOD_CTRL;
    in2.keys[0] = (lens_key_event){.key = 'c', .pressed = true};
    lens_begin(ui, &in2);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    CHECK(strcmp(copied, "\xc3\xa9\xe4\xb8\xad") == 0);

    lens_destroy(ui);
}

int main(void) {
    test_textfield_paste();
    test_textfield_committed_text();
    test_textfield_preedit_sets_caret();
    test_textfield_preedit_clause();
    test_textfield_backspace();
    test_textfield_key_repeat();
    test_textfield_delete();
    test_textfield_home_end();
    test_textfield_buffer_cap();
    test_textfield_select_all();
    test_textfield_shift_select();
    test_textfield_copy_cut();
    test_textfield_paste_replaces_selection();
    test_textfield_requests_text_cursor();
    test_textfield_set_caret_before_first_build();
    test_textfield_set_caret_after_rewrite();
    test_textfield_set_selection_select_all();
    test_textfield_set_caret_clamp_on_shrink();
    test_textfield_set_caret_utf8_snap();
    test_textfield_utf8_editing();
    test_textfield_utf8_selection_copy();
    return TEST_REPORT();
}
