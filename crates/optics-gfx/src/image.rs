//! Images.

use super::*;

pub(crate) fn image_data_len(width: u32, height: u32, format: Format) -> Result<usize, Error> {
    let bytes_per_pixel = match format {
        Format::R8Unorm => 1usize,
        Format::Rgba8Unorm | Format::Bgra8Unorm | Format::Rgba8Srgb | Format::Bgra8Srgb => 4usize,
        Format::Rgba16Sfloat => 8usize, // ADR-0069 working space
        _ => return Err(Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_UNSUPPORTED)),
    };
    (width as usize)
        .checked_mul(height as usize)
        .and_then(|pixels| pixels.checked_mul(bytes_per_pixel))
        .ok_or(Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE))
}

/// A GPU texture sampled by the canvas. Refcounted in C; this handle owns one
/// reference.
pub struct Image {
    pub(crate) raw: *mut sys::optics_gfx_image,
}

/// Color-space tag for [`Image::from_bytes_with_color_space`]
/// (ADR-0069/0070). Tagging changes how texels are decoded into the
/// working space — it never re-encodes the stored pixels. Untagged images
/// keep the format-derived default: 8-bit formats are sRGB,
/// `RGBA16_SFLOAT` is working-space linear.
#[derive(Debug, Default, Clone, Copy)]
pub struct ImageColorSpace<'a> {
    /// Parametric content tag (e.g. a Display P3 texture).
    pub space: Option<ColorSpace>,
    /// ICC profile tag; takes precedence over `space`. A
    /// parametric-extractable profile behaves like `space`; a LUT-only
    /// profile is baked into a 3D LUT at creation and sampled by the
    /// canvas image pipeline.
    pub icc: Option<&'a IccProfile>,
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
        let desc = sys::optics_gfx_image_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_IMAGE_DESC,
            width,
            height,
            format: format.raw(),
            usage: sys::optics_gfx_image_usage::OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_image_create(device.raw, &desc, &mut out) })?;
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
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }

        let desc = sys::optics_gfx_image_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_IMAGE_DESC,
            width,
            height,
            format: format.raw(),
            initial_data: data.as_ptr() as *const std::os::raw::c_void,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_image = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_image_create(device.raw, &desc, &mut out) })?;
        Ok(Image { raw: out })
    }

    /// Like [`Image::from_bytes`], but tags the content's color space
    /// through a `optics_gfx_image_color_space_desc` extension. The tag is read
    /// only during creation.
    pub fn from_bytes_with_color_space(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        data: &[u8],
        color_space: ImageColorSpace<'_>,
    ) -> Result<Image, Error> {
        let expected = image_data_len(width, height, format)?;
        if data.len() != expected {
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }

        let raw_space = color_space.space.map(|s| s.as_raw());
        let extension = sys::optics_gfx_image_color_space_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC,
            next: std::ptr::null(),
            space: raw_space
                .as_ref()
                .map(|s| s as *const sys::optics_gfx_color_space)
                .unwrap_or(std::ptr::null()),
            icc: color_space
                .icc
                .map(|p| p.as_raw() as *const sys::optics_gfx_icc_profile)
                .unwrap_or(std::ptr::null()),
        };
        let desc = sys::optics_gfx_image_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_IMAGE_DESC,
            next: if raw_space.is_some() || color_space.icc.is_some() {
                &extension as *const _ as *const std::os::raw::c_void
            } else {
                std::ptr::null()
            },
            width,
            height,
            format: format.raw(),
            initial_data: data.as_ptr() as *const std::os::raw::c_void,
            usage: sys::optics_gfx_image_usage::OPTICS_GFX_IMAGE_USAGE_SAMPLED,
        };
        let mut out: *mut sys::optics_gfx_image = std::ptr::null_mut();
        // SAFETY: desc is fully initialized; the extension and raw_space
        // outlive this call.
        Error::check(unsafe { sys::optics_gfx_image_create(device.raw, &desc, &mut out) })?;
        Ok(Image { raw: out })
    }

    /// Raw `optics_gfx_image` pointer. For passing the texture to a consumer that
    /// lives in a separate binding crate (e.g. ui's `optics_ui_image`) where the
    /// `optics_gfx_image` typedef is bindgen-generated independently.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_image {
        self.raw
    }

    pub fn size(&self) -> (u32, u32) {
        unsafe {
            (
                sys::optics_gfx_image_width(self.raw),
                sys::optics_gfx_image_height(self.raw),
            )
        }
    }

    pub fn format(&self) -> Format {
        Format::from_raw(unsafe { sys::optics_gfx_image_format(self.raw) })
    }

    /// Content bytes described by extent and format (ADR-0090 accounting).
    pub fn size_bytes(&self) -> usize {
        unsafe { sys::optics_gfx_image_size_bytes(self.raw) }
    }

    /// Publish an immutable snapshot handle. The original may then be updated;
    /// copy-on-write keeps this snapshot on its captured pixels.
    pub fn snapshot(&self) -> Result<Image, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_image_snapshot(self.raw, &mut out) })?;
        Ok(Image { raw: out })
    }
}

