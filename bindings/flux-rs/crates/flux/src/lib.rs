//! Safe Rust bindings to **flux** — a C23 Vulkan-first 2D/3D graphics library.
//!
//! This crate wraps the raw FFI in [`flux_sys`] with RAII handles and a
//! Rust-native error type. It is the seam through which the ass compositor
//! drives flux's device, surface, canvas, and the raw-Vulkan accessors needed
//! to interoperate with a `VkSurfaceKHR` and to import client buffers.
//!
//! [`Canvas`] is backend-agnostic: create it on the GPU from a [`Surface`]
//! ([`Canvas::new`]) or headless on the **software (CPU)** backend
//! ([`Canvas::new_cpu`]) — no GPU or window needed — then drive both with the
//! same drawing calls between [`Canvas::begin_frame`] and [`Canvas::end`].
//! CPU pixels come back via [`Canvas::read_pixels`].
//!
//! Coverage grows demand-first; today it covers device creation and the raw
//! handle accessors required to bring up a nested surface.

#![deny(rust_2018_idioms)]

use std::fmt;
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};

pub use flux_sys as sys;

/// Backend-neutral pixel format, re-exported from the raw bindings.
pub use flux_sys::flux_format as Format;

/// A flux result code surfaced as a Rust error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub sys::flux_result);

impl Error {
    /// Map a raw result to `Ok(())` on `FLUX_OK`, else `Err`.
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
        // flux_result_string returns a static C string.
        let s = unsafe { std::ffi::CStr::from_ptr(sys::flux_result_string(self.0)) };
        write!(f, "flux error: {}", s.to_string_lossy())
    }
}

impl std::error::Error for Error {}

/// Linux DRM character-device identity used to constrain Vulkan physical
/// device selection. The node may be either a primary (`cardN`) or render
/// (`renderDN`) node; Flux accepts only the physical device that reports it
/// through `VK_EXT_physical_device_drm`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrmNode {
    pub major: u32,
    pub minor: u32,
}

impl DrmNode {
    fn raw(self) -> sys::flux_device_drm_node_desc {
        sys::flux_device_drm_node_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DEVICE_DRM_NODE_DESC,
            next: std::ptr::null(),
            drm_major: self.major,
            drm_minor: self.minor,
        }
    }
}

/// DRM identities reported by the Vulkan physical device selected by Flux.
/// A display-capable device commonly exposes both a privileged primary node
/// and an unprivileged render node; either may be absent.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrmDeviceIdentity {
    pub primary: Option<DrmNode>,
    pub render: Option<DrmNode>,
}

/// Semantic device capabilities requested independently of the Vulkan
/// extensions used by Flux's current backend.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DeviceFeatures(u64);

impl DeviceFeatures {
    pub const NONE: Self = Self(0);
    pub const DMABUF: Self = Self(sys::FLUX_DEVICE_FEATURE_DMABUF as u64);
    pub const DMABUF_SYNC_FILE: Self = Self(sys::FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE as u64);

    pub const fn bits(self) -> u64 {
        self.0
    }

    pub const fn contains(self, required: Self) -> bool {
        (self.0 & required.0) == required.0
    }

    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl std::ops::BitOr for DeviceFeatures {
    type Output = Self;

    fn bitor(self, rhs: Self) -> Self::Output {
        Self(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for DeviceFeatures {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

/// Options for semantic device creation. Required features reject a device
/// that cannot provide them; optional features are enabled on the normally
/// selected device when their complete implementation is available.
pub struct DeviceOptions<'a> {
    pub headless: bool,
    pub instance_extensions: &'a [&'a std::ffi::CStr],
    pub device_extensions: &'a [&'a std::ffi::CStr],
    pub frames_in_flight: u32,
    pub drm_node: Option<DrmNode>,
    pub required_features: DeviceFeatures,
    pub optional_features: DeviceFeatures,
}

impl<'a> Default for DeviceOptions<'a> {
    fn default() -> Self {
        Self {
            headless: false,
            instance_extensions: &[],
            device_extensions: &[],
            frames_in_flight: 0,
            drm_node: None,
            required_features: DeviceFeatures::NONE,
            optional_features: DeviceFeatures::NONE,
        }
    }
}

/// A flux device: the root GPU object. Refcounted in C; this handle owns one
/// reference and releases it on drop — unless it was constructed via
/// [`Device::borrow_raw`], in which case it is a non-owning view and `Drop`
/// is a no-op.
pub struct Device {
    raw: *mut sys::flux_device,
    /// When `true`, this handle does **not** own a reference and must not
    /// release it on drop. Set by [`Device::borrow_raw`] for views over a
    /// device another owner created (e.g. iris's `PaintHost::device`).
    borrowed: bool,
}

impl Device {
    /// Create a device. `instance_extensions` / `device_extensions` are extra
    /// Vulkan extensions to require at bootstrap (e.g. surface + platform
    /// surface extensions for a nested backend, or dmabuf import extensions).
    ///
    /// `frames_in_flight` sets how many frame slots (and, for offscreen
    /// surfaces, how many ring images) the device runs concurrently; pass 0
    /// for the flux default (2). Values above `FLUX_MAX_FRAMES_IN_FLIGHT` (3)
    /// are clamped by the C core.
    pub fn new(
        headless: bool,
        instance_extensions: &[&std::ffi::CStr],
        device_extensions: &[&std::ffi::CStr],
        frames_in_flight: u32,
    ) -> Result<Device, Error> {
        Self::new_with_options(DeviceOptions {
            headless,
            instance_extensions,
            device_extensions,
            frames_in_flight,
            ..DeviceOptions::default()
        })
    }

    /// Create a device strictly bound to the physical GPU that owns
    /// `drm_node`. Selection fails rather than falling back to a different
    /// Vulkan device when the node cannot be matched.
    pub fn new_for_drm_node(
        drm_node: DrmNode,
        headless: bool,
        instance_extensions: &[&std::ffi::CStr],
        device_extensions: &[&std::ffi::CStr],
        frames_in_flight: u32,
    ) -> Result<Device, Error> {
        Self::new_with_options(DeviceOptions {
            headless,
            instance_extensions,
            device_extensions,
            frames_in_flight,
            drm_node: Some(drm_node),
            ..DeviceOptions::default()
        })
    }

    /// Create a device from semantic capability requirements plus optional
    /// low-level Vulkan extension escape hatches.
    pub fn new_with_options(options: DeviceOptions<'_>) -> Result<Device, Error> {
        let inst: Vec<*const std::os::raw::c_char> = options
            .instance_extensions
            .iter()
            .map(|s| s.as_ptr())
            .collect();
        let dev: Vec<*const std::os::raw::c_char> = options
            .device_extensions
            .iter()
            .map(|s| s.as_ptr())
            .collect();
        let mut drm_extension = options.drm_node.map(DrmNode::raw);
        let mut features_extension = (!options.required_features.is_empty()
            || !options.optional_features.is_empty())
        .then(|| sys::flux_device_features_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DEVICE_FEATURES_DESC,
            next: std::ptr::null(),
            required: options.required_features.bits(),
            optional: options.optional_features.bits(),
        });

        let mut next = std::ptr::null();
        if let Some(features) = features_extension.as_mut() {
            features.next = next;
            next = features as *const _ as *const std::ffi::c_void;
        }
        if let Some(drm) = drm_extension.as_mut() {
            drm.next = next;
            next = drm as *const _ as *const std::ffi::c_void;
        }

        let desc = sys::flux_device_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DEVICE_DESC,
            next,
            required_instance_extensions: if inst.is_empty() {
                std::ptr::null()
            } else {
                inst.as_ptr()
            },
            required_instance_extension_count: inst.len() as u32,
            required_device_extensions: if dev.is_empty() {
                std::ptr::null()
            } else {
                dev.as_ptr()
            },
            required_device_extension_count: dev.len() as u32,
            headless: options.headless,
            frames_in_flight: options.frames_in_flight,
            ..unsafe { std::mem::zeroed() }
        };

        let mut out: *mut sys::flux_device = std::ptr::null_mut();
        // SAFETY: desc is fully initialized; the extension pointers outlive this
        // call (the Vecs live to the end of the function).
        let rc = unsafe { sys::flux_device_create(&desc, &mut out) };
        Error::check(rc)?;
        debug_assert!(!out.is_null());
        Ok(Device {
            raw: out,
            borrowed: false,
        })
    }

    /// Wrap a raw `flux_device*` as a non-owning view. Use this to obtain a
    /// `&Device` for a device another owner created and retains — the
    /// canonical case being iris's `PaintHost::device()`, which hands the
    /// app the device iris owns for its window. Opening a second device is
    /// unsupported and crashes, so a host that needs a `flux_text::Text`
    /// context (or any `&Device` consumer) inside iris's paint callback
    /// **must** borrow iris's device rather than creating its own.
    ///
    /// The returned `Device` does not release on drop: the borrow is valid
    /// only as long as the real owner keeps the device alive (for iris, the
    /// app's lifetime).
    ///
    /// # Safety
    /// `raw` must be a live `flux_device*` obtained from the flux C API (or
    /// from another binding's ABI-identical opaque pointer, such as iris's
    /// `PaintHost::device()`), and must remain valid for as long as the
    /// returned `Device` (or any `&Device` derived from it) is used.
    pub unsafe fn borrow_raw(raw: *mut sys::flux_device) -> Device {
        Device {
            raw,
            borrowed: true,
        }
    }

    /// The underlying raw `flux_device` pointer. Borrowed; the `Device` retains
    /// ownership. Use this to hand the device to flux-ui (`Ui::with_device`),
    /// whose bindings declare a distinct-but-ABI-identical `flux_device` type —
    /// cast the pointer at that seam.
    pub fn as_raw(&self) -> *mut sys::flux_device {
        self.raw
    }

    /// Semantic capabilities enabled on this logical device.
    pub fn enabled_features(&self) -> DeviceFeatures {
        DeviceFeatures(unsafe { sys::flux_device_enabled_features(self.raw) })
    }

    /// DRM primary/render identities reported by the selected Vulkan physical
    /// device. Returns `None` when the Vulkan driver exposes no reliable DRM
    /// identity.
    pub fn drm_identity(&self) -> Option<DrmDeviceIdentity> {
        let mut raw = sys::flux_drm_device_identity::default();
        if !unsafe { sys::flux_device_get_drm_identity(self.raw, &mut raw) } {
            return None;
        }
        Some(DrmDeviceIdentity {
            primary: raw.has_primary.then_some(DrmNode {
                major: raw.primary.major,
                minor: raw.primary.minor,
            }),
            render: raw.has_render.then_some(DrmNode {
                major: raw.render.major,
                minor: raw.render.minor,
            }),
        })
    }

    /// Raw `VkInstance` flux created. Feed to `ash` (as a `u64` via `as_raw`)
    /// when constructing a `VkSurfaceKHR` for a windowed backend.
    pub fn vk_instance(&self) -> sys::VkInstance {
        unsafe { sys::flux_device_vk_instance(self.raw) }
    }

