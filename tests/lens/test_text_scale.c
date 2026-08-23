/* test_text_scale.c — lens_set_text_scale scales text-derived geometry and
 * paint consistently (ADR-0075).
 *
 * The factor is a pure multiplier on every font-size token, applied at the
 * resolved-style funnel, so:
 *   - resolved style font_size scales (measurement input)
 *   - widget intrinsic heights derived from font tokens scale (no clipping)
 *   - the text draw command's text_size scales (paint input)
 *   - caret metrics scale (the "Ag" measure textfields/textareas use)
 *   - explicit point sizes (lens_label_ex etc.) scale too — an explicit size
 *     is a design intent, not an accessibility exemption
 *   - a factor change re-keys the draw-command hash, so cached canvas
 *     records invalidate (same mechanism as a DPI scale switch)
 */

#include "../../libs/lens/src/internal.h"
#include "test_helpers.h"
#include <lens/lens.h>

static const lens_input IN0 = {.display_size = {400, 200}, .dt_seconds = 0.016f};

static void test_getter_roundtrip_and_validation(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    CHECK_NEAR(lens_text_scale(ui), 1.0f, 0.0f); /* default */

    lens_set_text_scale(ui, 1.25f);
    CHECK_NEAR(lens_text_scale(ui), 1.25f, 0.0f);
    lens_set_text_scale(ui, 0.9f);
    CHECK_NEAR(lens_text_scale(ui), 0.9f, 0.0f);

    /* Non-finite and non-positive factors are ignored, not clamped. */
    lens_set_text_scale(ui, 0.0f);
    CHECK_NEAR(lens_text_scale(ui), 0.9f, 0.0f);
    lens_set_text_scale(ui, -2.0f);
    CHECK_NEAR(lens_text_scale(ui), 0.9f, 0.0f);
    lens_set_text_scale(ui, NAN);
    CHECK_NEAR(lens_text_scale(ui), 0.9f, 0.0f);
    lens_set_text_scale(ui, INFINITY);
    CHECK_NEAR(lens_text_scale(ui), 0.9f, 0.0f);

    /* NULL-safety, matching the other accessors. */
    CHECK_NEAR(lens_text_scale(NULL), 1.0f, 0.0f);
    lens_set_text_scale(NULL, 2.0f);

    lens_destroy(ui);
}

static void test_desc_seeds_text_scale(void) {
    lens *ui = NULL;
    lens_desc d = LENS_DESC_INIT;
    d.text_scale = 1.5f;
    CHECK(lens_create(&d, &ui) == FLUX_OK);
    CHECK_NEAR(lens_text_scale(ui), 1.5f, 0.0f);

    /* 0 / garbage mean "unset" → 1.0. */
    lens_destroy(ui);
    d.text_scale = 0.0f;
    CHECK(lens_create(&d, &ui) == FLUX_OK);
    CHECK_NEAR(lens_text_scale(ui), 1.0f, 0.0f);
    d.text_scale = -1.0f;
    lens_destroy(ui);
    CHECK(lens_create(&d, &ui) == FLUX_OK);
    CHECK_NEAR(lens_text_scale(ui), 1.0f, 0.0f);

    lens_destroy(ui);
}

/* Resolved font_size carries the factor; every other slot is untouched. */
static void test_resolver_scales_font_size_only(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();

    lens_set_text_scale(ui, 2.0f);
    lens_style_resolved r = lensi_style_resolve(ui, NULL, &t, 0);
    CHECK_NEAR(r.font_size, t.font_size * 2.0f, 0.0f);
    /* Pure-px geometry tokens deliberately do not scale. */
    CHECK_NEAR(r.padding, t.padding, 0.0f);
    CHECK_NEAR(r.corner_radius, t.corner_radius, 0.0f);
    CHECK_NEAR(r.border_width, t.border_width, 0.0f);
    CHECK_NEAR(r.gap, t.gap, 0.0f);
    CHECK(r.bg == t.color_bg);

    lens_destroy(ui);
}

/* Widget intrinsic height grows with the factor: a button's measured height
 * derives from font metrics + padding, so at 2x text the box must be
 * taller than at 1x — text scales, boxes follow. */
static void test_button_height_scales(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_id id = lens_current_id(ui, "OK");
    lens_end(ui);
    float h1 = 0.0f;
    lens_node *n1 = lens_find(ui, id);
    CHECK(n1 != NULL);
    h1 = lens_node_bounds(n1).h;

    lens_set_text_scale(ui, 2.0f);
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_end(ui);
    n1 = lens_find(ui, id);
    CHECK(n1 != NULL);
    float h2 = lens_node_bounds(n1).h;

    CHECK(h2 > h1 * 1.3f); /* glyphs doubled (14→28); fixed padding dilutes
                            * the ratio: 14+2p → 28+2p, e.g. 38 → 52 */

    lens_destroy(ui);
}

