//! Safe Rust bindings to the `flux-scene-graph` glTF content layer.
//!
//! [`Scene`] parses an in-memory binary glTF (`.glb`), owns the uploaded mesh
//! resources, reports world-space bounds for automatic framing, samples glTF
//! and VRM Animation clips, and records static or GPU-skinned draws into an
//! active [`flux::ScenePass`].

#![deny(rust_2018_idioms)]

use std::fmt;

use flux::{Camera, Device, Format, Material, SceneLight, ScenePass};
use flux_scene_graph_sys as sys;

mod materials;

/// A flux result code surfaced as a Rust error — the SAME type as
/// [`flux::Error`], re-exported so callers need one fewer import. GLB-load
/// specific context lives in [`LoadError`], not in a second result-code
/// error type.
pub use flux::Error;

pub(crate) fn check(rc: sys::flux_result) -> Result<(), Error> {
    Error::check_raw(rc)
}

/// Render-target formats used to construct a GLB's per-primitive materials.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct MaterialTarget {
    pub color_format: Format,
    pub depth_format: Format,
}

/// Failure while validating a GLB or constructing its GPU material resources.
#[derive(Debug)]
pub enum LoadError {
    Scene(Error),
    Gltf(gltf::Error),
    Image(image::ImageError),
    Flux(flux::Error),
    Unsupported(String),
}

impl fmt::Display for LoadError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Scene(error) => error.fmt(f),
            Self::Gltf(error) => write!(f, "invalid glTF: {error}"),
            Self::Image(error) => write!(f, "failed to decode glTF image: {error}"),
            Self::Flux(error) => error.fmt(f),
            Self::Unsupported(message) => write!(f, "unsupported glTF material: {message}"),
        }
    }
}

impl std::error::Error for LoadError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Scene(error) => Some(error),
            Self::Gltf(error) => Some(error),
            Self::Image(error) => Some(error),
            Self::Flux(error) => Some(error),
            Self::Unsupported(_) => None,
        }
    }
}

// (Error is now a re-export of flux::Error; the From impls below use the
// concrete crate paths directly.)

impl From<gltf::Error> for LoadError {
    fn from(value: gltf::Error) -> Self {
        Self::Gltf(value)
    }
}

impl From<image::ImageError> for LoadError {
    fn from(value: image::ImageError) -> Self {
        Self::Image(value)
    }
}