    pub fn vk_physical_device(&self) -> sys::VkPhysicalDevice {
        unsafe { sys::flux_device_vk_physical_device(self.raw) }
    }

    pub fn vk_device(&self) -> sys::VkDevice {
        unsafe { sys::flux_device_vk_device(self.raw) }
    }

    pub fn vk_graphics_queue(&self) -> sys::VkQueue {
        unsafe { sys::flux_device_vk_graphics_queue(self.raw) }
    }

    pub fn vk_graphics_family(&self) -> u32 {
        unsafe { sys::flux_device_vk_graphics_family(self.raw) }
    }

    /// Block until the device is idle. Call before tearing down surfaces.
    pub fn wait_idle(&self) {
        unsafe { sys::flux_device_wait_idle(self.raw) };
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        // Only release when this handle actually owns a reference. A view
        // constructed via `borrow_raw` leaves the real owner in charge.
        if !self.borrowed {
            // SAFETY: we own one reference taken at create.
            unsafe { sys::flux_device_release(self.raw) };
        }
    }
}

/// One exported offscreen frame suitable for direct display through DRM/KMS
/// or another dma-buf consumer. The file descriptor is closed on drop.
#[derive(Debug)]
pub struct SurfaceDmabuf {
    pub fd: OwnedFd,
    /// Optional Linux sync_file that becomes readable when rendering and the
    /// Vulkan FOREIGN ownership release have completed.
    pub acquire_fence: Option<OwnedFd>,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub modifier: u64,
    /// Flux frame-in-flight slot that owns this image.
    pub slot: u32,
}

/// A presentable or offscreen rendering surface. Refcounted in C; this handle
/// owns one reference.
pub struct Surface {
    raw: *mut sys::flux_surface,
}

/// An immutable completed frame snapshot detached from a [`Surface`].
///
/// The staging allocation is independently owned, so this handle can be sent
/// to a worker while the surface continues presenting or is resized.
pub struct Readback {
    raw: *mut sys::flux_readback,
}

/// Pixel-aligned physical surface region captured by a frame readback.
/// Coordinates use the surface's top-left origin.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ReadbackRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl ReadbackRegion {
    fn raw(self) -> sys::flux_readback_region {
        sys::flux_readback_region {
            x: self.x,
            y: self.y,
            width: self.width,
            height: self.height,
        }
    }

    /// Number of tightly packed RGBA8 bytes in this region.
    pub fn byte_len(self) -> Option<usize> {
        if self.width == 0 || self.height == 0 {
            return None;
        }
        let pixels = usize::try_from(self.width)
            .ok()?
            .checked_mul(usize::try_from(self.height).ok()?)?;
        pixels.checked_mul(4)
    }
}

// The C handle exclusively owns immutable mapped staging and uses the
// device's locked staging cache when released.
unsafe impl Send for Readback {}

impl Readback {
    /// Exact surface region represented by this immutable snapshot.
    pub fn region(&self) -> ReadbackRegion {
        let mut raw = sys::flux_readback_region::default();
        unsafe { sys::flux_readback_get_region(self.raw, &mut raw) };
        ReadbackRegion {
            x: raw.x,
            y: raw.y,
            width: raw.width,
            height: raw.height,
        }
    }

    /// Copy and normalize the snapshot to tightly packed RGBA8.
    pub fn read_pixels(&self, dst: &mut [u8]) -> Result<(), Error> {
        Error::check(unsafe {
            sys::flux_readback_read_pixels(
                self.raw,
                dst.as_mut_ptr() as *mut std::os::raw::c_void,
                dst.len(),
            )
        })
    }
}

impl Drop for Readback {
    fn drop(&mut self) {
        unsafe { sys::flux_readback_release(self.raw) };
    }
}

impl Surface {
    /// Wrap a `VkSurfaceKHR` (created by the caller, e.g. via `ash`) as a flux
    /// surface with its swapchain.
    ///
    /// # Safety
    /// `vk_surface_khr` must be a live `VkSurfaceKHR` created from the same
    /// `VkInstance` as `device` ([`Device::vk_instance`]), and must outlive the
    /// returned `Surface`.
    pub unsafe fn from_vk(
        device: &Device,
        vk_surface_khr: *mut std::os::raw::c_void,
        width: u32,
        height: u32,
        vsync: bool,
    ) -> Result<Surface, Error> {
        let desc = sys::flux_surface_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_DESC,
            vk_surface_khr,
            width,
            height,
            vsync,
            // SAFETY: the bindgen descriptor is a C POD type; zero is the
            // documented default for fields not initialized above.
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_surface = std::ptr::null_mut();
        // SAFETY: `device` is live and the caller upholds the VkSurfaceKHR
        // lifetime and instance requirements documented by this function.
        Error::check(unsafe { sys::flux_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Create an OFFSCREEN surface (no window, no swapchain): flux owns RGBA8
    /// colour images at `width` x `height`. The frame loop is unchanged
    /// ([`Surface::begin_frame`] → record → [`Frame::submit`] →
    /// [`SubmittedFrame::present`]); `present` completes the frame without presenting,
    /// and [`Surface::read_pixels`] reads the result back. Both dimensions must
    /// be non-zero. Requires a headless or windowed device equally.
    pub fn offscreen(device: &Device, width: u32, height: u32) -> Result<Surface, Error> {
        let desc = sys::flux_surface_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_DESC,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Like [`Surface::offscreen`], but pinned to CPU readback: the images
    /// are never made dma-buf exportable, so [`Surface::read_pixels`] works
    /// even on a dma-buf-capable device (where plain offscreen surfaces
    /// transition their submitted images to the foreign consumer). The right
    /// constructor for screenshot/capture targets.
    pub fn offscreen_readback(device: &Device, width: u32, height: u32) -> Result<Surface, Error> {
        let readback = sys::flux_surface_readback_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_READBACK_DESC,
            next: std::ptr::null(),
            require_readback: true,
        };
        let desc = sys::flux_surface_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_DESC,
            next: &readback as *const _ as *const std::os::raw::c_void,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Create an exportable offscreen surface constrained to modifiers the
    /// external consumer accepts. Flux intersects `modifiers` with the Vulkan
    /// device's renderable, single-plane, dma-buf-exportable modifier set.
    ///
    /// This is the direct-display/zero-copy constructor: unlike [`Surface::offscreen`],
    /// it fails instead of silently creating an ordinary non-exportable image
    /// when no producer/consumer modifier is shared.
    pub fn offscreen_dmabuf(
        device: &Device,
        width: u32,
        height: u32,
        modifiers: &[u64],
    ) -> Result<Surface, Error> {
        if modifiers.is_empty() {
            return Err(Error(sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT));
        }
        let extension = sys::flux_surface_dmabuf_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_DMABUF_DESC,
            next: std::ptr::null(),
            modifiers: modifiers.as_ptr(),
            modifier_count: modifiers
                .len()
                .try_into()
                .map_err(|_| Error(sys::flux_result::FLUX_ERROR_OUT_OF_RANGE))?,
        };
        let desc = sys::flux_surface_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SURFACE_DESC,
            next: &extension as *const _ as *const std::os::raw::c_void,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_create(device.raw, &desc, &mut out) })?;
        let surface = Surface { raw: out };
        if !surface.is_exportable() {
            return Err(Error(sys::flux_result::FLUX_ERROR_UNSUPPORTED));
        }
        Ok(surface)
    }

    /// Read back an immutable frame snapshot as tightly packed RGBA8,
    /// row-major, top-left origin. `dst` must be at least the captured extent's
    /// `width * height * 4` bytes. A full-frame request uses the surface extent;
    /// a region request uses [`ReadbackRegion::byte_len`]. Windowed and
    /// exportable surfaces must first request a snapshot through
    /// [`Frame::request_readback`] or [`Frame::request_readback_region`].
    pub fn read_pixels(&self, dst: &mut [u8]) -> Result<(), Error> {
        Error::check(unsafe {
            sys::flux_surface_read_pixels(
                self.raw,
                dst.as_mut_ptr() as *mut std::os::raw::c_void,
                dst.len(),
            )
        })
    }

    /// Allocate readback staging before a latency-sensitive capture. Calling
    /// this is optional; [`Frame::request_readback`] otherwise allocates on
    /// first use. Re-run it after resizing the surface.
    pub fn prepare_readback(&self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_surface_prepare_readback(self.raw) })
    }

    /// Preallocate persistent staging for a future region readback. Existing
    /// staging with sufficient capacity is reused. Both dimensions must be
    /// non-zero and no larger than the surface extent.
    pub fn prepare_readback_region(&self, width: u32, height: u32) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_surface_prepare_readback_region(self.raw, width, height) })
    }

    /// Whether the most recently captured frame is available for
    /// [`read_pixels`](Self::read_pixels), without waiting. Once this returns
    /// `true`, `read_pixels` only copies already-mapped CPU memory.
    pub fn read_pixels_ready(&self) -> Result<bool, Error> {
        let mut ready = false;
        Error::check(unsafe { sys::flux_surface_read_pixels_ready(self.raw, &mut ready) })?;
        Ok(ready)
    }

    /// Detach the completed on-demand snapshot without copying its pixels.
    /// The returned handle can be moved to a post-processing worker while
    /// this surface continues presenting.
    pub fn take_readback(&self) -> Result<Readback, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_take_readback(self.raw, &mut out) })?;
        Ok(Readback { raw: out })
    }

    /// Whether this offscreen surface can export its rendered images as
    /// single-plane BGRA8 dma-bufs. Windowed surfaces are never exportable.
    pub fn is_exportable(&self) -> bool {
        unsafe { sys::flux_surface_exportable(self.raw) }
    }

    /// DRM format modifier selected for this exportable offscreen surface.
    /// `None` means this is not a dma-buf-exportable surface; `Some(0)` is the
    /// valid `DRM_FORMAT_MOD_LINEAR` modifier rather than absence.
    pub fn dmabuf_modifier(&self) -> Option<u64> {
        self.is_exportable()
            .then(|| unsafe { sys::flux_surface_dmabuf_modifier(self.raw) })
    }

    /// Export the most recently submitted offscreen frame without a pixel
    /// copy. This waits for Flux's GPU work, transfers image ownership to a
    /// foreign consumer, and returns an owned dma-buf descriptor plus the
    /// metadata required to create a DRM framebuffer.
    ///
    /// The caller must not let Flux reuse `slot` until the external consumer
    /// has released the buffer (for DRM/KMS, after a page flip has replaced
    /// the framebuffer). Dropping the returned fd does not itself release
    /// external image ownership.
    pub fn export_dmabuf(&self) -> Result<SurfaceDmabuf, Error> {
        let mut fd = -1;
        Error::check(unsafe { sys::flux_surface_export_dmabuf(self.raw, &mut fd) })?;
        if fd < 0 {
            return Err(Error(sys::flux_result::FLUX_ERROR_BACKEND_FAILURE));
        }
        let (width, height) = self.size();
        Ok(SurfaceDmabuf {
            // SAFETY: Flux returned ownership of a fresh descriptor on success.
            fd: unsafe { OwnedFd::from_raw_fd(fd) },
            acquire_fence: None,
            width,
            height,
            stride: unsafe { sys::flux_surface_dmabuf_stride(self.raw) },
            modifier: unsafe { sys::flux_surface_dmabuf_modifier(self.raw) },
            slot: unsafe { sys::flux_surface_last_slot(self.raw) },
        })
    }

    /// Export the most recently submitted frame with an explicit acquire
    /// fence, without waiting for the GPU on the calling CPU thread.
    ///
    /// The consumer must wait `acquire_fence` before reading the dma-buf. DRM
    /// atomic clients pass it to the plane's `IN_FENCE_FD` property. This may
    /// be called once per submitted frame slot.
    pub fn export_dmabuf_explicit(&self) -> Result<SurfaceDmabuf, Error> {
        let mut fd = -1;
        let mut sync_fd = -1;
        Error::check(unsafe {
            sys::flux_surface_export_dmabuf_explicit(self.raw, &mut fd, &mut sync_fd)
        })?;
        if fd < 0 || sync_fd < 0 {
            if fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(fd)) };
            }
            if sync_fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(sync_fd)) };
            }
            return Err(Error(sys::flux_result::FLUX_ERROR_BACKEND_FAILURE));
        }
        let (width, height) = self.size();
        Ok(SurfaceDmabuf {
            // SAFETY: Flux returned ownership of both fresh descriptors.
            fd: unsafe { OwnedFd::from_raw_fd(fd) },
            acquire_fence: Some(unsafe { OwnedFd::from_raw_fd(sync_fd) }),
            width,
            height,
            stride: unsafe { sys::flux_surface_dmabuf_stride(self.raw) },
            modifier: unsafe { sys::flux_surface_dmabuf_modifier(self.raw) },
            slot: unsafe { sys::flux_surface_last_slot(self.raw) },
        })
    }

    /// Recreate the swapchain at a new extent. Safe to call from a resize event.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_surface_resize(self.raw, width, height) })
    }

    /// Current swapchain extent.
    pub fn size(&self) -> (u32, u32) {
        let mut info = sys::flux_surface_info::default();
        unsafe { sys::flux_surface_get_info(self.raw, &mut info) };
        (info.width, info.height)
    }

    /// Backend-neutral color format selected for this surface.
    pub fn format(&self) -> Format {
        unsafe { sys::flux_format_from_vk(sys::flux_surface_vk_format(self.raw)) }
    }

    /// Acquire the next frame. Returns the backend result so callers can handle
    /// `SURFACE_LOST` / out-of-date by resizing.
    pub fn begin_frame(&self) -> Result<Frame<'_>, Error> {
        let desc = sys::flux_frame_begin_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_FRAME_BEGIN_DESC,
            timeout_ns: 0,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_frame = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_begin_frame(self.raw, &desc, &mut out) })?;
        Ok(Frame {
            raw: out,
            surface: self,
        })
    }

    pub fn as_raw(&self) -> *mut sys::flux_surface {
        self.raw
    }
}

