/*
 * glyph_lifecycle — the public contract that does not need a font.
 *
 * Version accessors, retain/release, scale and default-family policy,
 * compaction, and null tolerance. These are exactly the edges a binding or a
 * host touches before it can shape a single run, so they must not depend on
 * Fontconfig, FreeType or a Vulkan device.
 */

#include "optics_test.h"

#include <optics/text.h>

#include <string.h>

static optics_text_metrics measure(optics_text_ctx *t, const char *s, size_t len,
                                   const optics_text_style *style) {
    return optics_text_measure(t,
                               &(optics_text_measure_opts){.utf8 = s, .len = len, .style = style});
}

int main(void) {
    /* ---- Version surface: macros, accessors, packed number, string ---- */
    int major = -1, minor = -1, patch = -1;
    optics_text_version(&major, &minor, &patch);
    OT_EXPECT(major == OPTICS_TEXT_VERSION_MAJOR);
    OT_EXPECT(minor == OPTICS_TEXT_VERSION_MINOR);
    OT_EXPECT(patch == OPTICS_TEXT_VERSION_PATCH);
    OT_EXPECT(optics_text_version_number() == OPTICS_TEXT_VERSION_NUMBER);
    OT_EXPECT(optics_text_version_number() ==
              ((uint32_t)OPTICS_TEXT_VERSION_MAJOR << 16 |
               (uint32_t)OPTICS_TEXT_VERSION_MINOR << 8 | (uint32_t)OPTICS_TEXT_VERSION_PATCH));
    OT_EXPECT(optics_text_version_check(OPTICS_TEXT_VERSION_MAJOR, OPTICS_TEXT_VERSION_MINOR,
                                        OPTICS_TEXT_VERSION_PATCH));
    OT_EXPECT(!optics_text_version_check(OPTICS_TEXT_VERSION_MAJOR + 1, 0, 0));
    OT_EXPECT(
        !optics_text_version_check(OPTICS_TEXT_VERSION_MAJOR, OPTICS_TEXT_VERSION_MINOR + 1, 0));
    OT_EXPECT(!optics_text_version_check(OPTICS_TEXT_VERSION_MAJOR, OPTICS_TEXT_VERSION_MINOR,
                                         OPTICS_TEXT_VERSION_PATCH + 1));

    char expected[32];
    snprintf(expected, sizeof expected, "%d.%d.%d", OPTICS_TEXT_VERSION_MAJOR,
             OPTICS_TEXT_VERSION_MINOR, OPTICS_TEXT_VERSION_PATCH);
    OT_EXPECT_STR_EQ(optics_text_version_string(), expected);

    /* Missing out-parameters are tolerated independently. */
    optics_text_version(nullptr, nullptr, nullptr);
    optics_text_version(&major, nullptr, nullptr);
    OT_EXPECT(major == OPTICS_TEXT_VERSION_MAJOR);

    /* ---- Null context policy ---- */
    OT_EXPECT(optics_text_scale(nullptr) == 1.0f);
    OT_EXPECT(optics_text_default_family(nullptr) == OPTICS_TEXT_FAMILY_SANS);
    optics_text_set_scale(nullptr, 4.0f);
    optics_text_set_default_family(nullptr, OPTICS_TEXT_FAMILY_MONO);
    optics_text_compact(nullptr);
    optics_text_release(nullptr);

    optics_text_metrics empty = measure(nullptr, "A", 1, nullptr);
    OT_EXPECT(empty.width == 0.0f && empty.height == 0.0f && empty.baseline == 0.0f);

    /* ---- Context lifecycle ---- */
    optics_text_ctx *text = nullptr;
    OT_REQUIRE(optics_text_create(&(optics_text_desc){.device = nullptr, .scale = 1.0f}, &text) ==
               OPTICS_GFX_OK);
    OT_REQUIRE(text != nullptr);

    OT_EXPECT(optics_text_scale(text) == 1.0f);
    optics_text_set_scale(text, 2.0f);
    OT_EXPECT(optics_text_scale(text) == 2.0f);
    optics_text_set_scale(text, 0.0f);
    OT_EXPECT(optics_text_scale(text) == 1.0f);
    optics_text_set_scale(text, -3.0f);
    OT_EXPECT(optics_text_scale(text) == 1.0f);

    OT_EXPECT(optics_text_default_family(text) == OPTICS_TEXT_FAMILY_SANS);
    optics_text_set_default_family(text, OPTICS_TEXT_FAMILY_SERIF);
    OT_EXPECT(optics_text_default_family(text) == OPTICS_TEXT_FAMILY_SERIF);
    optics_text_set_default_family(text, OPTICS_TEXT_FAMILY_MONO);
    OT_EXPECT(optics_text_default_family(text) == OPTICS_TEXT_FAMILY_MONO);
    /* DEFAULT and any out-of-range value resolve to the sans default. */
    optics_text_set_default_family(text, OPTICS_TEXT_FAMILY_DEFAULT);
    OT_EXPECT(optics_text_default_family(text) == OPTICS_TEXT_FAMILY_SANS);
    optics_text_set_default_family(text, (optics_text_family)99);
    OT_EXPECT(optics_text_default_family(text) == OPTICS_TEXT_FAMILY_SANS);

    /* A mono fallback is always available, so measure must return a positive
     * height even on a host with no shaping backend. */
    optics_text_style style = {
        .size_px = 24.0f,
        .weight = 400.0f,
        .color = optics_gfx_color_rgba_premul(255, 255, 255, 255),
        .family = OPTICS_TEXT_FAMILY_SANS,
    };
    optics_text_metrics measured = measure(text, "A", 1, &style);
    OT_EXPECT(measured.width > 0.0f);
    OT_EXPECT(measured.height > 0.0f);
    OT_EXPECT(measured.baseline >= 0.0f && measured.baseline <= measured.height);

    /* Empty input is a value, not an error. */
    optics_text_metrics none = measure(text, "", 0, &style);
    OT_EXPECT(none.width == 0.0f && none.height == 0.0f && none.baseline == 0.0f);
    none = measure(text, nullptr, 3, &style);
    OT_EXPECT(none.width == 0.0f);

    /* Compaction must not change the answer; it only releases scratch. */
    optics_text_compact(text);
    optics_text_metrics after_compact = measure(text, "A", 1, &style);
    OT_EXPECT_NEAR(after_compact.width, measured.width, 0.001f);
    OT_EXPECT_NEAR(after_compact.height, measured.height, 0.001f);

    /* Retain returns the same handle and keeps the context alive. */
    optics_text_ctx *retained = optics_text_retain(text);
    OT_EXPECT(retained == text);
    optics_text_release(retained);
    after_compact = measure(text, "A", 1, &style);
    OT_EXPECT(after_compact.width > 0.0f);

    optics_text_release(text);
    return OT_REPORT();
}
