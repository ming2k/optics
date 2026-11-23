/*
 * optics_gfx_format ↔ VkFormat mapping. Lives in core so canvas / scene /
 * any module can translate without each owning the table.
 */
#include <optics/gfx/vulkan.h>

VkFormat optics_gfx_format_to_vk(optics_gfx_format f) {
    switch (f) {
    case OPTICS_GFX_FORMAT_UNDEFINED:
        return VK_FORMAT_UNDEFINED;
    case OPTICS_GFX_FORMAT_R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case OPTICS_GFX_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case OPTICS_GFX_FORMAT_BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case OPTICS_GFX_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case OPTICS_GFX_FORMAT_BGRA8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case OPTICS_GFX_FORMAT_RGB10A2_UNORM:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case OPTICS_GFX_FORMAT_RGBA16_SFLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case OPTICS_GFX_FORMAT_D32_SFLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case OPTICS_GFX_FORMAT_D24_UNORM_S8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case OPTICS_GFX_FORMAT_D32_SFLOAT_S8:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case OPTICS_GFX_FORMAT_R32_SFLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case OPTICS_GFX_FORMAT_RG32_SFLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case OPTICS_GFX_FORMAT_RGB32_SFLOAT:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case OPTICS_GFX_FORMAT_RGBA32_SFLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

optics_gfx_format optics_gfx_format_from_vk(VkFormat vf) {
    switch (vf) {
    case VK_FORMAT_R8_UNORM:
        return OPTICS_GFX_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return OPTICS_GFX_FORMAT_RGBA8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return OPTICS_GFX_FORMAT_BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return OPTICS_GFX_FORMAT_RGBA8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return OPTICS_GFX_FORMAT_BGRA8_SRGB;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return OPTICS_GFX_FORMAT_RGB10A2_UNORM;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return OPTICS_GFX_FORMAT_RGBA16_SFLOAT;
    case VK_FORMAT_D32_SFLOAT:
        return OPTICS_GFX_FORMAT_D32_SFLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return OPTICS_GFX_FORMAT_D24_UNORM_S8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return OPTICS_GFX_FORMAT_D32_SFLOAT_S8;
    case VK_FORMAT_R32_SFLOAT:
        return OPTICS_GFX_FORMAT_R32_SFLOAT;
    case VK_FORMAT_R32G32_SFLOAT:
        return OPTICS_GFX_FORMAT_RG32_SFLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT:
        return OPTICS_GFX_FORMAT_RGB32_SFLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return OPTICS_GFX_FORMAT_RGBA32_SFLOAT;
    default:
        return OPTICS_GFX_FORMAT_UNDEFINED;
    }
}