impl Image {
    /// Import a single-plane Linux dma-buf as a sampled image (zero-copy),
    /// taking ownership of `fd`: on success gfx consumes and closes it; on
    /// error `fd` is dropped (closed) on the Rust side, so there is no leak
    /// and no double-close either way. The device must have been created
    /// with the dma-buf import extensions (see [`dmabuf_supported`]).
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
        fd: OwnedFd,
        offset: u32,
        stride: u32,
    ) -> Result<Image, Error> {
        // SAFETY: the caller guarantees the dma-buf descriptor requirements
        // documented by this function; all values are forwarded unchanged.
        let result = unsafe {
            Self::import_dmabuf_raw(
                device,
                width,
                height,
                format,
                modifier,
                fd.as_raw_fd(),
                offset,
                stride,
                None,
                ImageColorSpace::default(),
            )
        };
        // On success gfx owns the fd now; on error Rust still does and the
        // OwnedFd drop closes it. Either way exactly one side closes it.
        if result.is_ok() {
            std::mem::forget(fd);
        }
        result
    }

    /// Raw-fd spelling of [`Image::import_dmabuf`] for callers that do not
    /// own the descriptor (e.g. it belongs to a compositor protocol object
    /// they must keep using). The caller keeps `fd` open in ALL cases; gfx
    /// internally dups it. Nothing is closed on the Rust side.
    ///
    /// # Safety
    /// `fd` must be a valid dma-buf file descriptor whose contents match
    /// the metadata arguments.
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn import_dmabuf_borrowed(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: i32,
        offset: u32,
        stride: u32,
    ) -> Result<Image, Error> {
        // SAFETY: caller guarantees the descriptor contract above.
        unsafe {
            Self::import_dmabuf_raw(
                device,
                width,
                height,
                format,
                modifier,
                fd,
                offset,
                stride,
                None,
                ImageColorSpace::default(),
            )
        }
    }

    /// Import a dma-buf and wait a Linux `sync_file` acquire fence before the
    /// image becomes sampleable, taking ownership of both descriptors: on
    /// success gfx consumes and closes them; on error Rust's `OwnedFd` drops
    /// close them. No leak, no double-close.
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
        fd: OwnedFd,
        offset: u32,
        stride: u32,
        acquire_sync_fd: OwnedFd,
    ) -> Result<Image, Error> {
        // SAFETY: the caller guarantees both file descriptors and the dma-buf
        // metadata meet the requirements documented by this function.
        let result = unsafe {
            Self::import_dmabuf_raw(
                device,
                width,
                height,
                format,
                modifier,
                fd.as_raw_fd(),
                offset,
                stride,
                Some(acquire_sync_fd.as_raw_fd()),
                ImageColorSpace::default(),
            )
        };
        if result.is_ok() {
            std::mem::forget(fd);
            std::mem::forget(acquire_sync_fd);
        }
        result
    }

    /// Like [`Image::import_dmabuf`], plus an optional acquire fence and a
    /// color-space tag (ADR-0069/0070): the tag decodes the imported texels
    /// into the working space when the canvas samples them — the zero-copy
    /// path for color-tagged client buffers. The tag is read only during
    /// creation. Takes ownership of `fd` (and of `acquire_sync_fd` when
    /// given): on success gfx consumes and closes them; on error the
    /// `OwnedFd` drops close them.
    ///
    /// # Safety
    /// Same contract as [`Image::import_dmabuf_with_acquire_fence`].
    #[allow(clippy::too_many_arguments)]
    pub unsafe fn import_dmabuf_with_color_space(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: OwnedFd,
        offset: u32,
        stride: u32,
        acquire_sync_fd: Option<OwnedFd>,
        color_space: ImageColorSpace<'_>,
    ) -> Result<Image, Error> {
        // SAFETY: the caller guarantees the dma-buf/sync-file descriptor
        // requirements documented above; all values are forwarded unchanged.
        let raw_fence = acquire_sync_fd.as_ref().map(|f| f.as_raw_fd());
        let result = unsafe {
            Self::import_dmabuf_raw(
                device,
                width,
                height,
                format,
                modifier,
                fd.as_raw_fd(),
                offset,
                stride,
                raw_fence,
                color_space,
            )
        };
        if result.is_ok() {
            std::mem::forget(fd);
            if let Some(f) = acquire_sync_fd {
                std::mem::forget(f);
            }
        }
        result
    }

    #[allow(clippy::too_many_arguments)]
    unsafe fn import_dmabuf_raw(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
        modifier: u64,
        fd: i32,
        offset: u32,
        stride: u32,
        acquire_sync_fd: Option<i32>,
        color_space: ImageColorSpace<'_>,
    ) -> Result<Image, Error> {
        let raw_space = color_space.space.map(|s| s.as_raw());
        let color_extension = sys::optics_gfx_image_color_space_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_IMAGE_COLOR_SPACE_DESC,
            next: std::ptr::null(),
            space: raw_space
                .as_ref()
                .map(|s| s as *const sys::optics_gfx_color_space)
                .unwrap_or(std::ptr::null()),
            icc: color_space
                .icc
                .map(|p| p.as_raw() as *const sys::optics_gfx_icc_profile)
                .unwrap_or(std::ptr::null()),
        };
        let mut desc = sys::optics_gfx_dmabuf_image_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_DMABUF_IMAGE_DESC,
            next: if raw_space.is_some() || color_space.icc.is_some() {
                &color_extension as *const _ as *const std::ffi::c_void
            } else {
                std::ptr::null()
            },
            width,
            height,
            format: format.raw(),
            modifier,
            plane_count: 1,
            // SAFETY: the bindgen descriptor is a C POD type; zero is the
            // documented default for fields not initialized above.
            ..unsafe { std::mem::zeroed() }
        };
        desc.planes[0] = sys::optics_gfx_dmabuf_plane { fd, offset, stride };
        if let Some(acquire_sync_fd) = acquire_sync_fd {
            desc.has_acquire_sync_fd = true;
            desc.acquire_sync_fd = acquire_sync_fd;
        }
        let mut out: *mut sys::optics_gfx_image = std::ptr::null_mut();
        // SAFETY: the caller of this function guarantees that `desc` describes
        // valid dma-buf and optional sync-file descriptors; the color
        // extension and raw_space outlive this call.
        Error::check(unsafe { sys::optics_gfx_image_import_dmabuf(device.raw, &desc, &mut out) })?;
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
        let opts = sys::optics_gfx_image_update_region_opts {
            x,
            y,
            width: w,
            height: h,
            data: data.as_ptr() as *const _,
            row_bytes: 0,
            bytes: data.len(),
            premultiply: false,
        };
        Error::check(unsafe { sys::optics_gfx_image_update_region(self.raw, &opts) })
    }

    /// [`Image::update_region`] for source rows wider than the region:
    /// row `i` of `data` starts at `i * row_bytes`.
    ///
    /// `row_bytes` must be at least `w * bytes_per_pixel`; the buffer must
    /// hold at least `(h - 1) * row_bytes + w * bpp` bytes (larger buffers
    /// are fine — trailing bytes are ignored).
    pub fn update_region_strided(
        &self,
        x: u32,
        y: u32,
        w: u32,
        h: u32,
        row_bytes: usize,
        data: &[u8],
    ) -> Result<(), Error> {
        let opts = sys::optics_gfx_image_update_region_opts {
            x,
            y,
            width: w,
            height: h,
            data: data.as_ptr() as *const _,
            row_bytes,
            bytes: data.len(),
            premultiply: false,
        };
        Error::check(unsafe { sys::optics_gfx_image_update_region(self.raw, &opts) })
    }

    /// Upload straight (non-premultiplied) RGBA8 pixels, premultiplying
    /// during the upload with the exact integer math of
    /// `optics_gfx_color_rgba_premul` — `(c * a + 127) / 255` and the
    /// `a == 255` / `a == 0` fast paths.
    ///
    /// The canvas samples premultiplied texels, so this is the correct
    /// entry point whenever the source buffer holds straight alpha (the
    /// common case for editor framebuffers): callers never replicate the
    /// premultiply convention themselves. Row `i` of `data` starts at
    /// `i * row_bytes` (pass `w * 4` for a packed buffer, or `0` — both
    /// mean packed).
    #[allow(clippy::too_many_arguments)]
    pub fn update_region_premultiply(
        &self,
        x: u32,
        y: u32,
        w: u32,
        h: u32,
        row_bytes: usize,
        data: &[u8],
    ) -> Result<(), Error> {
        let opts = sys::optics_gfx_image_update_region_opts {
            x,
            y,
            width: w,
            height: h,
            data: data.as_ptr() as *const _,
            row_bytes,
            bytes: data.len(),
            premultiply: true,
        };
        Error::check(unsafe { sys::optics_gfx_image_update_region(self.raw, &opts) })
    }
}

impl Drop for Image {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_image_release(self.raw) };
    }
}
