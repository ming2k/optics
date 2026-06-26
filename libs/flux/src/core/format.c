/*
 * flux_format ↔ VkFormat mapping. Lives in core so canvas / scene /
 * any module can translate without each owning the table.
 */
#include <flux/vulkan.h>

VkFormat flux_format_to_vk(flux_format f) {
    switch (f) {
    case FLUX_FORMAT_UNDEFINED:
        return VK_FORMAT_UNDEFINED;
    case FLUX_FORMAT_R8_UNORM:
        return VK_FORMAT_R8_UNORM;
    case FLUX_FORMAT_RGBA8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case FLUX_FORMAT_BGRA8_UNORM:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case FLUX_FORMAT_RGBA8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case FLUX_FORMAT_BGRA8_SRGB:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case FLUX_FORMAT_RGBA16_SFLOAT:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FLUX_FORMAT_D32_SFLOAT:
        return VK_FORMAT_D32_SFLOAT;
    case FLUX_FORMAT_D24_UNORM_S8:
        return VK_FORMAT_D24_UNORM_S8_UINT;
    case FLUX_FORMAT_D32_SFLOAT_S8:
        return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case FLUX_FORMAT_R32_SFLOAT:
        return VK_FORMAT_R32_SFLOAT;
    case FLUX_FORMAT_RG32_SFLOAT:
        return VK_FORMAT_R32G32_SFLOAT;
    case FLUX_FORMAT_RGB32_SFLOAT:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case FLUX_FORMAT_RGBA32_SFLOAT:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

flux_format flux_format_from_vk(VkFormat vf) {
    switch (vf) {
    case VK_FORMAT_R8_UNORM:
        return FLUX_FORMAT_R8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM:
        return FLUX_FORMAT_RGBA8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return FLUX_FORMAT_BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return FLUX_FORMAT_RGBA8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return FLUX_FORMAT_BGRA8_SRGB;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return FLUX_FORMAT_RGBA16_SFLOAT;
    case VK_FORMAT_D32_SFLOAT:
        return FLUX_FORMAT_D32_SFLOAT;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        return FLUX_FORMAT_D24_UNORM_S8;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return FLUX_FORMAT_D32_SFLOAT_S8;
    case VK_FORMAT_R32_SFLOAT:
        return FLUX_FORMAT_R32_SFLOAT;
    case VK_FORMAT_R32G32_SFLOAT:
        return FLUX_FORMAT_RG32_SFLOAT;
    case VK_FORMAT_R32G32B32_SFLOAT:
        return FLUX_FORMAT_RGB32_SFLOAT;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return FLUX_FORMAT_RGBA32_SFLOAT;
    default:
        return FLUX_FORMAT_UNDEFINED;
    }
}
