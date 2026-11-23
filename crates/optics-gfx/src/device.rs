//! Device selection and creation.

use super::*;

/// Linux DRM character-device identity used to constrain Vulkan physical
/// device selection. The node may be either a primary (`cardN`) or render
/// (`renderDN`) node; gfx accepts only the physical device that reports it
/// through `VK_EXT_physical_device_drm`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrmNode {
    pub major: u32,
    pub minor: u32,
}

impl DrmNode {
    pub(crate) fn raw(self) -> sys::optics_gfx_device_drm_node_desc {
        sys::optics_gfx_device_drm_node_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_DEVICE_DRM_NODE_DESC,
            next: std::ptr::null(),
            drm_major: self.major,
            drm_minor: self.minor,
        }
    }
}

/// DRM identities reported by the Vulkan physical device selected by gfx.
/// A display-capable device commonly exposes both a privileged primary node
/// and an unprivileged render node; either may be absent.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DrmDeviceIdentity {
    pub primary: Option<DrmNode>,
    pub render: Option<DrmNode>,
}

/// Semantic device capabilities requested independently of the Vulkan
/// extensions used by gfx's current backend.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct DeviceFeatures(u64);

impl DeviceFeatures {
    pub const NONE: Self = Self(0);
    pub const DMABUF: Self = Self(sys::OPTICS_GFX_DEVICE_FEATURE_DMABUF as u64);
    pub const DMABUF_SYNC_FILE: Self = Self(sys::OPTICS_GFX_DEVICE_FEATURE_DMABUF_SYNC_FILE as u64);

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

/// A gfx device: the root GPU object. Refcounted in C; this handle owns one
/// reference and releases it on drop — unless it was constructed via
/// [`Device::borrow_raw`], in which case it is a non-owning view and `Drop`
/// is a no-op.
pub struct Device {
    pub(crate) raw: *mut sys::optics_gfx_device,
    /// When `true`, this handle does **not** own a reference and must not
    /// release it on drop. Set by [`Device::borrow_raw`] for views over a
    /// device another owner created (e.g. window's `StartHost::optics_gfx_device`).
    borrowed: bool,
}

impl Device {
    /// Create a device. `instance_extensions` / `device_extensions` are extra
    /// Vulkan extensions to require at bootstrap (e.g. surface + platform
    /// surface extensions for a nested backend, or dmabuf import extensions).
    ///
    /// `frames_in_flight` sets how many frame slots (and, for offscreen
    /// surfaces, how many ring images) the device runs concurrently; pass 0
    /// for the gfx default (2). Values above `OPTICS_GFX_MAX_FRAMES_IN_FLIGHT` (3)
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
        .then(|| sys::optics_gfx_device_features_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_DEVICE_FEATURES_DESC,
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

        let desc = sys::optics_gfx_device_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_DEVICE_DESC,
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

        let mut out: *mut sys::optics_gfx_device = std::ptr::null_mut();
        // SAFETY: desc is fully initialized; the extension pointers outlive this
        // call (the Vecs live to the end of the function).
        let rc = unsafe { sys::optics_gfx_device_create(&desc, &mut out) };
        Error::check(rc)?;
        debug_assert!(!out.is_null());
        Ok(Device {
            raw: out,
            borrowed: false,
        })
    }

    /// Wrap a raw `optics_gfx_device*` as a non-owning view. Use this to obtain a
    /// `&Device` for a device another owner created and retains — the
    /// canonical case being window's `StartHost::optics_gfx_device()`, which hands the
    /// app the device window owns for its window. Opening a second device is
    /// unsupported and crashes, so a host that needs a `optics_text::Text`
    /// context (or any `&Device` consumer) inside window's lifecycle callbacks
    /// **must** borrow window's device rather than creating its own.
    ///
    /// The returned `Device` does not release on drop: the borrow is valid
    /// only as long as the real owner keeps the device alive (for window, the
    /// app's lifetime).
    ///
    /// # Safety
    /// `raw` must be a live `optics_gfx_device*` obtained from the gfx C API (or
    /// from another binding's ABI-identical opaque pointer, such as window's
    /// `StartHost::optics_gfx_device()`), and must remain valid for as long as the
    /// returned `Device` (or any `&Device` derived from it) is used.
    pub unsafe fn borrow_raw(raw: *mut sys::optics_gfx_device) -> Device {
        Device {
            raw,
            borrowed: true,
        }
    }

    /// The underlying raw `optics_gfx_device` pointer. Borrowed; the `Device` retains
    /// ownership. Use this to hand the device to gfx-ui (`Ui::with_device`),
    /// whose bindings declare a distinct-but-ABI-identical `optics_gfx_device` type —
    /// cast the pointer at that seam.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_device {
        self.raw
    }

    /// Semantic capabilities enabled on this logical device.
    pub fn enabled_features(&self) -> DeviceFeatures {
        DeviceFeatures(unsafe { sys::optics_gfx_device_enabled_features(self.raw) })
    }

