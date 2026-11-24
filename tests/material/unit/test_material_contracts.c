/*
 * test_material_contracts.c — CPU-only contracts for liboptics_material.
 *
 * Verifies without a Vulkan device:
 *   - version macros, packed integer, compatibility check, and string accessor;
 *   - optics_material_liquid_glass_max_shapes capability floor;
 *   - null-handle / invalid-argument safety across all five material filters
 *     (liquid_glass, frosted, acrylic, mica, backdrop_layer).
 */

#include "optics_test.h"

#include <optics/material.h>

#include <stdio.h>

static void test_version_contract(void) {
    int major = -1, minor = -1, patch = -1;
    optics_material_version(&major, &minor, &patch);
    OT_EXPECT(major == OPTICS_MATERIAL_VERSION_MAJOR);
    OT_EXPECT(minor == OPTICS_MATERIAL_VERSION_MINOR);
    OT_EXPECT(patch == OPTICS_MATERIAL_VERSION_PATCH);

    optics_material_version(nullptr, nullptr, nullptr);
    major = -1;
    optics_material_version(&major, nullptr, nullptr);
    OT_EXPECT(major == OPTICS_MATERIAL_VERSION_MAJOR);

    OT_EXPECT(optics_material_version_number() == OPTICS_MATERIAL_VERSION_NUMBER);
    OT_EXPECT(optics_material_version_check(OPTICS_MATERIAL_VERSION_MAJOR,
                                            OPTICS_MATERIAL_VERSION_MINOR,
                                            OPTICS_MATERIAL_VERSION_PATCH));
    OT_EXPECT(!optics_material_version_check(OPTICS_MATERIAL_VERSION_MAJOR + 1, 0, 0));
    OT_EXPECT(!optics_material_version_check(OPTICS_MATERIAL_VERSION_MAJOR,
                                             OPTICS_MATERIAL_VERSION_MINOR + 1, 0));
    OT_EXPECT(!optics_material_version_check(OPTICS_MATERIAL_VERSION_MAJOR,
                                             OPTICS_MATERIAL_VERSION_MINOR,
                                             OPTICS_MATERIAL_VERSION_PATCH + 1));

    char expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d", OPTICS_MATERIAL_VERSION_MAJOR,
             OPTICS_MATERIAL_VERSION_MINOR, OPTICS_MATERIAL_VERSION_PATCH);
    OT_EXPECT_STR_EQ(optics_material_version_string(), expected);
}

static void test_null_guards_all_filters(void) {
    OT_EXPECT(optics_material_liquid_glass_max_shapes() >= 2u);

    /* Liquid Glass */
    optics_material_liquid_glass_filter *lg = nullptr;
    OT_EXPECT(optics_material_liquid_glass_filter_create(nullptr, &lg) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_create(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_apply(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_stats(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_liquid_glass_filter_retain(nullptr) == nullptr);
    optics_material_liquid_glass_filter_release(nullptr);

    /* Frosted */
    optics_material_frosted_filter *fr = nullptr;
    OT_EXPECT(optics_material_frosted_filter_create(nullptr, &fr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_frosted_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_frosted_filter_apply(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_frosted_filter_retain(nullptr) == nullptr);
    optics_material_frosted_filter_release(nullptr);

    /* Acrylic */
    optics_material_acrylic_filter *ac = nullptr;
    OT_EXPECT(optics_material_acrylic_filter_create(nullptr, &ac) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_acrylic_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_acrylic_filter_apply(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_acrylic_filter_retain(nullptr) == nullptr);
    optics_material_acrylic_filter_release(nullptr);

    /* Mica */
    optics_material_mica_filter *mc = nullptr;
    OT_EXPECT(optics_material_mica_filter_create(nullptr, &mc) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_mica_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_mica_filter_apply(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_mica_filter_retain(nullptr) == nullptr);
    optics_material_mica_filter_release(nullptr);

    /* Backdrop Layer */
    optics_material_backdrop_layer_filter *bl = nullptr;
    OT_EXPECT(optics_material_backdrop_layer_filter_create(nullptr, &bl) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_backdrop_layer_filter_warmup(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_backdrop_layer_filter_apply(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_material_backdrop_layer_filter_retain(nullptr) == nullptr);
    optics_material_backdrop_layer_filter_release(nullptr);
}

int main(void) {
    test_version_contract();
    test_null_guards_all_filters();
    return OT_REPORT();
}
