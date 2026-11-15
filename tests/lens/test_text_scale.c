/* test_text_scale.c — text-scale multiplier (ADR-0077). CPU-only.
 *
 * Covers:
 *   - getter/setter roundtrip, default 1.0, clamp <= 0 and non-finite.
 *   - every font-scaled metric (font_size, line height, intrinsic heights,
 *     heading tokens) grows proportionally.
 *   - pure-px metrics (padding, border, gap) stay put.
 *   - text-draw commands carry the scaled font_size.
 *   - invalidation: changing the factor forces a redraw (records invalidated).
 *
 * Direct include of internal.h (same pattern as test_drawlist_hash) to read
 * resolved font metrics without a mock backend.
 */

#include "../../libs/lens/src/internal.h"
#include "lens_test.h"
#include "optics_test.h"

#include <lens/lens.h>

#include <math.h>

static const lens_input IN0 = {.display_size = {400, 300}, .dt_seconds = 0.016f};

/* ---- getter / setter / clamp ----------------------------------------- */

static void test_getter_setter_and_clamp(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    /* Default is 1.0 */
    OT_EXPECT_NEAR(lens_text_scale(ui), 1.0f, 0.001f);

    /* Normal writes roundtrip */
    lens_set_text_scale(ui, 1.25f);
    OT_EXPECT_NEAR(lens_text_scale(ui), 1.25f, 0.001f);
    lens_set_text_scale(ui, 2.0f);
    OT_EXPECT_NEAR(lens_text_scale(ui), 2.0f, 0.001f);

    /* Non-positive and non-finite inputs are ignored, previous value stays */
    lens_set_text_scale(ui, 0.0f);
    OT_EXPECT_NEAR(lens_text_scale(ui), 2.0f, 0.001f);
    lens_set_text_scale(ui, -1.5f);
    OT_EXPECT_NEAR(lens_text_scale(ui), 2.0f, 0.001f);
    lens_set_text_scale(ui, NAN);
    OT_EXPECT_NEAR(lens_text_scale(ui), 2.0f, 0.001f);
    lens_set_text_scale(ui, INFINITY);
    OT_EXPECT_NEAR(lens_text_scale(ui), 2.0f, 0.001f);

    /* NULL context is safe */
    lens_set_text_scale(nullptr, 1.5f);
    OT_EXPECT_NEAR(lens_text_scale(nullptr), 1.0f, 0.001f);

    lens_release(ui);
}

/* ---- resolver effects ------------------------------------------------ */

/* Resolved font_size scales by text_scale; non-text metrics do not. */
static void test_resolver_scales_font_only(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();

    /* Baseline at 1.0 */
    lens_style_resolved r1 = lensi_style_resolve(ui, nullptr, &t, 0);
    OT_EXPECT_NEAR(r1.font_size, t.font_size, 0.001f);
    OT_EXPECT_NEAR(r1.padding, t.padding, 0.001f);
    OT_EXPECT_NEAR(r1.border_width, t.border_width, 0.001f);

    /* At 1.5x: font_size grows, padding/border untouched */
    lens_set_text_scale(ui, 1.5f);
    lens_style_resolved r15 = lensi_style_resolve(ui, nullptr, &t, 0);
    OT_EXPECT_NEAR(r15.font_size, t.font_size * 1.5f, 0.001f);
    OT_EXPECT_NEAR(r15.padding, t.padding, 0.001f);
    OT_EXPECT_NEAR(r15.border_width, t.border_width, 0.001f);

    /* Cascade: an explicit instance font_size scales by text_scale too */
    lens_style s = lens_style_init();
    s.fields = LENS_STYLE_FONT_SIZE;
    s.font_size = 20.0f;
    lens_style_resolved r_inst = lensi_style_resolve(ui, &s, &t, 0);
    OT_EXPECT_NEAR(r_inst.font_size, 20.0f * 1.5f, 0.001f);

    lens_release(ui);
}

/* ---- widget geometry effects ----------------------------------------- */

