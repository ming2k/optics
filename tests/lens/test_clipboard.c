/* test_clipboard.c — host clipboard interface, paste delivery, caret
 * default, the focused-widget text context (ADR-0036), and the
 * size-aware lens_input copy guard (ADR-0013). */

#include "test_helpers.h"
#include <lens/lens.h>
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
    host_clip host = {0};
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.clipboard = {.set_text = host_set_text,
                                                 .request_text = host_request_text,
                                                 .user = &host}},
                      &ui) == FLUX_OK);

    /* copy forwards to host.set_text */
    lens_copy(ui, "hello", 5);
    CHECK(host.copied_len == 5);
    CHECK(strcmp(host.copied, "hello") == 0);

    /* request forwards to host.request_text */
    lens_request_paste(ui);
    lens_request_paste(ui);
    CHECK(host.request_count == 2);

    /* no-args / zero-len are safe no-ops */
    lens_copy(ui, NULL, 0);
    lens_copy(ui, "x", 0);
    CHECK(host.copied_len == 5);

    lens_destroy(ui);
}

static void test_copy_without_clipboard_is_noop(void) {
    /* No clipboard supplied → all clipboard calls must be safe no-ops. */
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_copy(ui, "x", 1);
    lens_request_paste(ui);
    lens_paste(ui, "y", 1); /* queues but no consumer */
    lens_destroy(ui);
}

static void test_caret_rect_default_zero(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_end(ui);
    flux_rect c = lens_caret_rect(ui);
    CHECK(c.x == 0 && c.y == 0 && c.w == 0 && c.h == 0);
    lens_destroy(ui);
}

/* The size guard: a zero-sized lens_input (legacy callers) must still copy
 * cleanly; a partial size must zero everything past it. We can only
 * observe the copy indirectly through behaviour: an input with size set
 * to a small prefix should still drive the frame without UB and leave
 * pre-edit fields cleared. */
static void test_input_size_guard(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* Legacy: size==0 → trust full struct (current behaviour). */
    lens_input legacy = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    legacy.size = 0;
    lens_begin(ui, &legacy);
    lens_end(ui);

    /* Forward-compat: size set to sizeof(lens_input). */
    lens_input modern = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    modern.size = sizeof(lens_input);
    memcpy(modern.preedit_utf8, "abc", 3);
    modern.preedit_cursor = 2;
    lens_begin(ui, &modern);
    lens_end(ui);
    /* nothing crashes; preedit copied through */
    CHECK(lens_caret_rect(ui).w == 0); /* still no text widget */

    /* Truncated: a partial size only covers the first byte-range. The
     * library must clamp to its own sizeof, never read past, and zero
     * what wasn't supplied. */
    lens_input small = {.size = 8}; /* far less than sizeof(lens_input) */
    lens_begin(ui, &small);
    lens_end(ui);

    lens_destroy(ui);
}

/* ---- focused-widget text context (lens_text_context_get, ADR-0036) ---- */

static void test_text_context_default_null(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_end(ui);
    lens_text_context tc = lens_text_context_get(ui);
    CHECK(tc.utf8 == NULL);
    CHECK(tc.len == 0 && tc.cursor == 0 && tc.multiline == false);
    lens_destroy(ui);
}

/* A focused textfield reports its buffer, caret offset, and
 * multiline=false; the context clears again once no text widget is
 * built. (Focus pattern mirrors test_textfield.c's setup_textfield.) */
static void test_text_context_textfield(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    char buf[64] = "hello";
    /* frame 1: enter; frame 2: focus explicitly */
    lens_begin(ui, &in);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);
    lens_begin(ui, &in);
    lens_set_focus(ui, lens_current_id(ui, "tf"));
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    /* frame 3: caret to end (EOL = 5) */
    lens_input end = in;
    end.key_count = 1;
    end.keys[0] = (lens_key_event){.key = LENS_KEY_END, .pressed = true};
    lens_begin(ui, &end);
    lens_textfield(ui, "tf", buf, sizeof buf);
    lens_end(ui);

    lens_text_context tc = lens_text_context_get(ui);
    CHECK(tc.utf8 == buf);
    CHECK(tc.len == 5);
    CHECK(tc.cursor == 5);
    CHECK(tc.multiline == false);

    /* frame 4: no text widget built — the context is cleared again */
    lens_begin(ui, &in);
    lens_end(ui);
    tc = lens_text_context_get(ui);
    CHECK(tc.utf8 == NULL);
    CHECK(tc.len == 0 && tc.cursor == 0 && tc.multiline == false);

    lens_destroy(ui);
}

/* A focused textarea reports multiline=true (focus pattern from
 * test_textarea.c's focus_textarea). */
static void test_text_context_textarea(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_input in = {.display_size = {100, 100}, .dt_seconds = 0.016f};

    char buf[64] = "ab\ncd";
    lens_begin(ui, &in);
    lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);
    lens_begin(ui, &in);
    lens_set_focus(ui, lens_current_id(ui, "ta"));
    lens_textarea(ui, "ta", buf, sizeof buf, 80.0f);
    lens_end(ui);

    lens_text_context tc = lens_text_context_get(ui);
    CHECK(tc.utf8 == buf);
    CHECK(tc.len == 5);
    CHECK(tc.cursor == 0);
    CHECK(tc.multiline == true);

    /* no text widget built — cleared again */
    lens_begin(ui, &in);
    lens_end(ui);
    tc = lens_text_context_get(ui);
    CHECK(tc.utf8 == NULL);

    lens_destroy(ui);
}

int main(void) {
    test_copy_and_request();
    test_copy_without_clipboard_is_noop();
    test_caret_rect_default_zero();
    test_input_size_guard();
    test_text_context_default_null();
    test_text_context_textfield();
    test_text_context_textarea();
    return TEST_REPORT();
}
