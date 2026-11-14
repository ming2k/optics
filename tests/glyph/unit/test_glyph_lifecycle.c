/*
 * glyph_lifecycle — the public contract that does not need a font.
 *
 * Version accessors, retain/release, scale and default-family policy,
 * compaction, and null tolerance. These are exactly the edges a binding or a
 * host touches before it can shape a single run, so they must not depend on
 * Fontconfig, FreeType or a Vulkan device.
 */

#include "optics_test.h"

#include <glyph/glyph.h>

#include <string.h>

int main(void) {
    /* ---- Version surface: macros, accessors, packed number, string ---- */
    int major = -1, minor = -1, patch = -1;
    glyph_version(&major, &minor, &patch);
    OT_EXPECT(major == GLYPH_VERSION_MAJOR);
    OT_EXPECT(minor == GLYPH_VERSION_MINOR);
    OT_EXPECT(patch == GLYPH_VERSION_PATCH);
    OT_EXPECT(glyph_version_number() == GLYPH_VERSION_NUMBER);
    OT_EXPECT(glyph_version_number() ==
              ((uint32_t)GLYPH_VERSION_MAJOR << 16 | (uint32_t)GLYPH_VERSION_MINOR << 8 |
               (uint32_t)GLYPH_VERSION_PATCH));
    OT_EXPECT(glyph_version_check(GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR, GLYPH_VERSION_PATCH));
    OT_EXPECT(!glyph_version_check(GLYPH_VERSION_MAJOR + 1, 0, 0));
    OT_EXPECT(!glyph_version_check(GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR + 1, 0));
    OT_EXPECT(
        !glyph_version_check(GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR, GLYPH_VERSION_PATCH + 1));

    char expected[32];
    snprintf(expected, sizeof expected, "%d.%d.%d", GLYPH_VERSION_MAJOR, GLYPH_VERSION_MINOR,
             GLYPH_VERSION_PATCH);
    OT_EXPECT_STR_EQ(glyph_version_string(), expected);

    /* Missing out-parameters are tolerated independently. */
    glyph_version(nullptr, nullptr, nullptr);
    glyph_version(&major, nullptr, nullptr);
    OT_EXPECT(major == GLYPH_VERSION_MAJOR);

    /* ---- Null context policy ---- */
    OT_EXPECT(glyph_scale(nullptr) == 1.0f);
    OT_EXPECT(glyph_default_family(nullptr) == GLYPH_FAMILY_SANS);
    glyph_set_scale(nullptr, 4.0f);
    glyph_set_default_family(nullptr, GLYPH_FAMILY_MONO);
    glyph_compact(nullptr);
    glyph_release(nullptr);

    glyph_metrics empty = glyph_measure(nullptr, "A", 1, nullptr);
    OT_EXPECT(empty.width == 0.0f && empty.height == 0.0f && empty.baseline == 0.0f);

    /* ---- Context lifecycle ---- */
    glyph_ctx *text = nullptr;
    OT_REQUIRE(glyph_create(&(glyph_desc){.device = nullptr, .scale = 1.0f}, &text) == FLUX_OK);
    OT_REQUIRE(text != nullptr);

    OT_EXPECT(glyph_scale(text) == 1.0f);
    glyph_set_scale(text, 2.0f);
    OT_EXPECT(glyph_scale(text) == 2.0f);
    glyph_set_scale(text, 0.0f);
    OT_EXPECT(glyph_scale(text) == 1.0f);
    glyph_set_scale(text, -3.0f);
    OT_EXPECT(glyph_scale(text) == 1.0f);

    OT_EXPECT(glyph_default_family(text) == GLYPH_FAMILY_SANS);
    glyph_set_default_family(text, GLYPH_FAMILY_SERIF);
    OT_EXPECT(glyph_default_family(text) == GLYPH_FAMILY_SERIF);
    glyph_set_default_family(text, GLYPH_FAMILY_MONO);
    OT_EXPECT(glyph_default_family(text) == GLYPH_FAMILY_MONO);
    /* DEFAULT and any out-of-range value resolve to the sans default. */
    glyph_set_default_family(text, GLYPH_FAMILY_DEFAULT);
    OT_EXPECT(glyph_default_family(text) == GLYPH_FAMILY_SANS);
    glyph_set_default_family(text, (glyph_family)99);
    OT_EXPECT(glyph_default_family(text) == GLYPH_FAMILY_SANS);

    /* A mono fallback is always available, so measure must return a positive
     * height even on a host with no shaping backend. */
    glyph_style style = {
        .size_px = 24.0f,
        .weight = 400.0f,
        .color = flux_color_rgba_premul(255, 255, 255, 255),
        .family = GLYPH_FAMILY_SANS,
    };
    glyph_metrics measured = glyph_measure(text, "A", 1, &style);
    OT_EXPECT(measured.width > 0.0f);
    OT_EXPECT(measured.height > 0.0f);
    OT_EXPECT(measured.baseline >= 0.0f && measured.baseline <= measured.height);

    /* Empty input is a value, not an error. */
    glyph_metrics none = glyph_measure(text, "", 0, &style);
    OT_EXPECT(none.width == 0.0f && none.height == 0.0f && none.baseline == 0.0f);
    none = glyph_measure(text, nullptr, 3, &style);
    OT_EXPECT(none.width == 0.0f);

    /* Compaction must not change the answer; it only releases scratch. */
    glyph_compact(text);
    glyph_metrics after_compact = glyph_measure(text, "A", 1, &style);
    OT_EXPECT_NEAR(after_compact.width, measured.width, 0.001f);
    OT_EXPECT_NEAR(after_compact.height, measured.height, 0.001f);

    /* Retain returns the same handle and keeps the context alive. */
    glyph_ctx *retained = glyph_retain(text);
    OT_EXPECT(retained == text);
    glyph_release(retained);
    after_compact = glyph_measure(text, "A", 1, &style);
    OT_EXPECT(after_compact.width > 0.0f);

    glyph_release(text);
    return OT_REPORT();
}
