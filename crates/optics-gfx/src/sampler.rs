//! Refcounted bindless texture samplers (`<optics/gfx/core.h>`).

use super::*;

/// Texture sampling filter.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Filter {
    Nearest,
    #[default]
    Linear,
}

/// Texture coordinate address mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum AddressMode {
    #[default]
    Repeat,
    ClampToEdge,
    MirroredRepeat,
    ClampToBorder,
}

/// Parameters for a refcounted bindless sampler.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SamplerDesc {
    pub min_filter: Filter,
    pub mag_filter: Filter,
    pub mipmap_mode: Filter,
    pub address_u: AddressMode,
    pub address_v: AddressMode,
    pub address_w: AddressMode,
    pub max_anisotropy: f32,
}

impl Default for SamplerDesc {
    fn default() -> Self {
        Self {
            min_filter: Filter::Linear,
            mag_filter: Filter::Linear,
            mipmap_mode: Filter::Linear,
            address_u: AddressMode::Repeat,
            address_v: AddressMode::Repeat,
            address_w: AddressMode::Repeat,
            max_anisotropy: 1.0,
        }
    }
}

/// Refcounted bindless texture sampler.
#[derive(Debug)]
pub struct Sampler {
    pub(crate) raw: *mut sys::optics_gfx_sampler,
}

impl Sampler {
    pub fn new(device: &Device, desc: SamplerDesc) -> Result<Self, Error> {
        fn filter(value: Filter) -> sys::optics_gfx_filter {
            match value {
                Filter::Nearest => sys::optics_gfx_filter::OPTICS_GFX_FILTER_NEAREST,
                Filter::Linear => sys::optics_gfx_filter::OPTICS_GFX_FILTER_LINEAR,
            }
        }
        fn address(value: AddressMode) -> sys::optics_gfx_address_mode {
            match value {
                AddressMode::Repeat => sys::optics_gfx_address_mode::OPTICS_GFX_ADDRESS_REPEAT,
                AddressMode::ClampToEdge => {
                    sys::optics_gfx_address_mode::OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE
                }
                AddressMode::MirroredRepeat => {
                    sys::optics_gfx_address_mode::OPTICS_GFX_ADDRESS_MIRRORED_REPEAT
                }
                AddressMode::ClampToBorder => {
                    sys::optics_gfx_address_mode::OPTICS_GFX_ADDRESS_CLAMP_TO_BORDER
                }
            }
        }
        let raw_desc = sys::optics_gfx_sampler_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SAMPLER_DESC,
            min_filter: filter(desc.min_filter),
            mag_filter: filter(desc.mag_filter),
            mipmap_mode: filter(desc.mipmap_mode),
            address_u: address(desc.address_u),
            address_v: address(desc.address_v),
            address_w: address(desc.address_w),
            max_anisotropy: desc.max_anisotropy,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_sampler_create(device.raw, &raw_desc, &mut raw) })?;
        Ok(Self { raw })
    }

    pub fn as_raw(&self) -> *mut sys::optics_gfx_sampler {
        self.raw
    }
}

impl Drop for Sampler {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_sampler_release(self.raw) };
    }
}

// SAFETY: optics_gfx_sampler is refcounted with atomic operations; creation and release
// are safe from any thread (docs/reference/thread-safety.md).
unsafe impl Send for Sampler {}
unsafe impl Sync for Sampler {}
