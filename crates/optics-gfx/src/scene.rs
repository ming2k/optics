//! 3D scene passes, cameras, lights, materials and samplers.

use super::*;

/// How a scene pass treats the existing surface color.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum SceneColorLoad {
    /// Preserve color recorded by an earlier pass in the same frame.
    Load,
    /// Clear the surface to linear RGBA before drawing the scene.
    Clear([f32; 4]),
}

/// Active depth-tested scene pass. Dropping the guard ends the pass.
pub struct ScenePass<'frame, 'surface> {
    pub(crate) frame: &'frame mut Frame<'surface>,
    pub(crate) active: bool,
    pub(crate) target: *mut sys::optics_gfx_image,
}

impl ScenePass<'_, '_> {
    /// Raw frame pointer for sibling content libraries such as
    /// `gfx-scene-graph`. The pass remains owned by this guard.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_frame {
        self.frame.raw
    }

    /// End the pass explicitly. Dropping the guard has the same effect.
    pub fn end(mut self) {
        self.finish();
    }

    fn finish(&mut self) {
        if !self.active {
            return;
        }
        unsafe {
            sys::optics_gfx_frame_end_pass(self.frame.raw);
            if !self.target.is_null() {
                let _ = sys::optics_gfx_frame_finish_image_target(self.frame.raw, self.target);
            }
        }
        self.active = false;
    }
}

impl Drop for ScenePass<'_, '_> {
    fn drop(&mut self) {
        self.finish();
    }
}

/// Perspective camera used by gfx scene renderers.
pub struct Camera {
    raw: sys::optics_gfx_camera,
}

impl Camera {
    /// Perspective projection (right-handed, depth 0..1 — Vulkan clip).
    pub fn perspective(fov_y_rad: f32, aspect: f32, z_near: f32, z_far: f32) -> Camera {
        let mut raw = sys::optics_gfx_camera::default();
        let opts = sys::optics_gfx_camera_perspective_opts {
            fov_y_rad,
            aspect,
            z_near,
            z_far,
        };
        unsafe { sys::optics_gfx_camera_perspective(&mut raw, &opts) };
        Camera { raw }
    }

    /// Position the camera at `eye` looking at `center` with `up` as the
    /// world-space up vector.
    pub fn look_at(&mut self, eye: [f32; 3], center: [f32; 3], up: [f32; 3]) {
        unsafe { sys::optics_gfx_camera_look_at(&mut self.raw, vec3(eye), vec3(center), vec3(up)) };
    }

    /// Raw `optics_gfx_camera*` (immutable view) for FFI edges.
    pub fn as_raw(&self) -> *const sys::optics_gfx_camera {
        &self.raw
    }
}

/// One directional light in world space.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SceneLight {
    pub direction: [f32; 3],
    pub color: [f32; 3],
    pub ambient: f32,
}

impl Default for SceneLight {
    fn default() -> Self {
        Self {
            direction: [-0.4, -0.8, -0.45],
            color: [1.0, 1.0, 1.0],
            ambient: 0.08,
        }
    }
}

impl SceneLight {
    pub fn as_raw(&self) -> sys::optics_gfx_scene_light {
        sys::optics_gfx_scene_light {
            direction: vec3(self.direction),
            color: vec3(self.color),
            ambient: self.ambient,
        }
    }
}

/// Built-in material pipeline kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MaterialKind {
    Unlit,
    Phong,
    /// ADR-0117: Perspective-correct analytic vector coverage.
    Vector,
}

/// Alpha interpretation and fixed-function blending for a scene material.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MaterialAlphaMode {
    #[default]
    Opaque,
    Mask,
    Blend,
}

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

/// Base-colour texture and UV transform borrowed during material creation.
#[derive(Clone, Copy)]
pub struct MaterialTexture<'a> {
    pub image: &'a Image,
    pub sampler: Option<&'a Sampler>,
    pub uv_offset: [f32; 2],
    pub uv_scale: [f32; 2],
    pub uv_rotation: f32,
}

