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
    pub fn new(
        headless: bool,
        instance_extensions: &[&std::ffi::CStr],
        device_extensions: &[&std::ffi::CStr],
    ) -> Result<Device, Error> {
        let inst: Vec<*const std::os::raw::c_char> =
            instance_extensions.iter().map(|s| s.as_ptr()).collect();
        let dev: Vec<*const std::os::raw::c_char> =
            device_extensions.iter().map(|s| s.as_ptr()).collect();

        let desc = sys::flux_device_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DEVICE_DESC,
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
            headless,
            ..unsafe { std::mem::zeroed() }
        };

        let mut out: *mut sys::flux_device = std::ptr::null_mut();
        // SAFETY: desc is fully initialized; the extension pointers outlive this
        // call (the Vecs live to the end of the function).
        let rc = unsafe { sys::flux_device_create(&desc, &mut out) };
        Error::check(rc)?;
        debug_assert!(!out.is_null());
        Ok(Device { raw: out, borrowed: false })
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
        Device { raw, borrowed: true }
    }

    /// The underlying raw `flux_device` pointer. Borrowed; the `Device` retains
    /// ownership. Use this to hand the device to flux-ui (`Ui::with_device`),
    /// whose bindings declare a distinct-but-ABI-identical `flux_device` type —
    /// cast the pointer at that seam.
    pub fn as_raw(&self) -> *mut sys::flux_device {
        self.raw
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

/// A presentable surface wrapping a caller-supplied `VkSurfaceKHR`. Refcounted
/// in C; this handle owns one reference.
pub struct Surface {
    raw: *mut sys::flux_surface,
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
            ..std::mem::zeroed()
        };
        let mut out: *mut sys::flux_surface = std::ptr::null_mut();
        Error::check(sys::flux_surface_create(device.raw, &desc, &mut out))?;
        Ok(Surface { raw: out })
    }

    /// Create an OFFSCREEN surface (no window, no swapchain): flux owns RGBA8
    /// colour images at `width` x `height`. The frame loop is unchanged
    /// ([`Surface::begin_frame`] → record → [`Frame::submit`] →
    /// [`Frame::present`]); `present` completes the frame without presenting,
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

    /// Offscreen surfaces only: read back the most recently submitted frame as
    /// tightly packed RGBA8, row-major, top-left origin. `dst` must be at least
    /// `width * height * 4` bytes. Waits for the frame's GPU work to complete.
    /// Returns [`Error`] wrapping `FLUX_ERROR_UNSUPPORTED` on a windowed surface
    /// or `FLUX_ERROR_INVALID_STATE` before the first submitted frame.
    pub fn read_pixels(&self, dst: &mut [u8]) -> Result<(), Error> {
        Error::check(unsafe {
            sys::flux_surface_read_pixels(
                self.raw,
                dst.as_mut_ptr() as *mut std::os::raw::c_void,
                dst.len(),
            )
        })
    }

    /// Recreate the swapchain at a new extent. Safe to call from a resize event.
    pub fn resize(&self, width: u32, height: u32) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_surface_resize(self.raw, width, height) })
    }

    /// Current swapchain extent.
    pub fn size(&self) -> (u32, u32) {
        let mut info = sys::flux_surface_info::default();
        unsafe { sys::flux_surface_get_info(self.raw, &mut info) };
        (info.width, info.height)
    }

    /// Acquire the next frame. Returns the backend result so callers can handle
    /// `SURFACE_LOST` / out-of-date by resizing.
    pub fn begin_frame(&self) -> Result<Frame, Error> {
        let desc = sys::flux_frame_begin_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_FRAME_BEGIN_DESC,
            timeout_ns: 0,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::flux_frame = std::ptr::null_mut();
        Error::check(unsafe { sys::flux_surface_begin_frame(self.raw, &desc, &mut out) })?;
        Ok(Frame { raw: out })
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

/// A transient per-frame handle obtained from [`Surface::begin_frame`]. Not
/// refcounted: valid until [`Frame::present`]; it is a borrow into the
/// surface's swapchain ring.
pub struct Frame {
    raw: *mut sys::flux_frame,
}

impl Frame {
    /// Submit recorded work to the GPU.
    pub fn submit(&self) -> Result<(), Error> {
        Error::check(unsafe { sys::flux_frame_submit(self.raw) })
    }

    /// Present the submitted frame to the surface.
    pub fn present(&self) -> Result<(), Error> {
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
        Canvas { raw, borrowed: true }
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
        Ok(Canvas { raw: out, borrowed: false })
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
        Ok(Canvas { raw: out, borrowed: false })
    }

    /// Begin recording. `clear` clears the surface to a packed color; `None`
    /// loads the existing contents.
    pub fn begin(&self, frame: &Frame, clear: Option<u32>) -> Result<(), Error> {
        let color = clear; // flux_color is a packed u32
        let ptr = color
            .as_ref()
            .map(|c| c as *const u32)
            .unwrap_or(std::ptr::null());
        Error::check(unsafe { sys::flux_canvas_begin(self.raw, frame.raw, ptr) })
    }

    /// Unified, backend-agnostic pass bracket. Pass `Some(frame)` for a GPU
    /// canvas and `None` for a CPU canvas; the drawing code in between is
    /// identical either way. `clear` clears to a packed color, `None` loads.
    pub fn begin_frame(&self, frame: Option<&Frame>, clear: Option<u32>) -> Result<(), Error> {
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

    /// Draw an image into the destination rectangle (pixel space).
    pub fn draw_image(&self, image: &Image, x: f32, y: f32, w: f32, h: f32) {
        let dst = sys::flux_rect { x, y, w, h };
        unsafe { sys::flux_canvas_draw_image(self.raw, image.raw, dst, std::ptr::null()) };
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

    pub fn as_raw(&self) -> *mut sys::flux_canvas {
        self.raw
    }
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

/// Pack an RGBA color (0–255 components) into a `flux_color`.
pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> u32 {
    unsafe { sys::flux_color_rgba(r, g, b, a) }
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

/// A GPU texture sampled by the canvas. Refcounted in C; this handle owns one
/// reference.
pub struct Image {
    raw: *mut sys::flux_image,
}

impl Image {
    /// Create an image from tightly packed pixel data. `data` must be exactly
    /// `width * height * bytes_per_pixel(format)` bytes. The upload is
    /// synchronous; the data may be freed after this returns.
    pub fn from_bytes(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        data: &[u8],
    ) -> Result<Image, Error> {
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
        let mut desc = sys::flux_dmabuf_image_desc {
            type_: sys::flux_struct_type::FLUX_TYPE_DMABUF_IMAGE_DESC,
            width,
            height,
            format,
            modifier,
            plane_count: 1,
            ..std::mem::zeroed()
        };
        desc.planes[0] = sys::flux_dmabuf_plane { fd, offset, stride };
        let mut out: *mut sys::flux_image = std::ptr::null_mut();
        Error::check(sys::flux_image_import_dmabuf(device.raw, &desc, &mut out))?;
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

/// Whether `device` supports dma-buf import (was created with the required
/// Vulkan extensions). A compositor checks this before advertising
/// `zwp_linux_dmabuf_v1` to clients.
pub fn dmabuf_supported(device: &Device) -> bool {
    unsafe { sys::flux_dmabuf_supported(device.raw) }
}

/// The Vulkan device extensions dma-buf import requires. Pass to
/// [`Device::new`].
pub const DMABUF_DEVICE_EXTENSIONS: [&std::ffi::CStr; 5] = [
    c"VK_KHR_external_memory",
    c"VK_KHR_external_memory_fd",
    c"VK_EXT_external_memory_dma_buf",
    c"VK_EXT_image_drm_format_modifier",
    c"VK_EXT_queue_family_foreign",
];
