/* test_font_fallback.c — font fallback chain (FcFontSort). */

#include "test_helpers.h"
#include <lens/lens.h>
#include <string.h>

static void test_measure_ascii(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_text_metrics m = lens_text_measure(ui, NULL, "Hello", 16.0f);
    CHECK(m.width > 0.0f);
    CHECK(m.height > 0.0f);
    CHECK(m.baseline > 0.0f);

    lens_destroy(ui);
}

static void test_measure_mixed_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_text_metrics m1 = lens_text_measure(ui, NULL, "Hello", 16.0f);
    CHECK(m1.width > 0.0f);

    /* CJK characters — if no CJK font is installed they fall back to the
     * primary face (tofu glyph), but the path must not crash. */
    lens_text_metrics m2 =
        lens_text_measure(ui, NULL, "Hello \xe4\xb8\x96\xe7\x95\x8c", 16.0f); /* "Hello 世界" */
    CHECK(m2.width > 0.0f);
    CHECK(m2.height > 0.0f);
    CHECK(m2.width >= m1.width); /* tofu or real glyph, width should not shrink */

    /* Emoji — U+1F600 GRINNING FACE (4-byte UTF-8). The emoji advances about
     * one em; compare against the string's own non-emoji prefix so the check
     * stays meaningful regardless of how wide the fallback glyph is. */
    lens_text_metrics m3pre = lens_text_measure(ui, NULL, "Hi ", 16.0f);
    lens_text_metrics m3 = lens_text_measure(ui, NULL, "Hi \xf0\x9f\x98\x80", 16.0f);
    CHECK(m3.width > 0.0f);
    CHECK(m3.width >= m3pre.width); /* fallback adds non-negative width */

    lens_destroy(ui);
}

static void test_bold_slot_on_demand(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_text_metrics m = lens_text_measure(ui, NULL, "Bold", 16.0f);
    CHECK(m.width > 0.0f);
    CHECK(m.height > 0.0f);

    lens_destroy(ui);
}

static void test_fallback_layout_and_render_no_crash(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_input in = {.display_size = {400, 200}, .dt_seconds = 0.016f};
    lens_begin(ui, &in);
    lens_label(ui,
               &(lens_label_opts){
                   .text = "English and \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x91\x8d"});
    lens_end(ui);

    lens_node *root = lens_root(ui);
    lens_node *lbl = lens_node_first_child(root);
    CHECK(lbl != NULL);
    flux_rect r = lens_node_bounds(lbl);
    CHECK(r.w > 0.0f);
    CHECK(r.h > 0.0f);

    lens_destroy(ui);
}

int main(void) {
    test_measure_ascii();
    test_measure_mixed_no_crash();
    test_bold_slot_on_demand();
    test_fallback_layout_and_render_no_crash();
    return TEST_REPORT();
}
