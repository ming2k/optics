/* test_clipboard.c — host clipboard interface, paste delivery, caret
 * default, the focused-widget text context (ADR-0036), and the
 * size-aware optics_ui_input copy guard (ADR-0013). */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>
#include <string.h>

/* ---- recording host clipboard ---- */

typedef struct host_clip {
    char copied[64];
    size_t copied_len;
    int request_count;
} host_clip;

static void host_set_text(const char *utf8, size_t len, void *user) {
    host_clip *h = user;
    size_t n = len < sizeof h->copied - 1 ? len : sizeof h->copied - 1;
    memcpy(h->copied, utf8, n);
    h->copied[n] = '\0';
    h->copied_len = n;
}
static void host_request_text(void *user) {
    ((host_clip *)user)->request_count++;
}

static void test_copy_and_request(void) {
    host_clip host = {};
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){.clipboard = {.set_text = host_set_text,
                                                               .request_text = host_request_text,
                                                               .user = &host}},
                               &ui) == OPTICS_GFX_OK);

    /* copy forwards to host.set_text */
    optics_ui_copy(ui, &(optics_ui_copy_opts){.utf8 = "hello", .len = 5});
    OT_EXPECT(host.copied_len == 5);
    OT_EXPECT(strcmp(host.copied, "hello") == 0);

    /* request forwards to host.request_text */
    optics_ui_request_paste(ui);
    optics_ui_request_paste(ui);
    OT_EXPECT(host.request_count == 2);

    /* no-args / zero-len are safe no-ops */
    optics_ui_copy(ui, nullptr);
    optics_ui_copy(ui, &(optics_ui_copy_opts){.utf8 = "x", .len = 0});
    OT_EXPECT(host.copied_len == 5);

    optics_ui_release(ui);
}

static void test_copy_without_clipboard_is_noop(void) {
    /* No clipboard supplied → all clipboard calls must be safe no-ops. */
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_copy(ui, &(optics_ui_copy_opts){.utf8 = "x", .len = 1});
    optics_ui_request_paste(ui);
    optics_ui_paste(ui,
                    &(optics_ui_paste_opts){.utf8 = "y", .len = 1}); /* queues but no consumer */
    optics_ui_release(ui);
}

static void test_caret_rect_default_zero(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    optics_gfx_rect c = optics_ui_caret_rect(ui);
    OT_EXPECT(c.x == 0 && c.y == 0 && c.w == 0 && c.h == 0);
    optics_ui_release(ui);
}

/* The size guard: a zero-sized optics_ui_input (legacy callers) must still copy
 * cleanly; a partial size must zero everything past it. We can only
 * observe the copy indirectly through behaviour: an input with size set
 * to a small prefix should still drive the frame without UB and leave
 * pre-edit fields cleared. */
static void test_input_size_guard(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    /* Legacy: size==0 → trust full struct (current behaviour). */
    optics_ui_input legacy = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    legacy.size = 0;
    optics_ui_begin(ui, &legacy);
    optics_ui_test_end(ui);

    /* Forward-compat: size set to sizeof(optics_ui_input). */
    optics_ui_input modern = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    modern.size = sizeof(optics_ui_input);
    memcpy(modern.preedit_utf8, "abc", 3);
    modern.preedit_cursor = 2;
    optics_ui_begin(ui, &modern);
    optics_ui_test_end(ui);
    /* nothing crashes; preedit copied through */
    OT_EXPECT(optics_ui_caret_rect(ui).w == 0); /* still no text widget */

    /* Truncated: a partial size only covers the first byte-range. The
     * library must clamp to its own sizeof, never read past, and zero
     * what wasn't supplied. */
    optics_ui_input small = {.size = 8}; /* far less than sizeof(optics_ui_input) */
    optics_ui_begin(ui, &small);
    optics_ui_test_end(ui);

    optics_ui_release(ui);
}

/* ---- focused-widget text context (optics_ui_text_context_get, ADR-0036) ---- */

static void test_text_context_default_null(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    optics_ui_text_context tc = optics_ui_text_context_get(ui);
    OT_EXPECT(tc.utf8 == nullptr);
    OT_EXPECT(tc.len == 0 && tc.cursor == 0 && tc.multiline == false);
    optics_ui_release(ui);
}

/* A focused textfield reports its buffer, caret offset, and
 * multiline=false; the context clears again once no text widget is
 * built. (Focus pattern mirrors test_textfield.c's setup_textfield.) */
static void test_text_context_textfield(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    char buf[64] = "hello";
    /* frame 1: enter; frame 2: focus explicitly */
    optics_ui_begin(ui, &in);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &in);
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "tf"));
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);

    /* frame 3: caret to end (EOL = 5) */
    optics_ui_input end = in;
    end.key_count = 1;
    end.keys[0] = (optics_ui_key_event){.key = OPTICS_UI_KEY_END, .pressed = true};
    optics_ui_begin(ui, &end);
    optics_ui_textedit(
        ui, &(optics_ui_textedit_opts){.box = {.id = "tf"}, .buf = buf, .cap = sizeof buf});
    optics_ui_test_end(ui);

    optics_ui_text_context tc = optics_ui_text_context_get(ui);
    OT_EXPECT(tc.utf8 == buf);
    OT_EXPECT(tc.len == 5);
    OT_EXPECT(tc.cursor == 5);
    OT_EXPECT(tc.multiline == false);

    /* frame 4: no text widget built — the context is cleared again */
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    tc = optics_ui_text_context_get(ui);
    OT_EXPECT(tc.utf8 == nullptr);
    OT_EXPECT(tc.len == 0 && tc.cursor == 0 && tc.multiline == false);

    optics_ui_release(ui);
}

/* A focused textarea reports multiline=true (focus pattern from
 * test_textarea.c's focus_textarea). */
static void test_text_context_textarea(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);
    optics_ui_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    char buf[64] = "ab\ncd";
    optics_ui_begin(ui, &in);
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "ta", .min_height = 80.0f},
                                                      .buf = buf,
                                                      .cap = sizeof buf,
                                                      .multiline = true});
    optics_ui_test_end(ui);
    optics_ui_begin(ui, &in);
    optics_ui_set_focus(ui, optics_ui_current_id(ui, "ta"));
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){.box = {.id = "ta", .min_height = 80.0f},
                                                      .buf = buf,
                                                      .cap = sizeof buf,
                                                      .multiline = true});
    optics_ui_test_end(ui);

    optics_ui_text_context tc = optics_ui_text_context_get(ui);
    OT_EXPECT(tc.utf8 == buf);
    OT_EXPECT(tc.len == 5);
    OT_EXPECT(tc.cursor == 0);
    OT_EXPECT(tc.multiline == true);

    /* no text widget built — cleared again */
    optics_ui_begin(ui, &in);
    optics_ui_test_end(ui);
    tc = optics_ui_text_context_get(ui);
    OT_EXPECT(tc.utf8 == nullptr);

    optics_ui_release(ui);
}

int main(void) {
    test_copy_and_request();
    test_copy_without_clipboard_is_noop();
    test_caret_rect_default_zero();
    test_input_size_guard();
    test_text_context_default_null();
    test_text_context_textfield();
    test_text_context_textarea();
    return OT_REPORT();
}