impl Drop for Surface {
    fn drop(&mut self) {
        unsafe { sys::flux_surface_release(self.raw) };
    }
}

/// A recording frame obtained from [`Surface::begin_frame`]. It borrows the
/// surface because the underlying C frame is stored inside that surface rather
/// than being independently refcounted.
///
/// The frame API uses typestate: submission consumes this value and returns a
/// [`SubmittedFrame`], which is the only type that can be presented. Duplicate
/// and out-of-order transitions therefore cannot be expressed through safe
/// Rust.
#[must_use = "a frame must be submitted and then presented"]
pub struct Frame<'surface> {
    raw: *mut sys::flux_frame,
    surface: &'surface Surface,
}

impl<'surface> Frame<'surface> {
    /// Copy this exact frame into immutable readback staging during submit.
    /// Later frames do not change the snapshot.
    pub fn request_readback(&mut self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_frame_request_readback(self.raw) })
    }

    /// Copy only `region` from this frame into tightly packed immutable
    /// readback staging. The Vulkan copy and later CPU normalization are both
    /// proportional to the selected extent; no full-surface intermediate is
    /// produced.
    pub fn request_readback_region(&mut self, region: ReadbackRegion) -> Result<(), Error> {
        let raw = region.raw();
        Error::check(unsafe { sys::flux_frame_request_readback_region(self.raw, &raw) })
    }

    /// Submit recorded work to the GPU and advance to the submitted state.
    pub fn submit(self) -> Result<SubmittedFrame<'surface>, Error> {
        Error::check(unsafe { sys::flux_frame_submit(self.raw) })?;
        Ok(SubmittedFrame {
            raw: self.raw,
            _surface: PhantomData,
        })
    }

    /// Frame-in-flight slot index. Resources written by the GPU should keep
    /// one instance per slot when the previous frame may still be executing.
    pub fn index(&self) -> u32 {
        unsafe { sys::flux_frame_index(self.raw) }
    }

    /// Current extent of the surface this frame records into.
    pub fn surface_size(&self) -> (u32, u32) {
        self.surface.size()
    }

    /// Begin a depth-tested scene pass targeting this frame's surface.
    ///
    /// `depth` must match the surface extent. The target's previous contents
    /// are discarded and depth is cleared to 1.0. The returned guard ends the
    /// pass on drop; scene drawing APIs accept the guard directly, preventing
    /// them from escaping the pass bracket.
    pub fn begin_scene_pass<'frame>(
        &'frame mut self,
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let (width, height) = self.surface.size();
        if depth.size() != (width, height) {
            return Err(Error(sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT));
        }

        self.begin_scene_pass_raw(
            std::ptr::null_mut(),
            self.surface.format(),
            (width, height),
            depth,
            color,
        )
    }

    /// Begin a depth-tested scene pass targeting a sampleable offscreen image.
    ///
    /// The color image and depth target must have the same extent. The image
    /// is transitioned from sampleable to color-attachment layout for the
    /// pass and restored when the returned guard ends, so Canvas and image
    /// effects can consume it immediately afterward.
    pub fn begin_image_scene_pass<'frame>(
        &'frame mut self,
        target: &'frame Image,
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let size = target.size();
        if depth.size() != size {
            return Err(Error(sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT));
        }
        Error::check(unsafe { sys::flux_frame_prepare_image_target(self.raw, target.raw) })?;
        self.begin_scene_pass_raw(target.raw, target.format(), size, depth, color)
    }

    fn begin_scene_pass_raw<'frame>(
        &'frame mut self,
        target: *mut sys::flux_image,
        color_format: Format,
        (width, height): (u32, u32),
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let (load_op, clear_color) = match color {
            SceneColorLoad::Load => (sys::flux_load_op::FLUX_LOAD_LOAD, [0.0, 0.0, 0.0, 0.0]),
            SceneColorLoad::Clear(rgba) => (sys::flux_load_op::FLUX_LOAD_CLEAR, rgba),
        };
        let color_attachment = sys::flux_pass_attachment {
            view: if target.is_null() {
                std::ptr::null_mut()
            } else {
                unsafe { sys::flux_image_vk_image_view(target) }
            },
            format: unsafe { sys::flux_format_to_vk(color_format) },
            load_op,
            store_op: sys::flux_store_op::FLUX_STORE_STORE,
            clear_color: vec4(clear_color),
            ..Default::default()
        };
        let depth_attachment = sys::flux_pass_depth_attachment {
            view: unsafe { sys::flux_target_vk_view(depth.raw) },
            format: unsafe { sys::flux_format_to_vk(depth.format) },
            load_op: sys::flux_load_op::FLUX_LOAD_CLEAR,
            store_op: sys::flux_store_op::FLUX_STORE_DONT_CARE,
            clear_depth: 1.0,
            ..Default::default()
        };
        let pass = sys::flux_pass_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_PASS_DESC,
            color_attachment_count: 1,
            color_attachments: &color_attachment,
            depth: &depth_attachment,
            width,
            height,
            ..Default::default()
        };
        unsafe {
            sys::flux_frame_prepare_target(self.raw, depth.raw);
            sys::flux_frame_begin_pass(self.raw, &pass);
            sys::flux_frame_set_viewport(self.raw, 0.0, 0.0, width as f32, height as f32, 0.0, 1.0);
            sys::flux_frame_set_scissor(self.raw, 0, 0, width, height);
        }
        Ok(ScenePass {
            frame: self,
            active: true,
            target,
        })
    }

    pub fn as_raw(&self) -> *mut sys::flux_frame {
        self.raw
    }
}

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
    frame: &'frame mut Frame<'surface>,
    active: bool,
    target: *mut sys::flux_image,
}