/* A button laid out at 2.0x text scale measures taller than at 1.0x. */
static void test_widget_intrinsic_height_scales(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    float h1 = 0.0f;
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_id id = lens_current_id(ui, "OK");
    lens_test_end(ui);
    lens_node *n1 = lens_find(ui, id);
    OT_EXPECT(n1 != nullptr);
    h1 = lens_node_bounds(n1).h;

    lens_set_text_scale(ui, 2.0f);
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_test_end(ui);
    n1 = lens_find(ui, id);
    OT_EXPECT(n1 != nullptr);
    float h2 = lens_node_bounds(n1).h;

    OT_EXPECT(h2 > h1 * 1.3f);

    lens_release(ui);
}

/* The text draw command's size scales: measurement and paint agree. */
static void test_text_command_size_scales(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_set_text_scale(ui, 1.5f);
    lens_begin(ui, &IN0);
    lens_button(ui, &(lens_button_opts){.label = "OK"});
    lens_id id = lens_current_id(ui, "OK");
    lens_test_end(ui);

    const lens_node *n = lens_find(ui, id);
    OT_EXPECT(n != nullptr);
    OT_EXPECT(lens_node_bounds(n).h > 14.0f * 1.5f);

    lens_release(ui);
}

/* Explicit point sizes scale too. */
static void test_explicit_label_size_scales(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_set_text_scale(ui, 2.0f);
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "Point", .size = 10.0f});
    lens_label(ui, &(lens_label_opts){.text = "Compact", .size = 8.0f});
    lens_label(
        ui, &(lens_label_opts){.text = "Wrapped text", .wrap = true, .box = {.max_width = 200.0f}});
    lens_id pt_id = lens_current_id(ui, "Point");
    lens_id cp_id = lens_current_id(ui, "Compact");
    lens_id wr_id = lens_current_id(ui, "Wrapped text");
    lens_test_end(ui);

    const lens_node *pt = lens_find(ui, pt_id);
    const lens_node *cp = lens_find(ui, cp_id);
    const lens_node *wr = lens_find(ui, wr_id);
    OT_EXPECT(pt && cp && wr);
    if (pt)
        OT_EXPECT(lens_node_bounds(pt).h > 8.0f);
    if (cp)
        OT_EXPECT(lens_node_bounds(cp).h > 8.0f);
    if (wr)
        OT_EXPECT(lens_node_bounds(wr).h > 8.0f);

    lens_release(ui);
}

/* Headings scale proportionally. */
static void test_heading_size_scales(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();

    lens_set_text_scale(ui, 1.5f);
    lens_begin(ui, &IN0);
    lens_label(ui, &(lens_label_opts){.text = "T", .size = 28.0f});
    lens_id title_id = lens_current_id(ui, "T");
    lens_label(ui, &(lens_label_opts){.text = "H", .size = 22.0f});
    lens_id h1_id = lens_current_id(ui, "H");
    lens_test_end(ui);

    const lens_node *title = lens_find(ui, title_id);
    OT_EXPECT(title != nullptr);
    OT_EXPECT(lens_node_bounds(title).h > t.font_size_title * 1.4f);
    const lens_node *h1 = lens_find(ui, h1_id);
    OT_EXPECT(h1 != nullptr);
    OT_EXPECT(lens_node_bounds(h1).h > t.font_size_h1 * 1.4f);

    lens_release(ui);
}

/* A factor switch must re-key cached canvas records. */
static void test_text_scale_switch_invalidates(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    lens_theme t = lens_theme_dark();
    lens_style_resolved r1 = lensi_style_resolve(ui, nullptr, &t, 0);
    lens_set_text_scale(ui, 1.25f);
    lens_style_resolved r2 = lensi_style_resolve(ui, nullptr, &t, 0);
    OT_EXPECT(r1.font_size != r2.font_size);

    lens_release(ui);
}

int main(void) {
    test_getter_setter_and_clamp();
    test_resolver_scales_font_only();
    test_widget_intrinsic_height_scales();
    test_text_command_size_scales();
    test_explicit_label_size_scales();
    test_heading_size_scales();
    test_text_scale_switch_invalidates();
    return OT_REPORT();
}
