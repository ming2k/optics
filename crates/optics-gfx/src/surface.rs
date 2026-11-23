//! Surfaces, swapchain presentation, readback and dma-buf export.

use super::*;

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
    /// gfx frame-in-flight slot that owns this image.
    pub slot: u32,
}

/// HDR presentation tuning for [`SurfaceColorOptions`] (ADR-0069).
/// Meaningful only when the negotiated color space is HDR (PQ or HLG);
/// ignored otherwise.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SurfaceHdr {
    /// Where SDR white (working-space 1.0) lands on the HDR output, in
    /// cd/m². 0 selects the default 203 cd/m² (ITU-R BT.2408 graphics
    /// white). Raise toward the panel's SDR brightness when mixing with
    /// bright SDR windows.
    pub sdr_white_nits: f32,
    /// HDR10 static metadata (SMPTE ST 2086 / CTA-861.3). Applied to the
    /// swapchain when the device advertises `VK_EXT_hdr_metadata`.
    pub metadata: Option<Hdr10Metadata>,
}

/// Opaque external-image lease returned by a dma-buf export. The surface's
/// frame slot cannot be reacquired until this lease is dropped.
#[derive(Debug)]
pub struct DmabufLease {
    raw: *mut sys::optics_gfx_surface_dmabuf_lease,
}

impl DmabufLease {
    /// Release the external image lease and allow the ring slot to be reused.
    pub fn release(self) {
        drop(self);
    }
}

impl Drop for DmabufLease {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::optics_gfx_surface_dmabuf_lease_release(self.raw) };
            self.raw = std::ptr::null_mut();
        }
    }
}

/// One exported offscreen frame plus the image lease that must live until the
/// external consumer (DRM/KMS page flip, Wayland buffer release, …) is done.
#[derive(Debug)]
pub struct ExportedDmabuf {
    pub dmabuf: SurfaceDmabuf,
    pub lease: DmabufLease,
}

impl SurfaceHdr {
    pub(crate) fn raw(self) -> sys::optics_gfx_surface_hdr_desc {
        let mut desc = sys::optics_gfx_surface_hdr_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_HDR_DESC,
            next: std::ptr::null(),
            sdr_white_nits: self.sdr_white_nits,
            has_metadata: self.metadata.is_some(),
            ..Default::default()
        };
        if let Some(m) = self.metadata {
            desc.mastering = sys::optics_gfx_surface_hdr_desc__bindgen_ty_1 {
                rx: m.mastering.rx,
                ry: m.mastering.ry,
                gx: m.mastering.gx,
                gy: m.mastering.gy,
                bx: m.mastering.bx,
                by: m.mastering.by,
                wx: m.mastering.wx,
                wy: m.mastering.wy,
            };
            desc.max_luminance = m.max_luminance;
            desc.min_luminance = m.min_luminance;
            desc.max_cll = m.max_cll;
            desc.max_fall = m.max_fall;
        }
        desc
    }
}

/// HDR10 static metadata (SMPTE ST 2086 / CTA-861.3). Luminances are
/// plain cd/m² (the ST 2086 0.0001-unit encoding is *not* used here).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hdr10Metadata {
    /// Mastering display primaries + white, CIE 1931 xy.
    pub mastering: PrimariesXy,
    pub max_luminance: f32,
    pub min_luminance: f32,
    /// MaxCLL, cd/m².
    pub max_cll: f32,
    /// MaxFALL, cd/m².
    pub max_fall: f32,
}

/// Pins the space pixels are *written in* (the display's actual space),
/// decoupled from the swapchain's negotiated space — the legacy-platform
/// display-profile path (ADR-0069). Do NOT combine with color-managed
/// compositors (Wayland color-management, Windows ACM): negotiate the
/// true space via [`SurfaceColorOptions::color_spaces`] there instead.
#[derive(Debug, Clone, Copy)]
pub struct SurfaceOutputColor<'a> {
    /// The display's actual space.
    pub content_space: ColorSpace,
    /// Optional ICC profile for the display; must be
    /// parametric-extractable.
    pub icc: Option<&'a IccProfile>,
}