impl ScenePass<'_, '_> {
    /// Raw frame pointer for sibling content libraries such as
    /// `flux-scene-graph`. The pass remains owned by this guard.
    pub fn as_raw(&self) -> *mut sys::flux_frame {
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
            sys::flux_frame_end_pass(self.frame.raw);
            if !self.target.is_null() {
                let _ = sys::flux_frame_finish_image_target(self.frame.raw, self.target);
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

/// Perspective camera used by flux scene renderers.
pub struct Camera {
    raw: sys::flux_camera,
}

impl Camera {
    pub fn perspective(fov_y_rad: f32, aspect: f32, z_near: f32, z_far: f32) -> Camera {
        let mut raw = sys::flux_camera::default();
        unsafe { sys::flux_camera_perspective(&mut raw, fov_y_rad, aspect, z_near, z_far) };
        Camera { raw }
    }

    pub fn look_at(&mut self, eye: [f32; 3], center: [f32; 3], up: [f32; 3]) {
        unsafe { sys::flux_camera_look_at(&mut self.raw, vec3(eye), vec3(center), vec3(up)) };
    }

    pub fn as_raw(&self) -> *const sys::flux_camera {
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
    pub fn as_raw(&self) -> sys::flux_scene_light {
        sys::flux_scene_light {
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

/// Refcounted built-in scene material.
pub struct Material {
    raw: *mut sys::flux_material,
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

    fn new_impl(
        device: &Device,
        desc: MaterialDesc,
        options: Option<MaterialOptions<'_>>,
    ) -> Result<Material, Error> {
        let kind = match desc.kind {
            MaterialKind::Unlit => sys::flux_material_kind::FLUX_MATERIAL_UNLIT,
            MaterialKind::Phong => sys::flux_material_kind::FLUX_MATERIAL_PHONG,
        };
        let raw_surface = options.map(|options| {
            let texture = options.base_color_texture;
            let alpha_mode = match options.alpha_mode {
                MaterialAlphaMode::Opaque => {
                    sys::flux_material_alpha_mode::FLUX_MATERIAL_ALPHA_OPAQUE
                }
                MaterialAlphaMode::Mask => sys::flux_material_alpha_mode::FLUX_MATERIAL_ALPHA_MASK,
                MaterialAlphaMode::Blend => {
                    sys::flux_material_alpha_mode::FLUX_MATERIAL_ALPHA_BLEND
                }
            };
            sys::flux_material_surface_desc {
                type_: sys::flux_struct_type::FLUX_TYPE_MATERIAL_SURFACE_DESC,
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
        let raw_desc = sys::flux_material_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_MATERIAL_DESC,
            next: raw_surface
                .as_ref()
                .map(|value| value as *const _ as *const std::ffi::c_void)
                .unwrap_or(std::ptr::null()),
            kind,
            base_color: vec4(desc.base_color),
            color_format: desc.color_format,
            depth_format: desc.depth_format,
            shininess: desc.shininess,
            specular: desc.specular,
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_material_create(device.raw, &raw_desc, &mut raw) })?;
        Ok(Material { raw })
    }

    pub fn as_raw(&self) -> *mut sys::flux_material {
        self.raw
    }
}

/// Refcounted bindless texture sampler.
pub struct Sampler {
    raw: *mut sys::flux_sampler,
}

impl Sampler {
    pub fn new(device: &Device, desc: SamplerDesc) -> Result<Self, Error> {
        fn filter(value: Filter) -> sys::flux_filter {
            match value {
                Filter::Nearest => sys::flux_filter::FLUX_FILTER_NEAREST,
                Filter::Linear => sys::flux_filter::FLUX_FILTER_LINEAR,
            }
        }
        fn address(value: AddressMode) -> sys::flux_address_mode {
            match value {
                AddressMode::Repeat => sys::flux_address_mode::FLUX_ADDRESS_REPEAT,
                AddressMode::ClampToEdge => sys::flux_address_mode::FLUX_ADDRESS_CLAMP_TO_EDGE,
                AddressMode::MirroredRepeat => sys::flux_address_mode::FLUX_ADDRESS_MIRRORED_REPEAT,
                AddressMode::ClampToBorder => sys::flux_address_mode::FLUX_ADDRESS_CLAMP_TO_BORDER,
            }
        }
        let raw_desc = sys::flux_sampler_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_SAMPLER_DESC,
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
        Error::check(unsafe { sys::flux_sampler_create(device.raw, &raw_desc, &mut raw) })?;
        Ok(Self { raw })
    }

    pub fn as_raw(&self) -> *mut sys::flux_sampler {
        self.raw
    }
}

impl Drop for Sampler {
    fn drop(&mut self) {
        unsafe { sys::flux_sampler_release(self.raw) };
    }
}

impl Drop for Material {
    fn drop(&mut self) {
        unsafe { sys::flux_material_release(self.raw) };
    }
}

/// Refcounted render target. Scene passes use a depth target matching the
/// surface extent, with one target per frame-in-flight slot.
pub struct Target {
    raw: *mut sys::flux_target,
    format: Format,
}

impl Target {
    pub fn depth(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
    ) -> Result<Target, Error> {
        let desc = sys::flux_target_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_TARGET_DESC,
            usage: sys::flux_target_usage::FLUX_TARGET_DEPTH as u32,
            format,
            width,
            height,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_target_create(device.raw, &desc, &mut raw) })?;
        Ok(Target { raw, format })
    }

    pub fn size(&self) -> (u32, u32) {
        unsafe {
            (
                sys::flux_target_width(self.raw),
                sys::flux_target_height(self.raw),
            )
        }
    }

    pub fn as_raw(&self) -> *mut sys::flux_target {
        self.raw
    }
}

impl Drop for Target {
    fn drop(&mut self) {
        unsafe { sys::flux_target_release(self.raw) };
    }
}

fn vec3(v: [f32; 3]) -> sys::flux_vec3 {
    sys::flux_vec3 {
        x: v[0],
        y: v[1],
        z: v[2],
    }
}

fn vec2(v: [f32; 2]) -> sys::flux_vec2 {
    sys::flux_vec2 { x: v[0], y: v[1] }
}

fn vec4(v: [f32; 4]) -> sys::flux_vec4 {
    sys::flux_vec4 {
        x: v[0],
        y: v[1],
        z: v[2],
        w: v[3],
    }
}

/// A successfully submitted frame waiting to be presented.
#[must_use = "a submitted frame must be presented"]
pub struct SubmittedFrame<'surface> {
    raw: *mut sys::flux_frame,
    _surface: PhantomData<&'surface Surface>,
}

impl SubmittedFrame<'_> {
    /// Present the frame and consume the surface-owned frame-slot borrow.
    pub fn present(self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_frame_present(self.raw) })
    }

    pub fn as_raw(&self) -> *mut sys::flux_frame {
        self.raw
    }
}

/// A 2D canvas bound to a [`Surface`]. Records draws into a [`Frame`] between
/// [`Canvas::begin`] and [`Canvas::end`].
pub struct Canvas {
    raw: *mut sys::flux_canvas,
    /// When `true`, this handle does **not** own the canvas and must not
    /// destroy it on drop. Set by [`Canvas::borrow_raw`] for views over a
    /// canvas another owner created and retains (e.g. iris's paint callback).
    borrowed: bool,
}

/// GPU attachment antialiasing for one [`Canvas`] pass.
///
/// CPU canvases accept this policy and retain their native software
/// antialiasing.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum CanvasAntialias {
    /// Preserve Flux's compatibility policy: clearing passes use 4x MSAA,
    /// while loading passes render directly into the one-sample destination.
    #[default]
    Auto,
    /// Render directly into the one-sample destination.
    None,
    /// Render with 4x MSAA and resolve. Requires `clear: Some(_)`.
    Msaa4x,
}

impl CanvasAntialias {
    fn raw(self) -> sys::flux_canvas_antialias {
        match self {
            Self::Auto => sys::flux_canvas_antialias::FLUX_CANVAS_ANTIALIAS_AUTO,
            Self::None => sys::flux_canvas_antialias::FLUX_CANVAS_ANTIALIAS_NONE,
            Self::Msaa4x => sys::flux_canvas_antialias::FLUX_CANVAS_ANTIALIAS_MSAA_4X,
        }
    }
}

/// Load and antialiasing policy for one [`Canvas`] pass.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub struct CanvasPassOptions {
    /// `Some(color)` clears the destination; `None` preserves its contents.
    pub clear: Option<u32>,
    pub antialias: CanvasAntialias,
    /// Optional dirty rectangle in physical framebuffer pixels. `None` uses
    /// the complete destination. Both clearing and drawing are constrained to
    /// this rectangle while pixels outside it remain untouched.
    pub render_area: Option<CanvasRenderArea>,
    /// Open a genuine no-stencil pass for callers that emit only operations
    /// whose semantics never require stencil (e.g. solid/image compositor
    /// blits). No stencil image is allocated, transitioned, cleared, or bound;
    /// Vulkan uses a separate pipeline variant with an undefined stencil
    /// format. A stencil-dependent path is dropped with an explicit Flux error
    /// instead of being silently misrendered; use [`Canvas::end_checked`] or
    /// [`Canvas::end_target_checked`] to receive that error.
    pub skip_stencil: bool,
}

/// Physical-pixel render area for a partial Canvas pass.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CanvasRenderArea {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

/// Build the stable base pass descriptor and its optional stack-owned pNext
/// extension, then invoke `f` while every referenced value is alive. Keeping
/// the no-stencil policy out of the base structure preserves C ABI compatibility
/// with applications compiled against older Flux headers.
fn with_raw_canvas_pass_desc<T>(
    options: CanvasPassOptions,
    f: impl FnOnce(&sys::flux_canvas_pass_desc) -> T,
) -> T {
    let clear = options.clear;
    let area = options.render_area;
    let no_stencil = sys::flux_canvas_no_stencil_desc {
        type_: sys::flux_struct_type::FLUX_TYPE_CANVAS_NO_STENCIL_DESC,
        next: std::ptr::null(),
        enabled: true,
    };
    let desc = sys::flux_canvas_pass_desc {
        type_: sys::flux_struct_type::FLUX_TYPE_CANVAS_PASS_DESC,
        next: if options.skip_stencil {
            (&no_stencil as *const sys::flux_canvas_no_stencil_desc).cast()
        } else {
            std::ptr::null()
        },
        clear_color: clear
            .as_ref()
            .map(|color| color as *const u32)
            .unwrap_or(std::ptr::null()),
        antialias: options.antialias.raw(),
        render_offset_x: area.map_or(0, |area| area.x),
        render_offset_y: area.map_or(0, |area| area.y),
        render_width: area.map_or(0, |area| area.width),
        render_height: area.map_or(0, |area| area.height),
    };
    f(&desc)
}

impl Canvas {
    /// Wrap a raw `flux_canvas*` as a non-owning view. The returned handle does
    /// **not** call `flux_canvas_destroy` on drop — the real owner (e.g. iris,
    /// for its window's canvas) stays in charge. Use this when a host hands
    /// you a live `flux_canvas*` that is already inside an open
    /// `flux_canvas_begin/end` pair and you want to issue draws through the
    /// safe surface without taking ownership.
    ///
    /// # Safety
    /// `raw` must be a live `flux_canvas*` obtained from the flux C API (or an
    /// ABI-identical pointer from a sibling binding), and must remain valid for
    /// as long as the returned `Canvas` (or any `&Canvas` derived from it) is
    /// used.
    pub unsafe fn borrow_raw(raw: *mut sys::flux_canvas) -> Canvas {
        Canvas {
            raw,
            borrowed: true,
        }
    }
}