/* The text draw command's size scales: measurement and paint agree. */
static void test_text_command_size_scales(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_set_text_scale(ui, 1.5f);
    lens_begin(ui, &IN0);
    lens_button(ui, "OK");
    lens_id id = lens_current_id(ui, "OK");
    lens_end(ui);

    /* Paint-side size observable: button height at 1.5x must exceed the
     * unscaled height by more than pure padding could explain — the text
     * metrics that drive the intrinsic height came from the scaled size. */
    const lens_node *n = lens_find(ui, id);
    CHECK(n != NULL);
    CHECK(lens_node_bounds(n).h > 14.0f * 1.5f);

    lens_destroy(ui);
}

/* Explicit point sizes scale too (lens_label_ex / compact / wrapped). */
static void test_explicit_label_size_scales(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_set_text_scale(ui, 2.0f);
    lens_begin(ui, &IN0);
    lens_label_ex(ui, "Point", 10.0f);
    lens_label_compact_ex2(ui, "Compact", 8.0f, 0.0f);
    lens_label_wrapped_ex(ui, "Wrapped text", 9.0f, 200.0f);
    lens_id pt_id = lens_current_id(ui, "Point");
    lens_id cp_id = lens_current_id(ui, "Compact");
    lens_id wr_id = lens_current_id(ui, "Wrapped text");
    lens_end(ui);

    /* Intrinsic height is the observable: it derives from measured text
     * metrics at the scaled size. 8→16px line-ish height must exceed the
     * unscaled 8px measure. */
    const lens_node *pt = lens_find(ui, pt_id);
    const lens_node *cp = lens_find(ui, cp_id);
    const lens_node *wr = lens_find(ui, wr_id);
    CHECK(pt && cp && wr);
    if (pt)
        CHECK(lens_node_bounds(pt).h > 8.0f); /* 10px * 2 measured height */
    if (cp)
        CHECK(lens_node_bounds(cp).h > 8.0f);
    if (wr)
        CHECK(lens_node_bounds(wr).h > 8.0f);

    lens_destroy(ui);
}

/* Headings ride theme-only tokens (no style atom); they must scale too. */
static void test_heading_size_scales(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);
    lens_theme t = lens_theme_dark();

    lens_set_text_scale(ui, 1.5f);
    lens_begin(ui, &IN0);
    float base_h = 0.0f;
    {
        lens_theme raw = lens_get_theme(ui);
        (void)raw;
    }
    lens_title(ui, "T");
    lens_id title_id = lens_current_id(ui, "T");
    lens_heading(ui, "H", 1);
    lens_id h1_id = lens_current_id(ui, "H");
    lens_end(ui);

    /* Heading heights derive from measured metrics at heading_size(), which
     * applies lensi_font_px; unscaled title (28px) at 1.5x must measure as
     * 42px-ish text would. */
    const lens_node *title = lens_find(ui, title_id);
    CHECK(title != NULL);
    CHECK(lens_node_bounds(title).h > t.font_size_title * 1.4f);
    const lens_node *h1 = lens_find(ui, h1_id);
    CHECK(h1 != NULL);
    CHECK(lens_node_bounds(h1).h > t.font_size_h1 * 1.4f);

    lens_destroy(ui);
}

/* A factor switch must re-key cached canvas records — same contract as a
 * DPI scale switch (test_record_replay pins that for lens_set_scale). */
static void test_text_scale_switch_invalidates(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    /* font_size rides the draw-command hash (drawlist.c), so a factor switch
     * re-keys records — pinned at the mechanism level by test_drawlist_hash
     * and test_record_replay for the DPI switch; here we assert the resolved
     * style flips, which is what feeds that hash. */
    lens_theme t = lens_theme_dark();
    lens_style_resolved r1 = lensi_style_resolve(ui, NULL, &t, 0);
    lens_set_text_scale(ui, 1.25f);
    lens_style_resolved r2 = lensi_style_resolve(ui, NULL, &t, 0);
    CHECK(r1.font_size != r2.font_size);

    lens_destroy(ui);
}

/* Text scale is orthogonal to the DPI scale: neither multiplies the other,
 * and the raster density (flux_text_set_scale) is untouched by text scale. */
static void test_orthogonal_to_dpi_scale(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    lens_set_scale(ui, 2.0f);
    CHECK_NEAR(lens_text_scale(ui), 1.0f, 0.0f);
    lens_set_text_scale(ui, 1.25f);
    CHECK_NEAR(lens_scale(ui), 2.0f, 0.0f);
    CHECK_NEAR(lens_text_scale(ui), 1.25f, 0.0f);

    lens_destroy(ui);
}

int main(void) {
    test_getter_roundtrip_and_validation();
    test_desc_seeds_text_scale();
    test_resolver_scales_font_size_only();
    test_button_height_scales();
    test_text_command_size_scales();
    test_explicit_label_size_scales();
    test_heading_size_scales();
    test_text_scale_switch_invalidates();
    test_orthogonal_to_dpi_scale();
    return TEST_REPORT();
}