    /// DRM primary/render identities reported by the selected Vulkan physical
    /// device. Returns `None` when the Vulkan driver exposes no reliable DRM
    /// identity.
    pub fn drm_identity(&self) -> Option<DrmDeviceIdentity> {
        let mut raw = sys::optics_gfx_drm_device_identity::default();
        if !unsafe { sys::optics_gfx_device_get_drm_identity(self.raw, &mut raw) } {
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

    /// Raw `VkInstance` gfx created. Feed to `ash` (as a `u64` via `as_raw`)
    /// when constructing a `VkSurfaceKHR` for a windowed backend.
    pub fn vk_instance(&self) -> sys::VkInstance {
        unsafe { sys::optics_gfx_device_vk_instance(self.raw) }
    }

    /// Raw Vulkan handle — for integrating native Vulkan code. The handle
    /// is owned by the device; do not destroy it.
    pub fn vk_physical_device(&self) -> sys::VkPhysicalDevice {
        unsafe { sys::optics_gfx_device_vk_physical_device(self.raw) }
    }

    /// Raw Vulkan device handle. Owned by gfx; do not destroy.
    pub fn vk_device(&self) -> sys::VkDevice {
        unsafe { sys::optics_gfx_device_vk_device(self.raw) }
    }

    /// Raw Vulkan graphics queue handle. Owned by gfx; do not destroy.
    pub fn vk_graphics_queue(&self) -> sys::VkQueue {
        unsafe { sys::optics_gfx_device_vk_graphics_queue(self.raw) }
    }

    /// Queue family index of the graphics queue.
    pub fn vk_graphics_family(&self) -> u32 {
        unsafe { sys::optics_gfx_device_vk_graphics_family(self.raw) }
    }

    /// Block until the device is idle. Call before tearing down surfaces.
    pub fn wait_idle(&self) {
        unsafe { sys::optics_gfx_device_wait_idle(self.raw) };
    }

    /// Open a batched-upload window. While a batch is open, texture uploads
    /// from `Image::from_bytes*`, `Image::update_region`, and mesh/buffer
    /// creation with initial data are recorded into one command buffer and
    /// submitted as a **single** `vkQueueSubmit` when the outermost guard
    /// returned by this method is dropped.
    ///
    /// Without a batch, every individual upload performs its own queue
    /// submit; hosts that refresh several textures per frame (compositors
    /// with continuously-updating SHM clients) pay a per-upload submission
    /// and command-pool recycling cost that this window removes.
    ///
    /// Scopes nest: a `uploads_begin` while a batch is already open joins
    /// that batch instead of failing, and only the outermost guard's drop
    /// (or explicit [`UploadBatch::flush`]) submits. A host that batches per
    /// draw pass therefore still produces one submission per frame, not one
    /// per pass — each recycled batch pool costs a `vkResetCommandPool` on
    /// the next frame's sweep, which dominates the frame path when it
    /// happens several times per frame.
    ///
    /// A batch left open holds its command buffer until the next flush, so
    /// keep the guard's lifetime scoped to the upload phase of a frame.
    /// Errors from the implicit flush at drop are swallowed; call
    /// [`UploadBatch::flush`] explicitly when they matter.
    pub fn uploads_begin(&self) -> Result<UploadBatch<'_>, Error> {
        Error::check(unsafe { sys::optics_gfx_uploads_begin(self.raw) })?;
        Ok(UploadBatch {
            raw: self.raw,
            flushed: false,
            _device: std::marker::PhantomData,
        })
    }
}

/// RAII guard for a `optics_gfx_uploads_begin` batch. Dropping the guard flushes
/// the batch: one command-buffer end plus one queue submit for every upload
/// recorded since `uploads_begin`.
pub struct UploadBatch<'a> {
    raw: *mut sys::optics_gfx_device,
    flushed: bool,
    /// Keep the device borrowed for `'a` so the batch cannot outlive it.
    _device: std::marker::PhantomData<&'a Device>,
}

impl UploadBatch<'_> {
    /// Close this scope. For an inner (nested) scope this is a no-op join;
    /// for the outermost scope it submits every upload recorded since the
    /// outermost `uploads_begin`. Safe to call more than once; only the
    /// first call closes the scope.
    pub fn flush(&mut self) -> Result<(), Error> {
        if self.flushed {
            return Ok(());
        }
        self.flushed = true;
        // SAFETY: `raw` came from a live `Device` reference that outlives
        // this guard; optics_gfx_uploads_flush on a closed batch is a no-op.
        Error::check(unsafe { sys::optics_gfx_uploads_flush(self.raw) })
    }
}

impl Drop for UploadBatch<'_> {
    fn drop(&mut self) {
        if !self.flushed {
            // SAFETY: see `flush`.
            let _ = self.flush();
        }
    }
}

impl Drop for Device {
    fn drop(&mut self) {
        // Only release when this handle actually owns a reference. A view
        // constructed via `borrow_raw` leaves the real owner in charge.
        if !self.borrowed {
            // SAFETY: we own one reference taken at create.
            unsafe { sys::optics_gfx_device_release(self.raw) };
        }
    }
}

// SAFETY: optics_gfx_device is free-threaded after creation; all internal queues,
// allocators, and heaps are protected by internal locks (docs/reference/thread-safety.md).
unsafe impl Send for Device {}
unsafe impl Sync for Device {}