impl Canvas {
    pub fn new(surface: &Surface) -> Result<Canvas, Error> {
        let desc = sys::flux_canvas_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_CANVAS_DESC,
            surface: surface.raw,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_canvas = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_canvas_create(&desc, &mut out) })?;
        Ok(Canvas {
            raw: out,
            borrowed: false,
        })
    }

    /// Create a headless **software (CPU)** canvas with a `width`x`height`
    /// framebuffer (physical pixels) and content `scale`. No GPU, device, or
    /// surface required. Record between [`begin_cpu`](Self::begin_cpu) /
    /// [`begin_frame`](Self::begin_frame) (pass `None` for the frame) and
    /// [`end`](Self::end), then read the result with
    /// [`read_pixels`](Self::read_pixels).
    ///
    /// Supported: solid fills, paths, rounded rects, gradients, clipping.
    /// Image and glyph (text) draws are ignored on a CPU canvas — they need a
    /// GPU-resident texture.
    pub fn new_cpu(width: u32, height: u32, scale: f32) -> Result<Canvas, Error> {
        let mut out: *mut sys::flux_canvas = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_canvas_create_cpu(width, height, scale, &mut out) })?;
        Ok(Canvas {
            raw: out,
            borrowed: false,
        })
    }

    /// Begin recording. `clear` clears the surface to a packed color; `None`
    /// loads the existing contents.
    pub fn begin(&self, frame: &Frame<'_>, clear: Option<u32>) -> Result<(), Error> {
        let color = clear; // flux_color is a packed u32
        let ptr = color
            .as_ref()
            .map(|c| c as *const u32)
            .unwrap_or(std::ptr::null());
        Error::check(unsafe { sys::flux_canvas_begin(self.raw, frame.raw, ptr) })
    }

    /// Wait a producer sync-file before this frame samples an already
    /// imported dma-buf image.
    ///
    /// Call between [`Canvas::begin`] and [`Canvas::end`], before drawing the
    /// image. Flux consumes `acquire_fence` on success; Rust closes it on
    /// error. The wait is submitted to the GPU and does not block the caller.
    pub fn wait_dmabuf_acquire(&self, image: &Image, acquire_fence: OwnedFd) -> Result<(), Error> {
        let result = unsafe {
            sys::flux_canvas_wait_dmabuf_acquire(self.raw, image.raw, acquire_fence.as_raw_fd())
        };
        Error::check(result)?;
        std::mem::forget(acquire_fence);
        Ok(())
    }

    /// Begin a Canvas pass with independent load and antialiasing policy.
    pub fn begin_pass(&self, frame: &Frame<'_>, options: CanvasPassOptions) -> Result<(), Error> {
        with_raw_canvas_pass_desc(options, |desc| {
            Error::check(unsafe { sys::flux_canvas_begin_pass(self.raw, frame.raw, desc) })
        })
    }

    /// Begin a Canvas pass into a sampleable offscreen render-target image.
    /// `Some(clear)` uses the Canvas MSAA resolve path; `None` loads the
    /// image's existing contents for composition after a scene pass.
    pub fn begin_target(
        &self,
        frame: &Frame<'_>,
        target: &Image,
        clear: Option<u32>,
    ) -> Result<(), Error> {
        let ptr = clear
            .as_ref()
            .map(|c| c as *const u32)
            .unwrap_or(std::ptr::null());
        Error::check(unsafe { sys::flux_canvas_begin_target(self.raw, frame.raw, target.raw, ptr) })
    }

    /// Begin an offscreen-target pass with independent load and antialiasing
    /// policy.
    pub fn begin_target_pass(
        &self,
        frame: &Frame<'_>,
        target: &Image,
        options: CanvasPassOptions,
    ) -> Result<(), Error> {
        with_raw_canvas_pass_desc(options, |desc| {
            Error::check(unsafe {
                sys::flux_canvas_begin_target_pass(self.raw, frame.raw, target.raw, desc)
            })
        })
    }

    /// End an offscreen Canvas pass and restore the target to a sampleable
    /// image layout.
    pub fn end_target(&self) {
        unsafe { sys::flux_canvas_end_target(self.raw) };
    }

    /// End an offscreen pass and surface any sticky draw-time contract error.
    /// No-stencil compositor passes should prefer this over [`Self::end_target`]
    /// so a future stencil-dependent draw cannot be silently dropped.
    pub fn end_target_checked(&self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_canvas_end_target_checked(self.raw) })
    }

    /// Unified, backend-agnostic pass bracket. Pass `Some(frame)` for a GPU
    /// canvas and `None` for a CPU canvas; the drawing code in between is
    /// identical either way. `clear` clears to a packed color, `None` loads.
    pub fn begin_frame(&self, frame: Option<&Frame<'_>>, clear: Option<u32>) -> Result<(), Error> {
        let fptr = frame.map(|f| f.raw).unwrap_or(std::ptr::null_mut());
        let ptr = clear
            .as_ref()
            .map(|c| c as *const u32)
            .unwrap_or(std::ptr::null());
        Error::check(unsafe { sys::flux_canvas_begin_frame(self.raw, fptr, ptr) })
    }

    /// Begin recording on a CPU canvas (equivalent to `begin_frame(None, clear)`).
    pub fn begin_cpu(&self, clear: Option<u32>) -> Result<(), Error> {
        self.begin_frame(None, clear)
    }

    pub fn end(&self) {
        unsafe { sys::flux_canvas_end(self.raw) };
    }

    /// End a surface pass and surface any sticky draw-time contract error.
    /// No-stencil compositor passes should use this checked form.
    pub fn end_checked(&self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_canvas_end_checked(self.raw) })
    }

    /// Snapshot the canvas' pixels as premultiplied RGBA8 (row-major). Returns
    /// `(width, height, stride_bytes, pixels)` on the CPU backend; `None` on the
    /// GPU backend (use an offscreen surface / target for GPU readback). The
    /// slice borrows canvas-owned memory, valid until the next call or drop.
    pub fn read_pixels(&self) -> Option<(u32, u32, u32, &[u8])> {
        let (mut w, mut h, mut stride) = (0u32, 0u32, 0u32);
        let ptr = unsafe { sys::flux_canvas_read_pixels(self.raw, &mut w, &mut h, &mut stride) };
        if ptr.is_null() {
            return None;
        }
        let len = (h as usize) * (stride as usize);
        // SAFETY: ptr is a valid buffer of `len` bytes owned by the canvas and
        // stable until the next read_pixels / destroy.
        let slice = unsafe { std::slice::from_raw_parts(ptr, len) };
        Some((w, h, stride, slice))
    }

    /// Set the content scale (device-pixel ratio). The canvas then draws in
    /// logical units mapped onto the physical surface, and `flux_text`
    /// rasterises glyphs to match — set this once when the surface scale
    /// changes instead of applying a per-frame [`scale`](Self::scale).
    pub fn set_scale(&self, scale: f32) {
        unsafe { sys::flux_canvas_set_scale(self.raw, scale) };
    }

    /// The current content scale (device-pixel ratio).
    pub fn content_scale(&self) -> f32 {
        unsafe { sys::flux_canvas_get_scale(self.raw) }
    }

    /// Save the current transform/clip state onto the stack.
    pub fn save(&self) {
        unsafe { sys::flux_canvas_save(self.raw) };
    }

    /// Restore the transform/clip state saved by the matching [`save`](Self::save).
    pub fn restore(&self) {
        unsafe { sys::flux_canvas_restore(self.raw) };
    }

    /// Translate the coordinate system by `(x, y)`.
    pub fn translate(&self, x: f32, y: f32) {
        unsafe { sys::flux_canvas_translate(self.raw, x, y) };
    }

    /// Scale the coordinate system by `(sx, sy)`. Used to map a logical
    /// (DPI-independent) coordinate space onto the physical framebuffer: pair
    /// `scale(s, s)` with `flux_text`'s device scale `s` for crisp HiDPI text.
    pub fn scale(&self, sx: f32, sy: f32) {
        unsafe { sys::flux_canvas_scale(self.raw, sx, sy) };
    }

    /// Rotate subsequent drawing by `radians` around the current origin.
    pub fn rotate(&self, radians: f32) {
        unsafe { sys::flux_canvas_rotate(self.raw, radians) };
    }

    /// Intersect subsequent drawing with an axis-aligned clip rectangle.
    pub fn clip_rect(&self, x: f32, y: f32, w: f32, h: f32) {
        let rect = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_clip_rect(self.raw, rect) };
    }

    /// Fill a rectangle with a packed solid color.
    pub fn fill_rect(&self, x: f32, y: f32, w: f32, h: f32, color: u32) {
        let r = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_fill_rect_color(self.raw, r, color) };
    }

    /// Fill a rounded rectangle with a packed solid color (analytic-AA SDF).
    /// Works on both backends.
    pub fn fill_rrect(&self, x: f32, y: f32, w: f32, h: f32, radius: f32, color: u32) {
        let r = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_fill_rrect(self.raw, r, radius, color) };
    }

    /// Stroke an analytic rounded rectangle with a premultiplied colour.
    #[allow(clippy::too_many_arguments)]
    pub fn stroke_rrect(
        &self,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        radius: f32,
        color: u32,
        width: f32,
    ) {
        let rect = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_stroke_rrect(self.raw, rect, radius, color, width) };
    }

    /// Fill `path` with `paint` (the paint's fill rule applies).
    pub fn fill_path(&self, path: &Path, paint: &Paint) {
        unsafe { sys::flux_canvas_fill_path(self.raw, path.raw, &paint.raw) };
    }

    /// Stroke `path` with `paint` (width, cap, and join apply).
    pub fn stroke_path(&self, path: &Path, paint: &Paint) {
        unsafe { sys::flux_canvas_stroke_path(self.raw, path.raw, &paint.raw) };
    }

    /// Fill a rectangle with a linear gradient in canvas pixel space.
    ///
    /// Flux supports at most eight stops; additional stops are ignored.
    pub fn fill_rect_linear_gradient(
        &self,
        rect: (f32, f32, f32, f32),
        from: (f32, f32),
        to: (f32, f32),
        stops: &[GradientStop],
    ) {
        let raw_stops = gradient_stops(stops);
        if raw_stops.is_empty() {
            return;
        }
        let paint = unsafe {
            sys::flux_paint_linear_gradient(
                sys::flux_point {
                    x: from.0,
                    y: from.1,
                },
                sys::flux_point { x: to.0, y: to.1 },
                raw_stops.as_ptr(),
                u32::try_from(raw_stops.len()).unwrap_or(8),
            )
        };
        let rect = sys::flux_rect {
            x: rect.0,
            y: rect.1,
            w: rect.2,
            h: rect.3,
        };
        unsafe { sys::flux_canvas_fill_rect(self.raw, rect, &paint) };
    }

    /// Fill a rectangle with a radial gradient in canvas pixel space.
    ///
    /// Flux supports at most eight stops; additional stops are ignored.
    pub fn fill_rect_radial_gradient(
        &self,
        rect: (f32, f32, f32, f32),
        center: (f32, f32),
        radius: f32,
        stops: &[GradientStop],
    ) {
        let raw_stops = gradient_stops(stops);
        if raw_stops.is_empty() {
            return;
        }
        let paint = unsafe {
            sys::flux_paint_radial_gradient(
                sys::flux_point {
                    x: center.0,
                    y: center.1,
                },
                radius.max(0.0),
                raw_stops.as_ptr(),
                u32::try_from(raw_stops.len()).unwrap_or(8),
            )
        };
        let rect = sys::flux_rect {
            x: rect.0,
            y: rect.1,
            w: rect.2,
            h: rect.3,
        };
        unsafe { sys::flux_canvas_fill_rect(self.raw, rect, &paint) };
    }

    /// Draw an image into the destination rectangle (pixel space).
    pub fn draw_image(&self, image: &Image, x: f32, y: f32, w: f32, h: f32) {
        let dst = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_draw_image(self.raw, image.raw, dst, std::ptr::null()) };
    }

    /// Draw an alpha-free RGB image, forcing opaque output and replacing the
    /// destination. This is the correct path for XRGB/XBGR DMA-BUF imports.
    pub fn draw_image_opaque(&self, image: &Image, x: f32, y: f32, w: f32, h: f32) {
        let dst = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_draw_image_opaque(self.raw, image.raw, dst) };
    }

    /// Draw an image using the tint and fixed-function blend mode in `paint`.
    pub fn draw_image_with_paint(
        &self,
        image: &Image,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        paint: &Paint,
    ) {
        let dst = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_draw_image(self.raw, image.raw, dst, paint.as_raw()) };
    }

    /// Draw an image clipped to an antialiased rounded rectangle. Set
    /// `radius` to half the destination size for a circular portrait.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_image_rrect(&self, image: &Image, x: f32, y: f32, w: f32, h: f32, radius: f32) {
        let dst = sys::flux_rect { x, y, w, h };
        unsafe {
            sys::flux_canvas_draw_image_rrect(self.raw, image.raw, dst, radius, std::ptr::null())
        };
    }

    /// Draw an image through one antialiased rounded clip independent of its
    /// destination. Reuse the same clip for every image in a composed surface
    /// tree so only the complete preview receives rounded corners.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_image_clipped_rrect_with_paint(
        &self,
        image: &Image,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        clip_x: f32,
        clip_y: f32,
        clip_w: f32,
        clip_h: f32,
        radius: f32,
        paint: &Paint,
    ) {
        let dst = sys::flux_rect { x, y, w, h };
        let clip = sys::flux_rect {
            x: clip_x,
            y: clip_y,
            w: clip_w,
            h: clip_h,
        };
        unsafe {
            sys::flux_canvas_draw_image_clipped_rrect(
                self.raw,
                image.raw,
                dst,
                clip,
                radius,
                paint.as_raw(),
            )
        };
    }

    /// Draw a sub-rectangle of `image` into `dst`. `src` is the sampled
    /// region in normalised texture coordinates `{u, v, du, dv}` where
    /// `(0.0, 0.0, 1.0, 1.0)` samples the whole image. Used for
    /// `wp_viewport.set_source` source-crop without a tint.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_image_sub(
        &self,
        image: &Image,
        dst_x: f32,
        dst_y: f32,
        dst_w: f32,
        dst_h: f32,
        src_u: f32,
        src_v: f32,
        src_du: f32,
        src_dv: f32,
    ) {
        let dst = sys::flux_rect {
            x: dst_x,
            y: dst_y,
            w: dst_w,
            h: dst_h,
        };
        let src = sys::flux_rect {
            x: src_u,
            y: src_v,
            w: src_du,
            h: src_dv,
        };
        unsafe { sys::flux_canvas_draw_image_sub(self.raw, image.raw, dst, src) };
    }

    /// Draw a sub-rectangle of an alpha-free RGB image, SRC-replace with
    /// alpha forced opaque (no destination read). Combines the source-crop of
    /// [`Self::draw_image_sub`] with the opaque behaviour of
    /// [`Self::draw_image_opaque`]. For XRGB/XBGR dma-bufs using
    /// `wp_viewport.set_source`.
    #[allow(clippy::too_many_arguments)]
    pub fn draw_image_opaque_sub(
        &self,
        image: &Image,
        dst_x: f32,
        dst_y: f32,
        dst_w: f32,
        dst_h: f32,
        src_u: f32,
        src_v: f32,
        src_du: f32,
        src_dv: f32,
    ) {
        let dst = sys::flux_rect {
            x: dst_x,
            y: dst_y,
            w: dst_w,
            h: dst_h,
        };
        let src = sys::flux_rect {
            x: src_u,
            y: src_v,
            w: src_du,
            h: src_dv,
        };
        unsafe { sys::flux_canvas_draw_image_opaque_sub(self.raw, image.raw, dst, src) };
    }

    pub fn as_raw(&self) -> *mut sys::flux_canvas {
        self.raw
    }
}

