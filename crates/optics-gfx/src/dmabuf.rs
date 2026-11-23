//! dma-buf capability queries.

use super::*;

/// Whether `device` supports dma-buf import (was created with the required
/// Vulkan extensions). A compositor checks this before advertising
/// `zwp_linux_dmabuf_v1` to clients.
pub fn dmabuf_supported(device: &Device) -> bool {
    unsafe { sys::optics_gfx_dmabuf_supported(device.raw) }
}

/// Whether this device can import/export Linux sync_file fences alongside
/// dma-bufs through `VK_KHR_external_semaphore_fd`.
pub fn dmabuf_sync_supported(device: &Device) -> bool {
    unsafe { sys::optics_gfx_dmabuf_sync_supported(device.raw) }
}

/// The single-plane DRM format modifiers a buffer of `format` may use to be
/// both sampleable by this device and importable as external dma-buf memory —
/// the set a compositor should advertise alongside the matching fourcc through
/// `zwp_linux_dmabuf_v1` so clients allocate GPU-optimal (tiled/compressed)
/// layouts instead of falling back to `DRM_FORMAT_MOD_LINEAR`.
///
/// Returns the modifiers on success. An unsupported format yields an empty
/// `Vec`; callers must omit it rather than synthesize an unreported fallback.
pub fn dmabuf_format_modifiers(device: &Device, format: Format) -> Vec<u64> {
    // Two-pass: probe the required length with count 0, then allocate and fill.
    let mut count: u32 = 0;
    let probe_opts = sys::optics_gfx_dmabuf_format_modifiers_opts {
        format: format.raw(),
        out_modifiers: std::ptr::null_mut(),
        inout_count: &mut count,
    };
    let rc = unsafe { sys::optics_gfx_dmabuf_format_modifiers(device.raw, &probe_opts) };
    // OPTICS_GFX_OK with count 0 (no qualifiers, or device lacks dma-buf) is the
    // common "nothing to advertise" path. An unsupported format is also mapped
    // to empty here so the caller's format loop is uniform.
    if rc != sys::optics_gfx_result::OPTICS_GFX_OK
        && rc != sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT
    {
        return Vec::new();
    }
    if count == 0 {
        return Vec::new();
    }
    let mut mods = vec![0u64; count as usize];
    let fill_opts = sys::optics_gfx_dmabuf_format_modifiers_opts {
        format: format.raw(),
        out_modifiers: mods.as_mut_ptr(),
        inout_count: &mut count,
    };
    let rc = unsafe { sys::optics_gfx_dmabuf_format_modifiers(device.raw, &fill_opts) };
    if rc != sys::optics_gfx_result::OPTICS_GFX_OK {
        return Vec::new();
    }
    mods.truncate(count as usize);
    mods
}

/// Low-level Vulkan device extensions used by DMA-BUF import. New callers
/// should request [`DeviceFeatures::DMABUF`] through [`DeviceOptions`]; this
/// list remains available for compatibility and unusual Vulkan interop.
pub const DMABUF_DEVICE_EXTENSIONS: [&std::ffi::CStr; 5] = [
    c"VK_KHR_external_memory",
    c"VK_KHR_external_memory_fd",
    c"VK_EXT_external_memory_dma_buf",
    c"VK_EXT_image_drm_format_modifier",
    c"VK_EXT_queue_family_foreign",
];

/// Low-level Vulkan device extension used for Linux sync_file interop. New
/// callers should request [`DeviceFeatures::DMABUF_SYNC_FILE`].
pub const DMABUF_SYNC_DEVICE_EXTENSIONS: [&std::ffi::CStr; 1] = [c"VK_KHR_external_semaphore_fd"];