/// Optional surface state layered onto a built-in material.
#[derive(Clone, Copy)]
pub struct MaterialOptions<'a> {
    pub base_color_texture: Option<MaterialTexture<'a>>,
    pub alpha_mode: MaterialAlphaMode,
    pub alpha_cutoff: f32,
    pub double_sided: bool,
}

impl Default for MaterialOptions<'_> {
    fn default() -> Self {
        Self {
            base_color_texture: None,
            alpha_mode: MaterialAlphaMode::Opaque,
            alpha_cutoff: 0.5,
            double_sided: false,
        }
    }
}

/// Parameters for a built-in scene material.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct MaterialDesc {
    pub kind: MaterialKind,
    pub base_color: [f32; 4],
    pub color_format: Format,
    pub depth_format: Format,
    pub shininess: f32,
    pub specular: f32,
}

/// GPU storage or vertex buffer with device address capabilities.
pub struct GpuBuffer {
    pub(crate) raw: *mut sys::optics_gfx_buffer,
}

impl GpuBuffer {
    /// Create a storage buffer from a raw byte slice.
    pub fn new_storage(device: &Device, data: &[u8]) -> Result<Self, Error> {
        let desc = sys::optics_gfx_buffer_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_BUFFER_DESC,
            next: std::ptr::null(),
            size: data.len(),
            usage: (sys::optics_gfx_buffer_usage::OPTICS_GFX_BUFFER_USAGE_STORAGE as u32)
                | (sys::optics_gfx_buffer_usage::OPTICS_GFX_BUFFER_USAGE_TRANSFER_SRC as u32),
            location: sys::optics_gfx_buffer_location::OPTICS_GFX_BUFFER_HOST_VISIBLE,
            device_address: true,
            initial_data: data.as_ptr() as *const std::ffi::c_void,
        };
        let mut raw = std::ptr::null_mut();
        let res = unsafe { sys::optics_gfx_buffer_create(device.raw, &desc, &mut raw) };
        if res != sys::optics_gfx_result::OPTICS_GFX_OK {
            return Err(Error::from_raw(res));
        }
        Ok(Self { raw })
    }

    /// 64-bit GPU virtual address of this buffer.
    pub fn device_address(&self) -> u64 {
        unsafe { sys::optics_gfx_buffer_device_address(self.raw) }
    }
}

impl Drop for GpuBuffer {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::optics_gfx_buffer_release(self.raw) };
        }
    }
}

unsafe impl Send for GpuBuffer {}
unsafe impl Sync for GpuBuffer {}

/// Refcounted built-in scene material.
pub struct Material {
    raw: *mut sys::optics_gfx_material,
}

impl Material {
    pub fn new(device: &Device, desc: MaterialDesc) -> Result<Material, Error> {
        Self::new_impl(device, desc, None)
    }

    pub fn new_with_options(
        device: &Device,
        desc: MaterialDesc,
        options: MaterialOptions<'_>,
    ) -> Result<Material, Error> {
        Self::new_impl(device, desc, Some(options))
    }

    /// Create an ADR-0117 perspective-correct vector coverage material.
    pub fn new_vector(
        device: &Device,
        base_color: [f32; 4],
        color_format: Format,
        depth_format: Format,
        segments_buffer: &GpuBuffer,
        segment_count: u32,
        fill_rule: FillRule,
    ) -> Result<Material, Error> {
        let raw_vec_desc = sys::optics_gfx_material_vector_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_MATERIAL_VECTOR_DESC,
            next: std::ptr::null(),
            segments_buffer: segments_buffer.raw,
            segment_count,
            fill_rule: fill_rule.raw() as u32,
        };