/// One colour sample in a canvas gradient.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GradientStop {
    pub offset: f32,
    pub color: u32,
}

impl GradientStop {
    /// Create a stop, clamping its offset to the gradient's `[0, 1]` domain.
    pub fn new(offset: f32, color: u32) -> Self {
        Self {
            offset: if offset.is_finite() {
                offset.clamp(0.0, 1.0)
            } else {
                0.0
            },
            color,
        }
    }
}

fn gradient_stops(stops: &[GradientStop]) -> Vec<sys::flux_gradient_stop> {
    stops
        .iter()
        .take(8)
        .map(|stop| sys::flux_gradient_stop {
            t: stop.offset,
            color: stop.color,
        })
        .collect()
}

impl Drop for Canvas {
    fn drop(&mut self) {
        // Only destroy when this handle actually owns the canvas. A view built
        // via `borrow_raw` leaves the real owner in charge.
        if !self.borrowed {
            unsafe { sys::flux_canvas_destroy(self.raw) };
        }
    }
}

/// Convert straight RGBA components (0–255) into flux's premultiplied
/// `flux_color` representation.
///
/// Canvas blending is premultiplied SRC_OVER, so public callers should pass
/// ordinary straight-alpha colour components here and let the binding enforce
/// the storage contract. Calling the raw `flux_color_rgba` packer with a
/// translucent colour would otherwise create bright opaque-looking fringes.
pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> u32 {
    unsafe { sys::flux_color_rgba_premul(r, g, b, a) }
}

/// A CPU-side bump allocator (`flux_arena`). flux's per-frame value types —
/// paths, and the glyph quads that flux-text emits — are allocated from one of
/// these and discarded wholesale via [`Arena::reset`] each frame.
///
/// The handle owns a stable heap allocation for the underlying `flux_arena`
/// struct so its address can be handed to C (`as_raw`) and stay put.
pub struct Arena {
    raw: *mut sys::flux_arena,
}

impl Arena {
    /// Create an arena backed by `capacity` bytes from the default (libc)
    /// allocator.
    pub fn with_capacity(capacity: usize) -> Result<Arena, Error> {
        // Box gives the flux_arena struct a fixed address for the C side.
        let raw = Box::into_raw(Box::new(unsafe { std::mem::zeroed::<sys::flux_arena>() }));
        let rc = unsafe { sys::flux_arena_init(raw, capacity, std::ptr::null()) };
        if let Err(e) = Error::check(rc) {
            // Reclaim the box; init failed so there is nothing to destroy.
            drop(unsafe { Box::from_raw(raw) });
            return Err(e);
        }
        Ok(Arena { raw })
    }

    /// Drop everything allocated since creation/last reset, keeping the buffer.
    pub fn reset(&self) {
        unsafe { sys::flux_arena_reset(self.raw) };
    }

    /// The underlying `flux_arena` pointer. Borrowed; the `Arena` retains
    /// ownership. Hand this to flux-text (cast to its ABI-identical
    /// `flux_arena` type at the binding seam).
    pub fn as_raw(&self) -> *mut sys::flux_arena {
        self.raw
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: raw came from Box::into_raw and was initialised by
        // flux_arena_init; destroy frees the buffer, then we reclaim the box.
        unsafe {
            sys::flux_arena_destroy(self.raw);
            drop(Box::from_raw(self.raw));
        }
    }
}

// =====================================================================
//  Paint + Path
// =====================================================================

/// Fixed-function blend mode used by Canvas paints.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum BlendMode {
    #[default]
    SrcOver,
    Src,
    Plus,
    Multiply,
}

impl BlendMode {
    fn raw(self) -> sys::flux_blend_mode {
        match self {
            Self::SrcOver => sys::flux_blend_mode::FLUX_BLEND_SRC_OVER,
            Self::Src => sys::flux_blend_mode::FLUX_BLEND_SRC,
            Self::Plus => sys::flux_blend_mode::FLUX_BLEND_PLUS,
            Self::Multiply => sys::flux_blend_mode::FLUX_BLEND_MULTIPLY,
        }
    }
}

/// How the open ends of a stroked subpath are rendered
/// (mirrors `flux_line_cap`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LineCap {
    /// Flat edge at the endpoint.
    #[default]
    Butt,
    /// Semicircular cap extending past the endpoint.
    Round,
    /// Square cap extending past the endpoint.
    Square,
}

impl LineCap {
    fn raw(self) -> sys::flux_line_cap {
        match self {
            LineCap::Butt => sys::flux_line_cap::FLUX_CAP_BUTT,
            LineCap::Round => sys::flux_line_cap::FLUX_CAP_ROUND,
            LineCap::Square => sys::flux_line_cap::FLUX_CAP_SQUARE,
        }
    }
}

/// How the corners of a stroked path are rendered
/// (mirrors `flux_line_join`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LineJoin {
    /// Sharp corner, clipped at the miter limit.
    #[default]
    Miter,
    /// Rounded corner.
    Round,
    /// Bevelled corner.
    Bevel,
}

impl LineJoin {
    fn raw(self) -> sys::flux_line_join {
        match self {
            LineJoin::Miter => sys::flux_line_join::FLUX_JOIN_MITER,
            LineJoin::Round => sys::flux_line_join::FLUX_JOIN_ROUND,
            LineJoin::Bevel => sys::flux_line_join::FLUX_JOIN_BEVEL,
        }
    }
}

/// How a surface is coloured when filling or stroking (mirrors
/// `flux_paint`). Construct via [`Paint::solid`]; stroke parameters are
/// set with the `with_*` builders. The C defaults are a 1px butt/miter
/// stroke with src-over blending.
#[derive(Clone, Copy)]
pub struct Paint {
    raw: sys::flux_paint,
}

impl std::fmt::Debug for Paint {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Paint")
            .field("color", &self.raw.color)
            .field("stroke_width", &self.raw.stroke_width)
            .finish_non_exhaustive()
    }
}

impl Paint {
    /// A flat premultiplied colour.
    pub fn solid(color: u32) -> Paint {
        Paint {
            raw: unsafe { sys::flux_paint_solid(color) },
        }
    }

    /// Stroke width in canvas units (default 1.0).
    pub fn with_stroke(mut self, width: f32) -> Self {
        self.raw.stroke_width = width;
        self
    }

    /// Stroke end cap (default [`LineCap::Butt`]).
    pub fn with_cap(mut self, cap: LineCap) -> Self {
        self.raw.cap = cap.raw();
        self
    }

    /// Stroke corner join (default [`LineJoin::Miter`]).
    pub fn with_join(mut self, join: LineJoin) -> Self {
        self.raw.join = join.raw();
        self
    }

    /// Select the fixed-function blend operation for this paint.
    pub fn with_blend(mut self, blend: BlendMode) -> Self {
        self.raw.blend = blend.raw();
        self
    }

    /// Borrow the raw C paint.
    pub fn as_raw(&self) -> &sys::flux_paint {
        &self.raw
    }
}

/// A vector path allocated from an [`Arena`] (mirrors `flux_path`).
///
/// The path borrows its arena: it is freed wholesale by [`Arena::reset`]
/// or drop, so a `Path` must not outlive the frame's arena cycle. Mutators
/// take `&self` because the path is a C-side growable buffer; overflow is
/// reported by [`Path::dropped_count`].
pub struct Path {
    raw: *mut sys::flux_path,
}