impl From<flux::Error> for LoadError {
    fn from(value: flux::Error) -> Self {
        Self::Flux(value)
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bounds {
    pub min: [f32; 3],
    pub max: [f32; 3],
}

impl Bounds {
    pub fn center(self) -> [f32; 3] {
        [
            (self.min[0] + self.max[0]) * 0.5,
            (self.min[1] + self.max[1]) * 0.5,
            (self.min[2] + self.max[2]) * 0.5,
        ]
    }

    pub fn half_diagonal(self) -> f32 {
        let dx = self.max[0] - self.min[0];
        let dy = self.max[1] - self.min[1];
        let dz = self.max[2] - self.min[2];
        (dx * dx + dy * dy + dz * dz).sqrt() * 0.5
    }
}

pub struct Scene {
    raw: *mut sys::flux_sg_scene,
}

impl Scene {
    pub fn from_glb(device: &Device, bytes: &[u8]) -> Result<Scene, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::flux_sg_load_glb(
                device.as_raw() as *mut sys::flux_device,
                bytes.as_ptr().cast(),
                bytes.len(),
                &mut raw,
            )
        })?;
        Ok(Scene { raw })
    }

    /// Load a GLB together with its base-colour textures and per-primitive
    /// materials. Image data is decoded with bounded resource limits, uploaded
    /// as sRGB, and retained by the installed materials.
    pub fn from_glb_with_materials(
        device: &Device,
        bytes: &[u8],
        target: MaterialTarget,
    ) -> Result<Scene, LoadError> {
        let gltf = gltf::Gltf::from_slice(bytes)?;
        let (materials, fallback) = materials::load(device, &gltf, target)?;
        let scene = Self::from_glb(device, bytes)?;
        let raw_materials: Vec<*mut sys::flux_material> = materials
            .iter()
            .map(|material| material.as_raw() as *mut sys::flux_material)
            .collect();
        check(unsafe {
            sys::flux_sg_scene_set_materials(
                scene.raw,
                if raw_materials.is_empty() {
                    std::ptr::null()
                } else {
                    raw_materials.as_ptr()
                },
                raw_materials.len() as u32,
                fallback.as_raw() as *mut sys::flux_material,
            )
        })?;
        Ok(scene)
    }

    pub fn primitive_count(&self) -> u32 {
        unsafe { sys::flux_sg_scene_primitive_count(self.raw) }
    }

    pub fn bounds(&self) -> Option<Bounds> {
        let mut min = sys::flux_vec3::default();
        let mut max = sys::flux_vec3::default();
        if unsafe { sys::flux_sg_scene_bounds(self.raw, &mut min, &mut max) } {
            Some(Bounds {
                min: [min.x, min.y, min.z],
                max: [max.x, max.y, max.z],
            })
        } else {
            None
        }
    }

    /// Current model-space position of a named VRM humanoid bone.
    pub fn humanoid_bone_position(&self, bone_name: &str) -> Option<[f32; 3]> {
        let bone_name = std::ffi::CString::new(bone_name).ok()?;
        let mut position = sys::flux_vec3::default();
        unsafe {
            sys::flux_sg_scene_humanoid_bone_position(self.raw, bone_name.as_ptr(), &mut position)
        }
        .then_some([position.x, position.y, position.z])
    }

    /// Load and bind the first glTF animation in `bytes`. VRM Animation 1.0
    /// clips are retargeted onto this scene's VRM humanoid rig.
    pub fn animation_from_glb(&self, bytes: &[u8]) -> Result<Animation, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::flux_sg_load_animation_glb(self.raw, bytes.as_ptr().cast(), bytes.len(), &mut raw)
        })?;
        Ok(Animation { raw })
    }

    /// Reset to the rest pose and apply `animation` at `time_seconds`.
    pub fn apply_animation(
        &mut self,
        animation: &Animation,
        time_seconds: f32,
        looping: bool,
    ) -> Result<(), Error> {
        check(unsafe {
            sys::flux_sg_scene_apply_animation(self.raw, animation.raw, time_seconds, looping)
        })
    }

    pub fn reset_pose(&mut self) {
        unsafe { sys::flux_sg_scene_reset_pose(self.raw) };
    }

    pub fn draw(
        &self,
        pass: &ScenePass<'_, '_>,
        camera: &Camera,
        material: &Material,
        light: Option<&SceneLight>,
    ) {
        let raw_light = light.map(SceneLight::as_raw);
        let opts = sys::flux_sg_draw_opts {
            material: material.as_raw() as *mut sys::flux_material,
            light: raw_light
                .as_ref()
                .map(|value| value as *const _ as *const sys::flux_scene_light)
                .unwrap_or(std::ptr::null()),
        };
        unsafe {
            sys::flux_sg_draw(
                pass.as_raw() as *mut sys::flux_frame,
                camera.as_raw() as *const sys::flux_camera,
                self.raw,
                &opts,
            )
        };
    }

    /// Draw using the per-primitive materials installed by
    /// [`Scene::from_glb_with_materials`].
    pub fn draw_materials(
        &self,
        pass: &ScenePass<'_, '_>,
        camera: &Camera,
        light: Option<&SceneLight>,
    ) {
        let raw_light = light.map(SceneLight::as_raw);
        let opts = sys::flux_sg_draw_opts {
            material: std::ptr::null_mut(),
            light: raw_light
                .as_ref()
                .map(|value| value as *const _ as *const sys::flux_scene_light)
                .unwrap_or(std::ptr::null()),
        };
        unsafe {
            sys::flux_sg_draw(
                pass.as_raw() as *mut sys::flux_frame,
                camera.as_raw() as *const sys::flux_camera,
                self.raw,
                &opts,
            )
        };
    }
}

/// A decoded animation clip bound to the scene that loaded it. Applying it to
/// another scene returns [`Error`] instead of using incompatible node indices.
pub struct Animation {
    raw: *mut sys::flux_sg_animation,
}

impl Animation {
    #[must_use]
    pub fn duration(&self) -> f32 {
        unsafe { sys::flux_sg_animation_duration(self.raw) }
    }

    #[must_use]
    pub fn channel_count(&self) -> u32 {
        unsafe { sys::flux_sg_animation_channel_count(self.raw) }
    }
}

impl Drop for Animation {
    fn drop(&mut self) {
        unsafe { sys::flux_sg_animation_release(self.raw) };
    }
}

impl Drop for Scene {
    fn drop(&mut self) {
        unsafe { sys::flux_sg_scene_release(self.raw) };
    }
}