        let raw_desc = sys::optics_gfx_material_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_MATERIAL_DESC,
            next: &raw_vec_desc as *const _ as *const std::ffi::c_void,
            kind: sys::optics_gfx_material_kind::OPTICS_GFX_MATERIAL_VECTOR,
            base_color: sys::optics_gfx_vec4 {
                x: base_color[0],
                y: base_color[1],
                z: base_color[2],
                w: base_color[3],
            },
            color_format: color_format.raw(),
            depth_format: depth_format.raw(),
            shininess: 0.0,
            specular: 0.0,
        };

        let mut raw = std::ptr::null_mut();
        let res = unsafe { sys::optics_gfx_material_create(device.raw, &raw_desc, &mut raw) };
        if res != sys::optics_gfx_result::OPTICS_GFX_OK {
            return Err(Error::from_raw(res));
        }

        Ok(Material { raw })
    }

    fn new_impl(
        device: &Device,
        desc: MaterialDesc,
        options: Option<MaterialOptions<'_>>,
    ) -> Result<Material, Error> {
        let kind = match desc.kind {
            MaterialKind::Unlit => sys::optics_gfx_material_kind::OPTICS_GFX_MATERIAL_UNLIT,
            MaterialKind::Phong => sys::optics_gfx_material_kind::OPTICS_GFX_MATERIAL_PHONG,
            MaterialKind::Vector => sys::optics_gfx_material_kind::OPTICS_GFX_MATERIAL_VECTOR,
        };
        let raw_surface = options.map(|options| {
            let texture = options.base_color_texture;
            let alpha_mode = match options.alpha_mode {
                MaterialAlphaMode::Opaque => {
                    sys::optics_gfx_material_alpha_mode::OPTICS_GFX_MATERIAL_ALPHA_OPAQUE
                }
                MaterialAlphaMode::Mask => {
                    sys::optics_gfx_material_alpha_mode::OPTICS_GFX_MATERIAL_ALPHA_MASK
                }
                MaterialAlphaMode::Blend => {
                    sys::optics_gfx_material_alpha_mode::OPTICS_GFX_MATERIAL_ALPHA_BLEND
                }
            };
            sys::optics_gfx_material_surface_desc {
                type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_MATERIAL_SURFACE_DESC,
                base_color_image: texture
                    .map(|value| value.image.as_raw())
                    .unwrap_or(std::ptr::null_mut()),
                base_color_sampler: texture
                    .and_then(|value| value.sampler)
                    .map(Sampler::as_raw)
                    .unwrap_or(std::ptr::null_mut()),
                uv_offset: vec2(texture.map(|value| value.uv_offset).unwrap_or([0.0, 0.0])),
                uv_scale: vec2(texture.map(|value| value.uv_scale).unwrap_or([1.0, 1.0])),
                uv_rotation: texture.map(|value| value.uv_rotation).unwrap_or(0.0),
                alpha_mode,
                alpha_cutoff: options.alpha_cutoff,
                double_sided: options.double_sided,
                ..Default::default()
            }
        });
        let raw_desc = sys::optics_gfx_material_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_MATERIAL_DESC,
            next: raw_surface
                .as_ref()
                .map(|value| value as *const _ as *const std::ffi::c_void)
                .unwrap_or(std::ptr::null()),
            kind,
            base_color: vec4(desc.base_color),
            color_format: desc.color_format.raw(),
            depth_format: desc.depth_format.raw(),
            shininess: desc.shininess,
            specular: desc.specular,
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_material_create(device.raw, &raw_desc, &mut raw) })?;
        Ok(Material { raw })
    }

    pub fn as_raw(&self) -> *mut sys::optics_gfx_material {
        self.raw
    }
}

/// Refcounted bindless texture sampler.
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

impl Drop for Material {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_material_release(self.raw) };
    }
}

// SAFETY: optics_gfx_material is immutable after creation and refcounted atomically (docs/reference/thread-safety.md).
unsafe impl Send for Material {}
unsafe impl Sync for Material {}