impl Path {
    /// Create an empty path in `arena`.
    pub fn new(arena: &Arena) -> Result<Path, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_path_create(&mut out, arena.as_raw()) })?;
        Ok(Path { raw: out })
    }

    /// Begin a new subpath at `(x, y)`.
    pub fn move_to(&self, x: f32, y: f32) -> &Self {
        unsafe { sys::flux_path_move_to(self.raw, x, y) };
        self
    }

    /// Append a straight segment to `(x, y)`.
    pub fn line_to(&self, x: f32, y: f32) -> &Self {
        unsafe { sys::flux_path_line_to(self.raw, x, y) };
        self
    }

    /// Append a quadratic Bézier with control point `(cx, cy)`.
    pub fn quad_to(&self, cx: f32, cy: f32, x: f32, y: f32) -> &Self {
        unsafe { sys::flux_path_quad_to(self.raw, cx, cy, x, y) };
        self
    }

    /// Append a cubic Bézier with control points `(c1x, c1y)`, `(c2x, c2y)`.
    pub fn cubic_to(&self, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) -> &Self {
        unsafe { sys::flux_path_cubic_to(self.raw, c1x, c1y, c2x, c2y, x, y) };
        self
    }

    /// Close the current subpath with a straight segment to its start.
    pub fn close(&self) -> &Self {
        unsafe { sys::flux_path_close(self.raw) };
        self
    }

    /// Append an axis-aligned rectangle subpath.
    pub fn add_rect(&self, x: f32, y: f32, w: f32, h: f32) -> &Self {
        unsafe { sys::flux_path_add_rect(self.raw, sys::flux_rect { x, y, w, h }) };
        self
    }

    /// Append a rounded-rectangle subpath.
    pub fn add_round_rect(&self, x: f32, y: f32, w: f32, h: f32, radius: f32) -> &Self {
        unsafe { sys::flux_path_add_round_rect(self.raw, sys::flux_rect { x, y, w, h }, radius) };
        self
    }

    /// Append a circle subpath.
    pub fn add_circle(&self, cx: f32, cy: f32, radius: f32) -> &Self {
        unsafe { sys::flux_path_add_circle(self.raw, cx, cy, radius) };
        self
    }

    /// Segments rejected due to arena exhaustion; non-zero means draws
    /// with this path silently dropped data (grow the arena).
    pub fn dropped_count(&self) -> u32 {
        unsafe { sys::flux_path_dropped_count(self.raw) }
    }
}

fn image_data_len(width: u32, height: u32, format: Format) -> Result<usize, Error> {
    let bytes_per_pixel = match format {
        Format::FLUX_FORMAT_R8_UNORM => 1usize,
        Format::FLUX_FORMAT_RGBA8_UNORM
        | Format::FLUX_FORMAT_BGRA8_UNORM
        | Format::FLUX_FORMAT_RGBA8_SRGB
        | Format::FLUX_FORMAT_BGRA8_SRGB => 4usize,
        _ => return Err(Error(sys::flux_result::FLUX_ERROR_UNSUPPORTED)),
    };
    (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(bytes_per_pixel))
        .ok_or(Error(sys::flux_result::FLUX_ERROR_OUT_OF_RANGE))
}

/// A GPU texture sampled by the canvas. Refcounted in C; this handle owns one
/// reference.
pub struct Image {
    raw: *mut sys::flux_image,
}

impl Image {
    /// Create a color image for an offscreen Canvas or scene attachment. Its
    /// initial contents are undefined; finishing its first target pass makes
    /// it sampleable.
    pub fn render_target(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
    ) -> Result<Image, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe {
            sys::flux_image_create_render_target(device.raw, width, height, format, &mut out)
        })?;
        Ok(Image { raw: out })
    }

    /// Create an image from tightly packed pixel data. `data` must be exactly
    /// `width * height * bytes_per_pixel(format)` bytes; the pixel bytes are
    /// copied before this returns and may be freed immediately. The upload
    /// itself is deferred: it is ordered before any later work on the same
    /// queue, so the image is safe to draw with right away.
    pub fn from_bytes(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        data: &[u8],
    ) -> Result<Image, Error> {
        let expected = image_data_len(width, height, format)?;
        if data.len() != expected {
            return Err(Error(sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT));
        }

        let desc = sys::flux_image_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_IMAGE_DESC,
            width,
            height,
            format,
            initial_data: data.as_ptr() as *const std::os::raw::c_void,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_image = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_image_create(device.raw, &desc, &mut out) })?;
        Ok(Image { raw: out })
    }

    /// Raw `flux_image` pointer. For passing the texture to a consumer that
    /// lives in a separate binding crate (e.g. lens's `lens_image`) where the
    /// `flux_image` typedef is bindgen-generated independently.
    pub fn as_raw(&self) -> *mut sys::flux_image {
        self.raw
    }

    pub fn size(&self) -> (u32, u32) {
        unsafe {
            (
                sys::flux_image_width(self.raw),
                sys::flux_image_height(self.raw),
            )
        }
    }

    pub fn format(&self) -> Format {
        unsafe { sys::flux_image_format(self.raw) }
    }
}

impl Image {
    /// Import a single-plane Linux dma-buf as a sampled image (zero-copy).
    ///
    /// On success flux takes ownership of `fd` and closes it; on error the
    /// caller retains `fd`. The device must have been created with the dma-buf
    /// import extensions (see [`dmabuf_supported`]).
    ///
    /// # Safety
    /// `fd` must be a valid dma-buf file descriptor whose contents match
    /// `width`/`height`/`format`/`modifier`/`offset`/`stride`.
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn import_dmabuf(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: i32,
        offset: u32,
        stride: u32,
    ) -> Result<Image, Error> {
        // SAFETY: the caller guarantees the dma-buf descriptor requirements
        // documented by this function; all values are forwarded unchanged.
        unsafe {
            Self::import_dmabuf_impl(
                device, width, height, format, modifier, fd, offset, stride, None,
            )
        }
    }

    /// Import a dma-buf and wait a Linux `sync_file` acquire fence before the
    /// image becomes sampleable. On success Flux owns both file descriptors;
    /// on error ownership remains with the caller.
    ///
    /// # Safety
    /// The dma-buf metadata must describe `fd`, and `acquire_sync_fd` must be
    /// a valid sync-file descriptor for the producer operation.
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn import_dmabuf_with_acquire_fence(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: i32,
        offset: u32,
        stride: u32,
        acquire_sync_fd: i32,
    ) -> Result<Image, Error> {
        // SAFETY: the caller guarantees both file descriptors and the dma-buf
        // metadata meet the requirements documented by this function.
        unsafe {
            Self::import_dmabuf_impl(
                device,
                width,
                height,
                format,
                modifier,
                fd,
                offset,
                stride,
                Some(acquire_sync_fd),
            )
        }
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn import_dmabuf_impl(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: i32,
        offset: u32,
        stride: u32,
        acquire_sync_fd: Option<i32>,
    ) -> Result<Image, Error> {
        let mut desc = sys::flux_dmabuf_image_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DMABUF_IMAGE_DESC,
            width,
            height,
            format,
            modifier,
            plane_count: 1,
            // SAFETY: the bindgen descriptor is a C POD type; zero is the
            // documented default for fields not initialized above.
            ..unsafe { std::mem::zeroed() }
        };
        desc.planes[0] = sys::flux_dmabuf_plane { fd, offset, stride };
        if let Some(acquire_sync_fd) = acquire_sync_fd {
            desc.has_acquire_sync_fd = true;
            desc.acquire_sync_fd = acquire_sync_fd;
        }
        let mut out: *mut sys::flux_image = std::ptr::null_mut();
        // SAFETY: the caller of this function guarantees that `desc` describes
        // valid dma-buf and optional sync-file descriptors.
        Error::check(unsafe { sys::flux_image_import_dmabuf(device.raw, &desc, &mut out) })?;
        Ok(Image { raw: out })
    }

    /// Replace a sub-rectangle of this image's contents with `data`.
    ///
    /// `data` must hold exactly `w * h * bytes_per_pixel` bytes in the
    /// image's native format (typically BGRA8 = 4 bytes/pixel). The
    /// sub-rect `(x, y, w, h)` must be inside the image's bounds. Used by
    /// compositors that track per-commit damage and want to avoid
    /// re-uploading the whole texture on small updates.
    pub fn update_region(&self, x: u32, y: u32, w: u32, h: u32, data: &[u8]) -> Result<(), Error> {
        Error::check(unsafe {
            sys::flux_image_update_region(
                self.raw,
                x,
                y,
                w,
                h,
                data.as_ptr() as *const _,
                data.len(),
            )
        })
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        unsafe { sys::flux_image_release(self.raw) };
    }
}

/// Reusable fixed-cost blur with a two-level Dual-Kawase pyramid per
/// frame-in-flight slot. This is the live-compositor path: blur width changes
/// sample offsets rather than dynamic kernel length, repeated calls do not
/// grow the transient effect pool, and no device-wide wait is required.
pub struct BlurFilter {
    raw: *mut sys::flux_blur_filter,
}

/// Input-pixel region whose realtime blur output must be valid. Regions may
/// be disjoint; pixels outside them are left untouched in the reusable effect
/// images. Callers should include the blur sampling footprint around the
/// visible destination.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BlurRegion {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}

impl BlurFilter {
    pub fn new(device: &Device) -> Result<BlurFilter, Error> {
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_blur_filter_create(device.raw, &mut raw) })?;
        Ok(BlurFilter { raw })
    }

    /// Record a realtime blur for this frame's reusable slot. `sigma` controls
    /// the visual blur width, but does not increase the number of texture
    /// samples. The returned image borrows the filter, preventing that slot's
    /// storage from being replaced while it is composed into the frame.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &Frame<'_>,
        input: &Image,
        sigma: f32,
    ) -> Result<BlurredImage<'filter>, Error> {
        self.apply_regions(frame, input, sigma, &[])
    }

    /// Record a realtime blur only for `regions`. An empty list preserves the
    /// full-image behaviour of [`Self::apply`]. Each region is mapped outward
    /// through the fixed two-level pyramid, so separated compositor chrome
    /// bands do not pay for the empty bounding box between them.
    pub fn apply_regions<'filter>(
        &'filter mut self,
        frame: &Frame<'_>,
        input: &Image,
        sigma: f32,
        regions: &[BlurRegion],
    ) -> Result<BlurredImage<'filter>, Error> {
        let raw_regions: Vec<sys::flux_effect_region> = regions
            .iter()
            .map(|region| sys::flux_effect_region {
                x: region.x,
                y: region.y,
                width: region.width,
                height: region.height,
            })
            .collect();
        let region_count = u32::try_from(raw_regions.len())
            .map_err(|_| Error(sys::flux_result::FLUX_ERROR_OUT_OF_RANGE))?;
        let region_desc = sys::flux_effect_blur_regions_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_EFFECT_BLUR_REGIONS_DESC,
            regions: if raw_regions.is_empty() {
                std::ptr::null()
            } else {
                raw_regions.as_ptr()
            },
            region_count,
            ..Default::default()
        };
        let desc = sys::flux_effect_blur_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_EFFECT_BLUR_DESC,
            next: if raw_regions.is_empty() {
                std::ptr::null()
            } else {
                (&region_desc as *const sys::flux_effect_blur_regions_desc).cast()
            },
            input: input.raw,
            sigma,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_blur_filter_apply(self.raw, frame.raw, &desc, &mut raw) })?;
        Ok(BlurredImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for BlurFilter {
    fn drop(&mut self) {
        unsafe { sys::flux_blur_filter_release(self.raw) };
    }
}

/// Borrowed output of a [`BlurFilter`] application.
pub struct BlurredImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut BlurFilter>,
}