/// Color-management extensions for surface creation (ADR-0069). All
/// fields are optional and combine freely; the default keeps the legacy
/// `hdr_preferred` negotiation behavior.
#[derive(Debug, Default, Clone, Copy)]
pub struct SurfaceColorOptions<'a> {
    /// Presentation color spaces the application can present, most
    /// preferred first. gfx walks the list and picks the first space the
    /// swapchain can do; the winner is reported through
    /// [`SurfaceInfo::color_space`]. On an offscreen surface the first
    /// listed space with a workable container is adopted, the container
    /// following the space's transfer function (see `offscreen_formats`).
    pub color_spaces: &'a [ColorSpace],
    /// HDR presentation tuning; ignored for SDR spaces.
    pub hdr: Option<SurfaceHdr>,
    /// Pin the written space independent of the swapchain space.
    pub output_color: Option<SurfaceOutputColor<'a>>,
    /// Offscreen-only: constrain the pixel container, most preferred
    /// first. A format must suit the written space's transfer function:
    /// sRGB/gamma accept any colour format, PQ/HLG require
    /// [`Format::Rgb10a2Unorm`] or
    /// [`Format::Rgba16Sfloat`], linear requires
    /// `RGBA16_SFLOAT`. Empty keeps the transfer-derived defaults
    /// (BGRA8 for gamma encodings). Ignored on windowed surfaces.
    pub offscreen_formats: &'a [Format],
}

/// Negotiated surface state reported by [`Surface::info`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SurfaceInfo {
    pub width: u32,
    pub height: u32,
    pub image_count: u32,
    /// The surface is actually HDR (PQ, HLG, or extended-linear).
    pub hdr: bool,
    /// The color space the surface actually presents in (the negotiated
    /// swapchain space).
    pub color_space: ColorSpace,
    /// What pixels are written in: equals `color_space` unless a
    /// [`SurfaceOutputColor`] pinned the display's actual space.
    pub content_space: ColorSpace,
    /// Pixel format of the surface's images: the negotiated swapchain
    /// format on a windowed surface, the chosen container on an offscreen
    /// one. Compositors mapping the dma-buf export to a DRM fourcc key
    /// off this.
    pub format: Format,
}

/// A presentable or offscreen rendering surface. Refcounted in C; this handle
/// owns one reference.
pub struct Surface {
    pub(crate) raw: *mut sys::optics_gfx_surface,
}

/// An immutable completed frame snapshot detached from a [`Surface`].
///
/// The staging allocation is independently owned, so this handle can be sent
/// to a worker while the surface continues presenting or is resized.
pub struct Readback {
    raw: *mut sys::optics_gfx_readback,
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
    pub(crate) fn raw(self) -> sys::optics_gfx_readback_region {
        sys::optics_gfx_readback_region {
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
        let mut raw = sys::optics_gfx_readback_region::default();
        unsafe { sys::optics_gfx_readback_get_region(self.raw, &mut raw) };
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
            sys::optics_gfx_readback_read_pixels(
                self.raw,
                dst.as_mut_ptr() as *mut std::os::raw::c_void,
                dst.len(),
            )
        })
    }
}

impl Drop for Readback {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_readback_release(self.raw) };
    }
}

