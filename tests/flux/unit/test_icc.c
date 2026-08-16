/*
 * ICC parser tests (ADR-0070), profiles synthesized via icc_testkit.h:
 *
 *   - matrix + sRGB-parametric-TRC profile extracts exactly onto
 *     FLUX_COLOR_SPACE_SRGB;
 *   - matrix + gamma-TRC profile extracts BT.709 + FLUX_TRANSFER_GAMMA;
 *   - a table-TRC matrix profile falls to the LUT bake;
 *   - an mft1 LUT profile bakes a LUT (parametric query declines);
 *   - malformed inputs fail with the right error codes.
 */
#include "icc_testkit.h"
#include "test_helpers.h"
#include <flux/flux.h>

#include <string.h>

static bool feq(float a, float b, float eps) {
    float d = a - b;
    if (d < 0)
        d = -d;
    return d <= eps;
}

int main(void) {
    uint8_t profile[16384];
    uint8_t data[8192];
    icc_tag_def defs[8];
    size_t dlen = 0;

    /* --- 1. matrix + sRGB parametric TRC -> FLUX_COLOR_SPACE_SRGB --- */
    {
        uint32_t n = icc_build_matrix_tags(data, defs, 0, &dlen);
        size_t size = icc_build_profile(profile, "mntr", "XYZ ", data, dlen, defs, n);
        flux_icc_profile *p = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &p) == FLUX_OK);
        flux_color_space cs;
        EXPECT(flux_icc_profile_color_space(p, &cs));
        EXPECT(flux_color_space_equal(cs, (flux_color_space)FLUX_COLOR_SPACE_SRGB));
        flux_icc_profile_release(p);
    }

    /* --- 2. matrix + gamma 2.2 TRC -> BT709 + GAMMA --- */
    {
        uint32_t n = icc_build_matrix_tags(data, defs, 1, &dlen);
        size_t size = icc_build_profile(profile, "mntr", "XYZ ", data, dlen, defs, n);
        flux_icc_profile *p = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &p) == FLUX_OK);
        flux_color_space cs;
        EXPECT(flux_icc_profile_color_space(p, &cs));
        EXPECT(cs.primaries == FLUX_PRIMARIES_BT709);
        EXPECT(cs.transfer == FLUX_TRANSFER_GAMMA);
        EXPECT(feq(cs.gamma, 2.19921875f, 1e-4f));
        flux_icc_profile_release(p);
    }

    /* --- 3. table TRC -> not parametric (LUT bake path) --- */
    {
        uint32_t n = icc_build_matrix_tags(data, defs, 2, &dlen);
        size_t size = icc_build_profile(profile, "mntr", "XYZ ", data, dlen, defs, n);
        flux_icc_profile *p = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &p) == FLUX_OK);
        flux_color_space cs;
        EXPECT(!flux_icc_profile_color_space(p, &cs));
        flux_icc_profile_release(p);
    }

    /* --- 4. mft1 constant-white CLUT -> LUT, not parametric --- */
    {
        size_t len = icc_build_mft1_constant_white(data, &defs[0]);
        size_t size = icc_build_profile(profile, "mntr", "XYZ ", data, len, defs, 1);
        flux_icc_profile *p = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &p) == FLUX_OK);
        flux_color_space cs;
        EXPECT(!flux_icc_profile_color_space(p, &cs));
        flux_icc_profile_release(p);
    }

    /* --- 5. malformed inputs --- */
    {
        flux_icc_profile *p = nullptr;
        EXPECT(flux_icc_profile_create(nullptr, 0, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(flux_icc_profile_create(profile, 64, &p) == FLUX_ERROR_INVALID_ARGUMENT);
        uint8_t bad_sig[256];
        memset(bad_sig, 0, sizeof(bad_sig));
        EXPECT(flux_icc_profile_create(bad_sig, sizeof(bad_sig), &p) ==
               FLUX_ERROR_INVALID_ARGUMENT);
        /* Wrong device class (printer) -> UNSUPPORTED. */
        uint32_t n = icc_build_matrix_tags(data, defs, 0, &dlen);
        size_t size = icc_build_profile(profile, "prtr", "XYZ ", data, dlen, defs, n);
        EXPECT(flux_icc_profile_create(profile, size, &p) == FLUX_ERROR_UNSUPPORTED);
        /* Truncated: declared size exceeds the buffer. */
        size = icc_build_profile(profile, "mntr", "XYZ ", data, dlen, defs, n);
        EXPECT(flux_icc_profile_create(profile, size / 2, &p) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    TEST_SUMMARY();
}