impl BlurredImage<'_> {
    /// Draw the blurred output through the Canvas image pipeline.
    pub fn draw(&self, canvas: &Canvas, x: f32, y: f32, width: f32, height: f32) {
        let destination = sys::flux_rect {
            x,
            y,
            w: width,
            h: height,
        };
        unsafe { sys::flux_canvas_draw_image(canvas.raw, self.raw, destination, std::ptr::null()) };
    }
}

/// One rounded-rectangle volume in backdrop-capture pixel coordinates.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LiquidGlassShape {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub corner_radius: f32,
}

/// One soft optical emphasis field inside an existing glass body.
///
/// The field changes local clarity and directional light; it does not create
/// another SDF body. Its shape must remain inside the primary body's bounds,
/// and it is mutually exclusive with a merged secondary shape.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LiquidGlassFocus {
    pub shape: LiquidGlassShape,
    pub strength: f32,
}

/// One independently composited glass body. `merged` is smoothly unioned
/// with `primary`, which is useful for spring-driven droplets and controls.
///
/// Per-body optical character is caller policy, used verbatim: the drop
/// shadow (alpha 0 disables it), `tint_color`, an RGB multiplier on the
/// adaptive body tint for accent-tinted glass (`[255, 255, 255]` = neutral),
/// and an optional single-body optical `focus` field. Focus and `merged` are
/// mutually exclusive.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LiquidGlassGroup {
    pub primary: LiquidGlassShape,
    pub merged: Option<LiquidGlassShape>,
    pub blend_radius: f32,
    pub opacity: f32,
    pub shadow_alpha: f32,
    pub shadow_blur: f32,
    pub shadow_offset_y: f32,
    pub tint_color: [u8; 3],
    pub focus: Option<LiquidGlassFocus>,
}

/// Optical properties shared by all bodies in one liquid-glass dispatch.
/// Drop shadows are per body — see [`LiquidGlassGroup`].
///
/// Every policy knob lives here or on the group; only the curve shapes
/// (lens profile, falloff curves) are the material's identity and stay in
/// the shader.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LiquidGlassParams {
    pub refraction: f32,
    pub chromatic_aberration: f32,
    pub saturation: f32,
    pub brightness: f32,
    pub edge_width: f32,
    pub glare: f32,
    pub light_direction: (f32, f32),
    pub opacity: f32,
    /// Body small-side size (px) at which rim and lensing render at full
    /// strength; smaller bodies scale them down. 0 disables scaling.
    pub size_reference: f32,
    /// Floor of the size-scaling factor.
    pub size_scale_min: f32,
    /// Multiplier on the adaptive body tint (1.0 = reference recipe).
    pub tint_strength: f32,
    /// Multiplier on the scattering layer (1.0 = reference recipe).
    pub frost_strength: f32,
}

impl Default for LiquidGlassParams {
    fn default() -> Self {
        Self {
            refraction: 8.0,
            chromatic_aberration: 1.25,
            saturation: 1.08,
            brightness: 1.02,
            edge_width: 18.0,
            glare: 0.55,
            light_direction: (-0.45, -0.89),
            opacity: 1.0,
            size_reference: 72.0,
            size_scale_min: 0.15,
            tint_strength: 1.0,
            frost_strength: 1.0,
        }
    }
}

/// Reusable analytic liquid-glass compositor with one output per frame slot.
pub struct LiquidGlassFilter {
    raw: *mut sys::flux_liquid_glass_filter,
}

impl LiquidGlassFilter {
    pub fn new(device: &Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_liquid_glass_filter_create(device.raw, &mut raw) })?;
        Ok(Self { raw })
    }

    /// Refract `input` through analytic rounded SDFs, mixing in the matching
    /// realtime `blurred` capture for local frost. The returned image is
    /// transparent outside the SDF and its drop-shadow falloff, and borrows
    /// this filter's frame slot. An empty `groups` slice clears footprints
    /// retained by that slot after all glass bodies disappear.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &Frame<'_>,
        input: &Image,
        blurred: &BlurredImage<'_>,
        groups: &[LiquidGlassGroup],
        params: LiquidGlassParams,
    ) -> Result<LiquidGlassImage<'filter>, Error> {
        let raw_shape = |shape: LiquidGlassShape| sys::flux_liquid_glass_shape {
            bounds: sys::flux_rect {
                x: shape.x,
                y: shape.y,
                w: shape.width,
                h: shape.height,
            },
            corner_radius: shape.corner_radius,
        };
        let raw_groups: Vec<sys::flux_liquid_glass_group> = groups
            .iter()
            .map(|group| {
                let mut shapes = [raw_shape(group.primary), raw_shape(group.primary)];
                let shape_count = if let Some(merged) = group.merged {
                    shapes[1] = raw_shape(merged);
                    2
                } else {
                    1
                };
                sys::flux_liquid_glass_group {
                    shapes,
                    shape_count,
                    blend_radius: group.blend_radius,
                    opacity: group.opacity,
                    shadow_alpha: group.shadow_alpha,
                    shadow_blur: group.shadow_blur,
                    shadow_offset_y: group.shadow_offset_y,
                    tint_color: (u32::from(group.tint_color[0]) << 16)
                        | (u32::from(group.tint_color[1]) << 8)
                        | u32::from(group.tint_color[2]),
                    focus: group
                        .focus
                        .map(|focus| raw_shape(focus.shape))
                        .unwrap_or_else(|| raw_shape(group.primary)),
                    focus_strength: group.focus.map_or(0.0, |focus| focus.strength),
                }
            })
            .collect();
        let desc = sys::flux_liquid_glass_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_LIQUID_GLASS_DESC,
            input: input.raw,
            blurred_input: blurred.raw,
            groups: if raw_groups.is_empty() {
                std::ptr::null()
            } else {
                raw_groups.as_ptr()
            },
            group_count: u32::try_from(raw_groups.len()).unwrap_or(u32::MAX),
            refraction: params.refraction,
            chromatic_aberration: params.chromatic_aberration,
            saturation: params.saturation,
            brightness: params.brightness,
            edge_width: params.edge_width,
            glare: params.glare,
            light_direction: sys::flux_point {
                x: params.light_direction.0,
                y: params.light_direction.1,
            },
            opacity: params.opacity,
            size_reference: params.size_reference,
            size_scale_min: params.size_scale_min,
            tint_strength: params.tint_strength,
            frost_strength: params.frost_strength,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe {
            sys::flux_liquid_glass_filter_apply(self.raw, frame.raw, &desc, &mut raw)
        })?;
        Ok(LiquidGlassImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for LiquidGlassFilter {
    fn drop(&mut self) {
        unsafe { sys::flux_liquid_glass_filter_release(self.raw) };
    }
}

/// Borrowed full-capture liquid-glass composite.
pub struct LiquidGlassImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut LiquidGlassFilter>,
}

impl LiquidGlassImage<'_> {
    pub fn draw(&self, canvas: &Canvas, x: f32, y: f32, width: f32, height: f32) {
        let destination = sys::flux_rect {
            x,
            y,
            w: width,
            h: height,
        };
        unsafe { sys::flux_canvas_draw_image(canvas.raw, self.raw, destination, std::ptr::null()) };
    }
}

/// Whether `device` supports dma-buf import (was created with the required
/// Vulkan extensions). A compositor checks this before advertising
/// `zwp_linux_dmabuf_v1` to clients.
pub fn dmabuf_supported(device: &Device) -> bool {
    unsafe { sys::flux_dmabuf_supported(device.raw) }
}

/// Whether this device can import/export Linux sync_file fences alongside
/// dma-bufs through `VK_KHR_external_semaphore_fd`.
pub fn dmabuf_sync_supported(device: &Device) -> bool {
    unsafe { sys::flux_dmabuf_sync_supported(device.raw) }
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
    let rc = unsafe {
        sys::flux_dmabuf_format_modifiers(device.raw, format, std::ptr::null_mut(), &mut count)
    };
    // FLUX_OK with count 0 (no qualifiers, or device lacks dma-buf) is the
    // common "nothing to advertise" path. An unsupported format is also mapped
    // to empty here so the caller's format loop is uniform.
    if rc != sys::flux_result::FLUX_OK && rc != sys::flux_result::FLUX_ERROR_INVALID_ARGUMENT {
        return Vec::new();
    }
    if count == 0 {
        return Vec::new();
    }
    let mut mods = vec![0u64; count as usize];
    let rc = unsafe {
        sys::flux_dmabuf_format_modifiers(device.raw, format, mods.as_mut_ptr(), &mut count)
    };
    if rc != sys::flux_result::FLUX_OK {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn drm_node_maps_to_strict_device_extension() {
        let raw = DrmNode {
            major: 226,
            minor: 128,
        }
        .raw();
        assert_eq!(
            raw.type_,
            sys::flux_struct_type::FLUX_TYPE_DEVICE_DRM_NODE_DESC
        );
        assert!(raw.next.is_null());
        assert_eq!(raw.drm_major, 226);
        assert_eq!(raw.drm_minor, 128);
    }

    #[test]
    fn semantic_device_features_compose_without_vulkan_names() {
        let features = DeviceFeatures::DMABUF | DeviceFeatures::DMABUF_SYNC_FILE;
        assert!(features.contains(DeviceFeatures::DMABUF));
        assert!(features.contains(DeviceFeatures::DMABUF_SYNC_FILE));
        assert!(!DeviceFeatures::NONE.contains(DeviceFeatures::DMABUF));
    }

    #[test]
    fn image_data_len_validates_supported_formats() {
        assert_eq!(
            image_data_len(3, 2, Format::FLUX_FORMAT_R8_UNORM).unwrap(),
            6
        );
        assert_eq!(
            image_data_len(3, 2, Format::FLUX_FORMAT_RGBA8_UNORM).unwrap(),
            24
        );
        assert_eq!(
            image_data_len(1, 1, Format::FLUX_FORMAT_RGBA16_SFLOAT).unwrap_err(),
            Error(sys::flux_result::FLUX_ERROR_UNSUPPORTED)
        );
    }

    #[test]
    fn readback_region_byte_len_is_checked() {
        assert_eq!(
            ReadbackRegion {
                x: 0,
                y: 0,
                width: 0,
                height: 7,
            }
            .byte_len(),
            None
        );
        assert_eq!(
            ReadbackRegion {
                x: 17,
                y: 9,
                width: 12,
                height: 7,
            }
            .byte_len(),
            Some(12 * 7 * 4)
        );
        assert_eq!(
            ReadbackRegion {
                x: 0,
                y: 0,
                width: u32::MAX,
                height: u32::MAX,
            }
            .byte_len(),
            None
        );
    }
}
