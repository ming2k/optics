//! Safe Rust bindings to the `flux-scene-graph` glTF content layer.
//!
//! [`Scene`] parses an in-memory binary glTF (`.glb`), owns the uploaded mesh
//! resources, reports world-space bounds for automatic framing, samples glTF
//! and VRM Animation clips, and records static or GPU-skinned draws into an
//! active [`flux::ScenePass`].

#![deny(rust_2018_idioms)]

use std::fmt;

use flux::{Camera, Device, Material, SceneLight, ScenePass};
use flux_scene_graph_sys as sys;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub sys::flux_result);

impl Error {
    fn check(rc: sys::flux_result) -> Result<(), Error> {
        if rc == sys::flux_result::FLUX_OK {
            Ok(())
        } else {
            Err(Error(rc))
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        use sys::flux_result::*;
        let message = match self.0 {
            FLUX_OK => "ok",
            FLUX_ERROR_INVALID_ARGUMENT => "invalid or malformed GLB",
            FLUX_ERROR_OUT_OF_MEMORY => "out of memory",
            FLUX_ERROR_OUT_OF_RANGE => "out of range",
            FLUX_ERROR_INVALID_STATE => "invalid state",
            FLUX_ERROR_UNSUPPORTED => "GLB contains no supported mesh primitives",
            FLUX_ERROR_BACKEND_FAILURE => "GPU backend failure",
            FLUX_ERROR_DEVICE_LOST => "device lost",
            FLUX_ERROR_SURFACE_LOST => "surface lost",
            FLUX_ERROR_TIMEOUT => "timeout",
        };
        write!(f, "flux scene graph error: {message}")
    }
}

impl std::error::Error for Error {}

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
        Error::check(unsafe {
            sys::flux_sg_load_glb(
                device.as_raw() as *mut sys::flux_device,
                bytes.as_ptr().cast(),
                bytes.len(),
                &mut raw,
            )
        })?;
        Ok(Scene { raw })
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
        Error::check(unsafe {
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
        Error::check(unsafe {
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
