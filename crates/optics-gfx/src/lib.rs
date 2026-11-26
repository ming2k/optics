//! Safe Rust bindings to **gfx** — a C23 Vulkan-first 2D graphics library.
//!
//! This crate wraps the raw FFI in [`optics_gfx_sys`] with RAII handles and a
//! Rust-native error type.
//!
//! [`Canvas`] is backend-agnostic: create it on the GPU from a [`Surface`]
//! ([`Canvas::new`]) or headless on the **software (CPU)** backend
//! ([`Canvas::new_cpu`]) — no GPU or window needed — then drive both with the
//! same drawing calls between [`Canvas::begin_session`] and [`CanvasSession::end`].
//! CPU pixels come back via [`Canvas::read_pixels`].
//!
//! Modules mirror the C headers: devices and surfaces, frames and render
//! targets, the canvas and its display lists, paints and paths, images,
//! samplers, blur/shadow effects and dma-buf interop. Everything is
//! re-exported at the crate root.

#![deny(rust_2018_idioms)]

use std::fmt;
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

/// Raw-bindings escape hatch, shared with the other safe crates. Kept `pub`
/// (not `pub(crate)`) deliberately: a thin binding ecosystem needs one
/// documented way to reach a symbol the safe layer has not wrapped yet, and
/// every such use is inherently `unsafe`.
pub use optics_gfx_sys as sys;

/// Library version string, from the linked library — the same value C's `OPTICS_GFX_VERSION_NUMBER` macros
/// describe. Sibling crates (`ui`, `window`, `material`) carry matching
/// wrappers, so a mixed-version process can be detected at startup.
pub fn version() -> &'static str {
    // SAFETY: optics_gfx_version_string returns a static NUL-terminated string.
    unsafe { std::ffi::CStr::from_ptr(sys::optics_gfx_version_string()) }
        .to_str()
        .unwrap_or("<invalid>")
}

/// Packed monotonic version of the linked library (the runtime form of
/// `OPTICS_GFX_VERSION_NUMBER`): `major << 22 | minor << 12 | patch`.
pub fn version_number() -> u32 {
    // SAFETY: pure query.
    unsafe { sys::optics_gfx_version_number() }
}

/// True when the linked library is at least `major.minor.patch`. Bindings
/// crates enforce the floor at build time (`atleast_version` in every
/// build.rs); this is the runtime check for a dynamically-loaded liboptics_gfx
/// (e.g. an app embedding gfx through a plugin host).
pub fn version_check(major: u32, minor: u32, patch: u32) -> bool {
    // SAFETY: pure query; the C signature takes int32s.
    unsafe { sys::optics_gfx_version_check(major as i32, minor as i32, patch as i32) }
}

mod canvas;
mod canvas_commands;
mod color;
mod device;
mod display_list;
mod dmabuf;
mod effect;
mod error;
mod format;
mod frame;
mod image;
mod paint;
mod sampler;
mod surface;
mod target;
#[cfg(test)]
mod tests;

pub use canvas::*;
pub use color::*;
pub use device::*;
pub use display_list::*;
pub use dmabuf::*;
pub use effect::*;
pub use error::*;
pub use format::*;
pub use frame::*;
pub use image::*;
pub use paint::*;
pub use sampler::*;
pub use surface::*;
pub use target::*;

pub(crate) fn vec3(v: [f32; 3]) -> sys::optics_gfx_vec3 {
    sys::optics_gfx_vec3 {
        x: v[0],
        y: v[1],
        z: v[2],
    }
}

pub(crate) fn vec4(v: [f32; 4]) -> sys::optics_gfx_vec4 {
    sys::optics_gfx_vec4 {
        x: v[0],
        y: v[1],
        z: v[2],
        w: v[3],
    }
}
