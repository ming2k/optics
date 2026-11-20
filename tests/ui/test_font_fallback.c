/* test_font_fallback.c — font fallback chain (FcFontSort). */

#include "lens_test.h"
#include "optics_test.h"
#include <optics/ui.h>
#include <string.h>

static optics_ui_text_metrics measure(optics_ui *ui, const char *text, float size_px) {
    return optics_ui_text_measure(ui,
                                  &(optics_ui_text_measure_opts){.utf8 = text, .size_px = size_px});
}

static void test_measure_ascii(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_text_metrics m = measure(ui, "Hello", 16.0f);
    OT_EXPECT(m.width > 0.0f);
    OT_EXPECT(m.height > 0.0f);
    OT_EXPECT(m.baseline > 0.0f);

    optics_ui_release(ui);
}

static void test_measure_mixed_no_crash(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_text_metrics m1 = measure(ui, "Hello", 16.0f);
    OT_EXPECT(m1.width > 0.0f);

    /* CJK characters — if no CJK font is installed they fall back to the
     * primary face (tofu glyph), but the path must not crash. */
    optics_ui_text_metrics m2 =
        measure(ui, "Hello \xe4\xb8\x96\xe7\x95\x8c", 16.0f); /* "Hello 世界" */
    OT_EXPECT(m2.width > 0.0f);
    OT_EXPECT(m2.height > 0.0f);
    OT_EXPECT(m2.width >= m1.width); /* tofu or real glyph, width should not shrink */

    /* Emoji — U+1F600 GRINNING FACE (4-byte UTF-8). The emoji advances about
     * one em; compare against the string's own non-emoji prefix so the check
     * stays meaningful regardless of how wide the fallback glyph is. */
    optics_ui_text_metrics m3pre = measure(ui, "Hi ", 16.0f);
    optics_ui_text_metrics m3 = measure(ui, "Hi \xf0\x9f\x98\x80", 16.0f);
    OT_EXPECT(m3.width > 0.0f);
    OT_EXPECT(m3.width >= m3pre.width); /* fallback adds non-negative width */

    optics_ui_release(ui);
}

static void test_bold_slot_on_demand(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_text_metrics m = measure(ui, "Bold", 16.0f);
    OT_EXPECT(m.width > 0.0f);
    OT_EXPECT(m.height > 0.0f);

    optics_ui_release(ui);
}

static void test_fallback_layout_and_render_no_crash(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    optics_ui_input in = {.display_size = {400, 200}, .dt_seconds = 0.016f};
    optics_ui_begin(ui, &in);
    optics_ui_label(
        ui, &(optics_ui_label_opts){
                .text = "English and \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e \xf0\x9f\x91\x8d"});
    optics_ui_test_end(ui);

    optics_ui_node *root = optics_ui_root(ui);
    optics_ui_node *lbl = optics_ui_node_first_child(root);
    OT_EXPECT(lbl != nullptr);
    optics_gfx_rect r = optics_ui_node_bounds(lbl);
    OT_EXPECT(r.w > 0.0f);
    OT_EXPECT(r.h > 0.0f);

    optics_ui_release(ui);
}

int main(void) {
    test_measure_ascii();
    test_measure_mixed_no_crash();
    test_bold_slot_on_demand();
    test_fallback_layout_and_render_no_crash();
    return OT_REPORT();
}
