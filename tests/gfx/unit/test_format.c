/*
 * flux_format ↔ VkFormat round-trip and edge cases.
 */
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

int main(void) {
    /* Every defined flux_format must round-trip through to_vk → from_vk. */
    static const optics_gfx_format all[] = {
        OPTICS_GFX_FORMAT_UNDEFINED,     OPTICS_GFX_FORMAT_R8_UNORM,
        OPTICS_GFX_FORMAT_RGBA8_UNORM,   OPTICS_GFX_FORMAT_BGRA8_UNORM,
        OPTICS_GFX_FORMAT_RGBA8_SRGB,    OPTICS_GFX_FORMAT_BGRA8_SRGB,
        OPTICS_GFX_FORMAT_RGBA16_SFLOAT, OPTICS_GFX_FORMAT_D32_SFLOAT,
        OPTICS_GFX_FORMAT_D24_UNORM_S8,  OPTICS_GFX_FORMAT_D32_SFLOAT_S8,
        OPTICS_GFX_FORMAT_R32_SFLOAT,    OPTICS_GFX_FORMAT_RG32_SFLOAT,
        OPTICS_GFX_FORMAT_RGB32_SFLOAT,  OPTICS_GFX_FORMAT_RGBA32_SFLOAT,
    };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        VkFormat vk = optics_gfx_format_to_vk(all[i]);
        if (all[i] == OPTICS_GFX_FORMAT_UNDEFINED) {
            OT_EXPECT(vk == VK_FORMAT_UNDEFINED);
        } else {
            OT_EXPECT(vk != VK_FORMAT_UNDEFINED);
        }
        OT_EXPECT(optics_gfx_format_from_vk(vk) == all[i]);
    }

    /* Unknown VkFormat → UNDEFINED. R32_UINT is valid Vulkan but
     * deliberately not exposed by flux_format. */
    OT_EXPECT(optics_gfx_format_from_vk(VK_FORMAT_R32_UINT) == OPTICS_GFX_FORMAT_UNDEFINED);

    /* Out-of-range flux_format value → UNDEFINED on to_vk. */
    OT_EXPECT(optics_gfx_format_to_vk((optics_gfx_format)999) == VK_FORMAT_UNDEFINED);

    return OT_REPORT();
}