impl Surface {
    /// Wrap a `VkSurfaceKHR` (created by the caller, e.g. via `ash`) as a gfx
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
        let desc = sys::optics_gfx_surface_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DESC,
            vk_surface_khr,
            width,
            height,
            vsync,
            // SAFETY: the bindgen descriptor is a C POD type; zero is the
            // documented default for fields not initialized above.
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_surface = std::ptr::null_mut();
        // SAFETY: `device` is live and the caller upholds the VkSurfaceKHR
        // lifetime and instance requirements documented by this function.
        Error::check(unsafe { sys::optics_gfx_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Like [`Surface::from_vk`], plus color-management extensions
    /// (presentation-space preference list, HDR tuning, output-space
    /// override) — see [`SurfaceColorOptions`] (ADR-0069).
    ///
    /// # Safety
    /// Same contract as [`Surface::from_vk`].
    pub unsafe fn from_vk_with_color_options(
        device: &Device,
        vk_surface_khr: *mut std::os::raw::c_void,
        width: u32,
        height: u32,
        vsync: bool,
        options: SurfaceColorOptions<'_>,
    ) -> Result<Surface, Error> {
        Self::create_with_color_options(device, vk_surface_khr, width, height, vsync, options, None)
    }

    /// Like [`Surface::offscreen`], plus color-management extensions. An
    /// offscreen surface adopts the first listed space whose container is
    /// workable (deep-color/HDR spaces pick RGB10A2 or RGBA16F; see
    /// [`SurfaceColorOptions::offscreen_formats`]);
    /// [`SurfaceColorOptions::output_color`] is equivalent to requesting
    /// the space through `color_spaces`.
    pub fn offscreen_with_color_options(
        device: &Device,
        width: u32,
        height: u32,
        options: SurfaceColorOptions<'_>,
    ) -> Result<Surface, Error> {
        Self::create_with_color_options(
            device,
            std::ptr::null_mut(),
            width,
            height,
            false,
            options,
            None,
        )
    }

    /// Like [`Surface::offscreen_dmabuf`], plus color-management extensions
    /// (ADR-0069): negotiate the written color space and the pixel container
    /// — e.g. BT.2020 PQ into RGB10A2 for direct HDR scanout through
    /// DRM/KMS. Fails when no shared modifier supports a suitable container.
    pub fn offscreen_dmabuf_with_color_options(
        device: &Device,
        width: u32,
        height: u32,
        modifiers: &[u64],
        options: SurfaceColorOptions<'_>,
    ) -> Result<Surface, Error> {
        if modifiers.is_empty() {
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }
        let surface = Self::create_with_color_options(
            device,
            std::ptr::null_mut(),
            width,
            height,
            false,
            options,
            Some(modifiers),
        )?;
        if !surface.is_exportable() {
            return Err(Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_UNSUPPORTED));
        }
        Ok(surface)
    }

    /// Shared constructor for the color-managed surface entry points:
    /// builds the three optional stack-owned pNext extensions and links
    /// them into the base descriptor while every referenced value is
    /// alive (same pattern as [`Device::new_with_options`]).
    fn create_with_color_options(
        device: &Device,
        vk_surface_khr: *mut std::os::raw::c_void,
        width: u32,
        height: u32,
        vsync: bool,
        options: SurfaceColorOptions<'_>,
        dmabuf_modifiers: Option<&[u64]>,
    ) -> Result<Surface, Error> {
        let mut dmabuf_extension = dmabuf_modifiers
            .map(|modifiers| {
                Ok(sys::optics_gfx_surface_dmabuf_desc {
                    type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DMABUF_DESC,
                    next: std::ptr::null(),
                    modifiers: modifiers.as_ptr(),
                    modifier_count: modifiers.len().try_into().map_err(|_| {
                        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE)
                    })?,
                })
            })
            .transpose()?;
        let raw_spaces: Vec<sys::optics_gfx_color_space> =
            options.color_spaces.iter().map(|s| s.as_raw()).collect();
        let mut color_space_extension = (!raw_spaces.is_empty())
            .then(|| {
                Ok(sys::optics_gfx_surface_color_space_desc {
                    type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_COLOR_SPACE_DESC,
                    next: std::ptr::null(),
                    spaces: raw_spaces.as_ptr(),
                    space_count: raw_spaces.len().try_into().map_err(|_| {
                        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE)
                    })?,
                })
            })
            .transpose()?;
        let mut hdr_extension = options.hdr.map(SurfaceHdr::raw);
        let mut offscreen_format_extension = (!options.offscreen_formats.is_empty())
            .then(|| {
                Ok(sys::optics_gfx_surface_offscreen_format_desc {
                    type_:
                        sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_OFFSCREEN_FORMAT_DESC,
                    next: std::ptr::null(),
                    formats: options
                        .offscreen_formats
                        .iter()
                        .map(|f| f.raw())
                        .collect::<Vec<_>>()
                        .as_ptr(),
                    format_count: options.offscreen_formats.len().try_into().map_err(|_| {
                        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE)
                    })?,
                })
            })
            .transpose()?;
        let mut output_extension =
            options
                .output_color
                .map(|output| sys::optics_gfx_surface_output_color_desc {
                    type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_OUTPUT_COLOR_DESC,
                    next: std::ptr::null(),
                    content_space: output.content_space.as_raw(),
                    icc: output
                        .icc
                        .map(|p| p.as_raw() as *const sys::optics_gfx_icc_profile)
                        .unwrap_or(std::ptr::null()),
                });

        let mut next = std::ptr::null();
        if let Some(dmabuf) = dmabuf_extension.as_mut() {
            dmabuf.next = next;
            next = dmabuf as *const _ as *const std::ffi::c_void;
        }
        if let Some(offscreen_format) = offscreen_format_extension.as_mut() {
            offscreen_format.next = next;
            next = offscreen_format as *const _ as *const std::ffi::c_void;
        }
        if let Some(output) = output_extension.as_mut() {
            output.next = next;
            next = output as *const _ as *const std::ffi::c_void;
        }
        if let Some(hdr) = hdr_extension.as_mut() {
            hdr.next = next;
            next = hdr as *const _ as *const std::ffi::c_void;
        }
        if let Some(color_space) = color_space_extension.as_mut() {
            color_space.next = next;
            next = color_space as *const _ as *const std::ffi::c_void;
        }

        let desc = sys::optics_gfx_surface_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DESC,
            next,
            vk_surface_khr,
            width,
            height,
            vsync,
            // SAFETY: the bindgen descriptor is a C POD type; zero is the
            // documented default for fields not initialized above.
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_surface = std::ptr::null_mut();
        // SAFETY: desc is fully initialized; the extension structs and the
        // raw_spaces Vec outlive this call. `vk_surface_khr` lifetime is
        // upheld by the caller of the unsafe public entry point, and is
        // null for offscreen surfaces.
        Error::check(unsafe { sys::optics_gfx_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Create an OFFSCREEN surface (no window, no swapchain): gfx owns RGBA8
    /// colour images at `width` x `height`. The frame loop is unchanged
    /// ([`Surface::begin_frame`] → record → [`Frame::submit`] →
    /// [`SubmittedFrame::present`]); `present` completes the frame without presenting,
    /// and [`Surface::read_pixels`] reads the result back. Both dimensions must
    /// be non-zero. Requires a headless or windowed device equally.
    pub fn offscreen(device: &Device, width: u32, height: u32) -> Result<Surface, Error> {
        let desc = sys::optics_gfx_surface_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DESC,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Like [`Surface::offscreen`], but pinned to CPU readback: the images
    /// are never made dma-buf exportable, so [`Surface::read_pixels`] works
    /// even on a dma-buf-capable device (where plain offscreen surfaces
    /// transition their submitted images to the foreign consumer). The right
    /// constructor for screenshot/capture targets.
    pub fn offscreen_readback(device: &Device, width: u32, height: u32) -> Result<Surface, Error> {
        let readback = sys::optics_gfx_surface_readback_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_READBACK_DESC,
            next: std::ptr::null(),
            require_readback: true,
        };
        let desc = sys::optics_gfx_surface_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DESC,
            next: &readback as *const _ as *const std::os::raw::c_void,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_surface_create(device.raw, &desc, &mut out) })?;
        Ok(Surface { raw: out })
    }

    /// Create an exportable offscreen surface constrained to modifiers the
    /// external consumer accepts. gfx intersects `modifiers` with the Vulkan
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
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }
        let extension = sys::optics_gfx_surface_dmabuf_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DMABUF_DESC,
            next: std::ptr::null(),
            modifiers: modifiers.as_ptr(),
            modifier_count: modifiers
                .len()
                .try_into()
                .map_err(|_| Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE))?,
        };
        let desc = sys::optics_gfx_surface_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_DESC,
            next: &extension as *const _ as *const std::os::raw::c_void,
            vk_surface_khr: std::ptr::null_mut(),
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_surface = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_surface_create(device.raw, &desc, &mut out) })?;
        let surface = Surface { raw: out };
        if !surface.is_exportable() {
            return Err(Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_UNSUPPORTED));
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
            sys::optics_gfx_surface_read_pixels(
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
        Error::check(unsafe { sys::optics_gfx_surface_prepare_readback(self.raw) })
    }

    /// Preallocate persistent staging for a future region readback. Existing
    /// staging with sufficient capacity is reused. Both dimensions must be
    /// non-zero and no larger than the surface extent.
    pub fn prepare_readback_region(&self, width: u32, height: u32) -> Result<(), Error> {
        Error::check(unsafe {
            sys::optics_gfx_surface_prepare_readback_region(self.raw, width, height)
        })
    }

    /// Whether the most recently captured frame is available for
    /// [`read_pixels`](Self::read_pixels), without waiting. Once this returns
    /// `true`, `read_pixels` only copies already-mapped CPU memory.
    pub fn read_pixels_ready(&self) -> Result<bool, Error> {
        let mut ready = false;
        Error::check(unsafe { sys::optics_gfx_surface_read_pixels_ready(self.raw, &mut ready) })?;
        Ok(ready)
    }

    /// Detach the completed on-demand snapshot without copying its pixels.
    /// The returned handle can be moved to a post-processing worker while
    /// this surface continues presenting.
    pub fn take_readback(&self) -> Result<Readback, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_surface_take_readback(self.raw, &mut out) })?;
        Ok(Readback { raw: out })
    }

    /// Whether this offscreen surface can export its rendered images as
    /// single-plane BGRA8 dma-bufs. Windowed surfaces are never exportable.
    pub fn is_exportable(&self) -> bool {
        unsafe { sys::optics_gfx_surface_exportable(self.raw) }
    }

    /// DRM format modifier selected for this exportable offscreen surface.
    /// `None` means this is not a dma-buf-exportable surface; `Some(0)` is the
    /// valid `DRM_FORMAT_MOD_LINEAR` modifier rather than absence.
    pub fn dmabuf_modifier(&self) -> Option<u64> {
        self.is_exportable()
            .then(|| unsafe { sys::optics_gfx_surface_dmabuf_modifier(self.raw) })
    }

    /// Export the most recently submitted offscreen frame without a pixel
    /// copy. This waits for gfx's GPU work, transfers image ownership to a
    /// foreign consumer, and returns an owned dma-buf descriptor plus the
    /// metadata required to create a DRM framebuffer.
    ///
    /// The returned `DmabufLease` owns the image lifetime. gfx refuses to
    /// reacquire the ring slot until the lease is dropped (DRM/KMS releases it
    /// after the page flip retires; Wayland after `wl_buffer.release`).
    pub fn export_dmabuf(&self) -> Result<ExportedDmabuf, Error> {
        let mut fd = -1;
        let mut lease = std::ptr::null_mut();
        Error::check(unsafe {
            sys::optics_gfx_surface_export_dmabuf(self.raw, &mut fd, &mut lease)
        })?;
        if fd < 0 || lease.is_null() {
            if fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(fd)) };
            }
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_BACKEND_FAILURE,
            ));
        }
        let (width, height) = self.size();
        Ok(ExportedDmabuf {
            dmabuf: SurfaceDmabuf {
                // SAFETY: gfx returned ownership of a fresh descriptor on success.
                fd: unsafe { OwnedFd::from_raw_fd(fd) },
                acquire_fence: None,
                width,
                height,
                stride: unsafe { sys::optics_gfx_surface_dmabuf_stride(self.raw) },
                modifier: unsafe { sys::optics_gfx_surface_dmabuf_modifier(self.raw) },
                slot: unsafe { sys::optics_gfx_surface_last_slot(self.raw) },
            },
            lease: DmabufLease { raw: lease },
        })
    }

    /// Export the most recently submitted frame with an explicit acquire
    /// fence, without waiting for the GPU on the calling CPU thread.
    ///
    /// The consumer must wait `acquire_fence` before reading the dma-buf. DRM
    /// atomic clients pass it to the plane's `IN_FENCE_FD` property. This may
    /// be called once per submitted frame slot.
    pub fn export_dmabuf_explicit(&self) -> Result<ExportedDmabuf, Error> {
        let mut fd = -1;
        let mut sync_fd = -1;
        let mut lease = std::ptr::null_mut();
        Error::check(unsafe {
            sys::optics_gfx_surface_export_dmabuf_explicit(
                self.raw,
                &mut fd,
                &mut sync_fd,
                &mut lease,
            )
        })?;
        if fd < 0 || sync_fd < 0 || lease.is_null() {
            if fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(fd)) };
            }
            if sync_fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(sync_fd)) };
            }
            if !lease.is_null() {
                unsafe { sys::optics_gfx_surface_dmabuf_lease_release(lease) };
            }
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_BACKEND_FAILURE,
            ));
        }
        let (width, height) = self.size();
        Ok(ExportedDmabuf {
            dmabuf: SurfaceDmabuf {
                // SAFETY: gfx returned ownership of both fresh descriptors.
                fd: unsafe { OwnedFd::from_raw_fd(fd) },
                acquire_fence: Some(unsafe { OwnedFd::from_raw_fd(sync_fd) }),
                width,
                height,
                stride: unsafe { sys::optics_gfx_surface_dmabuf_stride(self.raw) },
                modifier: unsafe { sys::optics_gfx_surface_dmabuf_modifier(self.raw) },
                slot: unsafe { sys::optics_gfx_surface_last_slot(self.raw) },
            },
            lease: DmabufLease { raw: lease },
        })
    }

    /// Persistent/pooled dma-buf export for a slot table (ScreenCast). No
    /// image lease is returned: the ring may reuse this image on a later
    /// frame, so the consumer must rely on the per-frame explicit fence.
    pub fn export_dmabuf_pooled(&self) -> Result<SurfaceDmabuf, Error> {
        let mut fd = -1;
        Error::check(unsafe { sys::optics_gfx_surface_export_dmabuf_pooled(self.raw, &mut fd) })?;
        if fd < 0 {
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_BACKEND_FAILURE,
            ));
        }
        let (width, height) = self.size();
        Ok(SurfaceDmabuf {
            fd: unsafe { OwnedFd::from_raw_fd(fd) },
            acquire_fence: None,
            width,
            height,
            stride: unsafe { sys::optics_gfx_surface_dmabuf_stride(self.raw) },
            modifier: unsafe { sys::optics_gfx_surface_dmabuf_modifier(self.raw) },
            slot: unsafe { sys::optics_gfx_surface_last_slot(self.raw) },
        })
    }

    /// Explicit-sync pooled export. The returned fence orders this frame's
    /// write; the ring may still reuse the image later without a lease.
    pub fn export_dmabuf_pooled_explicit(&self) -> Result<SurfaceDmabuf, Error> {
        let mut fd = -1;
        let mut sync_fd = -1;
        Error::check(unsafe {
            sys::optics_gfx_surface_export_dmabuf_pooled_explicit(self.raw, &mut fd, &mut sync_fd)
        })?;
        if fd < 0 || sync_fd < 0 {
            if fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(fd)) };
            }
            if sync_fd >= 0 {
                unsafe { drop(OwnedFd::from_raw_fd(sync_fd)) };
            }
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_BACKEND_FAILURE,
            ));
        }
        let (width, height) = self.size();
        Ok(SurfaceDmabuf {
            fd: unsafe { OwnedFd::from_raw_fd(fd) },
            acquire_fence: Some(unsafe { OwnedFd::from_raw_fd(sync_fd) }),
            width,
            height,
            stride: unsafe { sys::optics_gfx_surface_dmabuf_stride(self.raw) },
            modifier: unsafe { sys::optics_gfx_surface_dmabuf_modifier(self.raw) },
            slot: unsafe { sys::optics_gfx_surface_last_slot(self.raw) },
        })
    }

    /// Recreate the swapchain at a new extent. Safe to call from a resize event.
    pub fn resize(&mut self, width: u32, height: u32) -> Result<(), Error> {
        Error::check(unsafe { sys::optics_gfx_surface_resize(self.raw, width, height) })
    }

    /// Current swapchain extent.
    pub fn size(&self) -> (u32, u32) {
        let mut info = sys::optics_gfx_surface_info::default();
        unsafe { sys::optics_gfx_surface_get_info(self.raw, &mut info) };
        (info.width, info.height)
    }

    /// Negotiated surface state: extent, image count, HDR flag, and the
    /// color spaces the surface presents in / is written in (ADR-0069).
    pub fn info(&self) -> SurfaceInfo {
        let mut raw = sys::optics_gfx_surface_info::default();
        unsafe { sys::optics_gfx_surface_get_info(self.raw, &mut raw) };
        SurfaceInfo {
            width: raw.width,
            height: raw.height,
            image_count: raw.image_count,
            hdr: raw.hdr,
            color_space: ColorSpace::from_raw(raw.color_space),
            content_space: ColorSpace::from_raw(raw.content_space),
            format: Format::from_raw(raw.format),
        }
    }

    /// Backend-neutral color format selected for this surface.
    pub fn format(&self) -> Format {
        Format::from_raw(unsafe {
            sys::optics_gfx_format_from_vk(sys::optics_gfx_surface_vk_format(self.raw))
        })
    }

    /// Acquire the next frame. Returns the backend result so callers can handle
    /// `SURFACE_LOST` / out-of-date by resizing.
    ///
    /// The caller did not describe what changed, so gfx conservatively treats
    /// the frame as full-extent. Use [`Self::begin_frame_with_damage`] when the
    /// compositor has a localized damage set.
    pub fn begin_frame(&self) -> Result<Frame<'_>, Error> {
        self.begin_frame_opts(0, false, &Damage::Full)
    }

    /// Acquire the next frame with an explicit timeout in nanoseconds. Equivalent
    /// to [`Self::begin_frame`] with the default conservative damage verdict.
    pub fn begin_frame_timeout(&self, timeout_ns: u64) -> Result<Frame<'_>, Error> {
        self.begin_frame_opts(timeout_ns, false, &Damage::Full)
    }

    /// Acquire the next frame with the changes this frame is about to record.
    ///
    /// gfx unions this with the history the acquired image missed and exposes
    /// the exact repaint scope through [`Frame::repaint_scope`]. On windowed
    /// surfaces the engine repairs history from its persistent content image,
    /// so the returned scope is only the current damage.
    pub fn begin_frame_with_damage(&self, damage: &Damage) -> Result<Frame<'_>, Error> {
        self.begin_frame_opts(0, false, damage)
    }

    /// Timed variant of [`Self::begin_frame_with_damage`].
    pub fn begin_frame_timeout_with_damage(
        &self,
        timeout_ns: u64,
        damage: &Damage,
    ) -> Result<Frame<'_>, Error> {
        self.begin_frame_opts(timeout_ns, false, damage)
    }

    /// Acquire the next frame non-blockingly. Returns `Error::Timeout` immediately
    /// if the fence has not retired or a swapchain image is not ready, without
    /// blocking the calling thread.
    pub fn begin_frame_nonblocking(&self) -> Result<Frame<'_>, Error> {
        self.begin_frame_opts(0, true, &Damage::Full)
    }

    fn begin_frame_opts(
        &self,
        timeout_ns: u64,
        non_blocking: bool,
        damage: &Damage,
    ) -> Result<Frame<'_>, Error> {
        let (raw_rects, full) = damage.raw();
        let desc = sys::optics_gfx_frame_begin_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_FRAME_BEGIN_DESC,
            timeout_ns,
            non_blocking,
            damage: sys::optics_gfx_damage {
                rects: raw_rects.as_ptr(),
                count: raw_rects.len() as u32,
            },
            damage_full: full,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_frame = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_surface_begin_frame(self.raw, &desc, &mut out) })?;
        Ok(Frame {
            raw: out,
            surface: self,
        })
    }

    /// The damage the last presented frame has not put on screen yet, in
    /// physical surface pixels. This is the set a KMS/scanout hint should use:
    /// it includes frames submitted and then abandoned before scanout, and it is
    /// independent of which ring image the next frame acquires.
    pub fn scanout_damage(&self) -> Result<Damage, Error> {
        let mut raw = [sys::optics_gfx_rect {
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
        }; sys::OPTICS_GFX_DAMAGE_CAPACITY as usize];
        let mut count = 0u32;
        let mut full = false;
        let opts = sys::optics_gfx_surface_scanout_damage_opts {
            out_rects: raw.as_mut_ptr(),
            capacity: raw.len() as u32,
            out_count: &mut count,
            out_full: &mut full,
        };
        Error::check(unsafe { sys::optics_gfx_surface_scanout_damage(self.raw, &opts) })?;
        Ok(Damage::from_raw(&raw, count, full))
    }

    /// Forget all damage epochs, e.g. after a VT switch made the display
    /// contents unknown. The next frame repaints in full. Must not be called
    /// while a frame is recording.
    pub fn invalidate_damage(&self) {
        unsafe { sys::optics_gfx_surface_invalidate_damage(self.raw) };
    }

    /// Raw `optics_gfx_surface*` for FFI edges (window does this when bridging).
    pub fn as_raw(&self) -> *mut sys::optics_gfx_surface {
        self.raw
    }
}

impl Drop for Surface {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_surface_release(self.raw) };
    }
}
