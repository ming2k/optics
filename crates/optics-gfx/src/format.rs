//! Texel formats.

use super::*;

/// Backend-neutral pixel format, re-exported from the raw bindings.
/// Image/attachment texel format. The idiomatic mirror of the C
/// `optics_gfx_format` enum; convert at the FFI edge (the conversion helpers are crate-private by
/// design; public signatures speak [`Format`], never the raw enum).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Format {
    Undefined,
    R8Unorm,
    Rgba8Unorm,
    Bgra8Unorm,
    Rgba8Srgb,
    Bgra8Srgb,
    Rgba16Sfloat,
    D32Sfloat,
    D24UnormS8,
    Rgb10a2Unorm,
}

impl Format {
    /// Convert to the underlying C `optics_gfx_format` value.
    pub fn as_raw(self) -> sys::optics_gfx_format {
        self.raw()
    }

    pub(crate) fn raw(self) -> sys::optics_gfx_format {
        use sys::optics_gfx_format as f;
        match self {
            Format::Undefined => f::OPTICS_GFX_FORMAT_UNDEFINED,
            Format::R8Unorm => f::OPTICS_GFX_FORMAT_R8_UNORM,
            Format::Rgba8Unorm => f::OPTICS_GFX_FORMAT_RGBA8_UNORM,
            Format::Bgra8Unorm => f::OPTICS_GFX_FORMAT_BGRA8_UNORM,
            Format::Rgba8Srgb => f::OPTICS_GFX_FORMAT_RGBA8_SRGB,
            Format::Bgra8Srgb => f::OPTICS_GFX_FORMAT_BGRA8_SRGB,
            Format::Rgba16Sfloat => f::OPTICS_GFX_FORMAT_RGBA16_SFLOAT,
            Format::D32Sfloat => f::OPTICS_GFX_FORMAT_D32_SFLOAT,
            Format::D24UnormS8 => f::OPTICS_GFX_FORMAT_D24_UNORM_S8,
            Format::Rgb10a2Unorm => f::OPTICS_GFX_FORMAT_RGB10A2_UNORM,
        }
    }

    pub(crate) fn from_raw(raw: sys::optics_gfx_format) -> Format {
        use sys::optics_gfx_format as f;
        match raw {
            f::OPTICS_GFX_FORMAT_R8_UNORM => Format::R8Unorm,
            f::OPTICS_GFX_FORMAT_RGBA8_UNORM => Format::Rgba8Unorm,
            f::OPTICS_GFX_FORMAT_BGRA8_UNORM => Format::Bgra8Unorm,
            f::OPTICS_GFX_FORMAT_RGBA8_SRGB => Format::Rgba8Srgb,
            f::OPTICS_GFX_FORMAT_BGRA8_SRGB => Format::Bgra8Srgb,
            f::OPTICS_GFX_FORMAT_RGBA16_SFLOAT => Format::Rgba16Sfloat,
            f::OPTICS_GFX_FORMAT_D32_SFLOAT => Format::D32Sfloat,
            f::OPTICS_GFX_FORMAT_D24_UNORM_S8 => Format::D24UnormS8,
            f::OPTICS_GFX_FORMAT_RGB10A2_UNORM => Format::Rgb10a2Unorm,
            _ => Format::Undefined,
        }
    }
}
