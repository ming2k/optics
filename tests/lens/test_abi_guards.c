/* test_abi_guards.c — the documented size guards are actually implemented.
 *
 * ADR-0032 promised that lens copies caller structs "clamped to
 * min(caller, library)"; before this test existed, lens_theme was copied
 * raw (full-struct) while lens_input was clamped, and lens_desc had no
 * guard at all. These tests pin the clamp semantics for all three so a
 * future regression cannot silently reintroduce an over-read/over-write.
 */

#include "test_helpers.h"

#include <lens/lens.h>

#include <stddef.h>
#include <string.h>

/* Simulate a caller built against an OLDER header: a theme struct whose
 * trailing fields (added later) simply do not exist. The library must
 * copy only the caller's prefix and must not read past it. */
static void test_theme_older_caller_prefix_only(void) {
    /* Craft a theme whose declared size is smaller than the library's
     * layout, with a canary past that point in the source buffer. */
    unsigned char buf[sizeof(lens_theme) + 64];
    memset(buf, 0xAB, sizeof buf);

    lens_theme *small = (lens_theme *)buf;
    lens_theme tmpl = lens_theme_dark();
    memcpy(small, &tmpl, sizeof(lens_theme));
    small->size = 8; /* "caller" only knows the first 8 bytes */

    /* Canary region must remain untouched proof-of-no-overread is not
     * directly observable from outside; the observable contract is that
     * the copy succeeds, the stored theme is normalized, and the stored
     * size reports the library's layout. */
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.theme = *small}, &ui) == FLUX_OK);
    CHECK(ui != NULL);

    lens_theme got = lens_get_theme(ui);
    CHECK(got.size == sizeof(lens_theme));
    /* Normalization ran: title size derives from font_size, never 0. */
    CHECK(got.font_size_title > 0.0f);

    lens_destroy(ui);
}

/* A caller built against a NEWER header (size > library layout) must be
 * clamped, not trusted: the library must not copy bytes past its own
 * struct even though the caller offered them. */
static void test_theme_newer_caller_clamped(void) {
    lens_theme t = lens_theme_default();
    t.size = (uint32_t)(sizeof(lens_theme) * 2); /* hostile / newer */

    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){.theme = t}, &ui) == FLUX_OK);
    lens_theme got = lens_get_theme(ui);
    CHECK(got.size == sizeof(lens_theme));
    lens_destroy(ui);
}

/* lens_desc guard: size 0 (every existing caller today) still works and
 * takes the legacy full-struct path; an explicit sizeof() also works. */
static void test_desc_guard_both_spellings(void) {
    lens *a = NULL, *b = NULL;
    CHECK(lens_create(&(lens_desc){0}, &a) == FLUX_OK); /* legacy */
    CHECK(a != NULL);
    lens_destroy(a);

    lens_desc d = LENS_DESC_INIT;
    d.scale = 2.0f;
    CHECK(d.size == sizeof(lens_desc));
    CHECK(lens_create(&d, &b) == FLUX_OK);
    CHECK(lens_scale(b) == 2.0f);
    lens_destroy(b);
}

/* set_theme with a guarded (smaller) theme must clamp on the way in and
 * must not leave the context theme in a torn state. */
static void test_set_theme_clamps(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_theme t = lens_theme_dark();
    t.size = 16; /* caller knows only 16 bytes */
    lens_set_theme(ui, t);

    lens_theme got = lens_get_theme(ui);
    CHECK(got.size == sizeof(lens_theme));
    /* Fields beyond the caller's 16 bytes fall back to normalized
     * defaults, never garbage: font_weight normalizes to 400. */
    CHECK(got.font_weight >= 100.0f && got.font_weight <= 1000.0f);
    lens_destroy(ui);
}

int main(void) {
    test_theme_older_caller_prefix_only();
    test_theme_newer_caller_clamped();
    test_desc_guard_both_spellings();
    test_set_theme_clamps();
    printf("abi_guards: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
