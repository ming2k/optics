/*
 * flux_format ↔ VkFormat round-trip and edge cases.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

int main(void) {
    /* Every defined flux_format must round-trip through to_vk → from_vk. */
    static const flux_format all[] = {
        FLUX_FORMAT_UNDEFINED,     FLUX_FORMAT_R8_UNORM,      FLUX_FORMAT_RGBA8_UNORM,
        FLUX_FORMAT_BGRA8_UNORM,   FLUX_FORMAT_RGBA8_SRGB,    FLUX_FORMAT_BGRA8_SRGB,
        FLUX_FORMAT_RGBA16_SFLOAT, FLUX_FORMAT_D32_SFLOAT,    FLUX_FORMAT_D24_UNORM_S8,
        FLUX_FORMAT_D32_SFLOAT_S8, FLUX_FORMAT_R32_SFLOAT,    FLUX_FORMAT_RG32_SFLOAT,
        FLUX_FORMAT_RGB32_SFLOAT,  FLUX_FORMAT_RGBA32_SFLOAT,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        VkFormat vk = flux_format_to_vk(all[i]);
        if (all[i] == FLUX_FORMAT_UNDEFINED) {
            EXPECT(vk == VK_FORMAT_UNDEFINED);
        } else {
            EXPECT(vk != VK_FORMAT_UNDEFINED);
        }
        EXPECT(flux_format_from_vk(vk) == all[i]);
    }

    /* Unknown VkFormat → UNDEFINED. R32_UINT is valid Vulkan but
     * deliberately not exposed by flux_format. */
    EXPECT(flux_format_from_vk(VK_FORMAT_R32_UINT) == FLUX_FORMAT_UNDEFINED);

    /* Out-of-range flux_format value → UNDEFINED on to_vk. */
    EXPECT(flux_format_to_vk((flux_format)999) == VK_FORMAT_UNDEFINED);

    TEST_SUMMARY();
}
