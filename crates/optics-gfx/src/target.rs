//! Render targets and canvas sessions.

use super::*;

/// Refcounted render target. Scene passes use a depth target matching the
/// surface extent, with one target per frame-in-flight slot.
pub struct Target {
    pub(crate) raw: *mut sys::optics_gfx_target,
    pub(crate) format: Format,
}

impl Target {
    pub fn depth(
        device: &Device,
        width: u32,
        height: u32,
        format: Format,
    ) -> Result<Target, Error> {
        let desc = sys::optics_gfx_target_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_TARGET_DESC,
            usage: sys::optics_gfx_target_usage::OPTICS_GFX_TARGET_DEPTH as u32,
            format: format.raw(),
            width,
            height,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_target_create(device.raw, &desc, &mut raw) })?;
        Ok(Target { raw, format })
    }

    pub fn size(&self) -> (u32, u32) {
        unsafe {
            (
                sys::optics_gfx_target_width(self.raw),
                sys::optics_gfx_target_height(self.raw),
            )
        }
    }

    pub fn as_raw(&self) -> *mut sys::optics_gfx_target {
        self.raw
    }

    /// Create an owned CPU raster target (ADR-0088).
    pub fn create_cpu(width: u32, height: u32) -> Result<Target, Error> {
        let desc = sys::optics_gfx_cpu_target_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_CPU_TARGET_DESC,
            next: std::ptr::null(),
            width,
            height,
            format: sys::optics_gfx_format::OPTICS_GFX_FORMAT_RGBA8_UNORM,
            user_buffer: std::ptr::null_mut(),
            buffer_bytes: 0,
            stride_bytes: 0,
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_target_create_cpu(&desc, &mut raw) })?;
        Ok(Target {
            raw,
            format: Format::Rgba8Unorm,
        })
    }

    /// Readback CPU target pixels as a byte slice.
    pub fn cpu_pixels(&self) -> Option<&[u8]> {
        let mut w = 0;
        let mut h = 0;
        let mut stride = 0;
        let ptr =
            unsafe { sys::optics_gfx_target_cpu_pixels(self.raw, &mut w, &mut h, &mut stride) };
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { std::slice::from_raw_parts(ptr, (h * stride) as usize) })
        }
    }
}

mod sealed {
    pub trait Sealed {}
    impl Sealed for &mut super::Target {}
    impl Sealed for () {}
    impl Sealed for &mut super::Frame<'_> {}
    impl Sealed for super::ImageTarget<'_, '_> {}
}

/// Closed set of destinations. `()` selects the CPU Canvas framebuffer;
/// a Frame selects its GPU attachment; Target selects an owned CPU target.
/// ImageTarget pairs a GPU image with its explicit recording frame.
pub trait AsTarget: sealed::Sealed {
    #[doc(hidden)]
    fn configure_pass(&mut self, desc: &mut sys::optics_gfx_canvas_pass_desc);
}
impl AsTarget for () {
    fn configure_pass(&mut self, _: &mut sys::optics_gfx_canvas_pass_desc) {}
}
impl AsTarget for &mut Target {
    fn configure_pass(&mut self, desc: &mut sys::optics_gfx_canvas_pass_desc) {
        desc.attachment.kind =
            sys::optics_gfx_canvas_attachment_kind::OPTICS_GFX_CANVAS_ATTACHMENT_TARGET;
        desc.attachment.__bindgen_anon_1.target = self.raw;
    }
}
impl AsTarget for &mut Frame<'_> {
    fn configure_pass(&mut self, desc: &mut sys::optics_gfx_canvas_pass_desc) {
        desc.frame = self.raw;
    }
}

/// A writable image attachment and the frame recording its commands.
pub struct ImageTarget<'borrow, 'surface> {
    pub frame: &'borrow mut Frame<'surface>,
    pub image: &'borrow mut Image,
}
impl AsTarget for ImageTarget<'_, '_> {
    fn configure_pass(&mut self, desc: &mut sys::optics_gfx_canvas_pass_desc) {
        desc.frame = self.frame.raw;
        desc.attachment.kind =
            sys::optics_gfx_canvas_attachment_kind::OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE;
        desc.attachment.__bindgen_anon_1.image = self.image.raw;
    }
}

/// Drawing capability valid only within an active pass. It exposes no pass
/// lifecycle operations and cannot be constructed by safe caller code.
pub struct CanvasCommands<'session> {
    pub(crate) raw: *mut sys::optics_gfx_canvas,
    pub(crate) _session: PhantomData<&'session mut Canvas>,
}
impl<'session> CanvasCommands<'session> {
    /// Borrow commands from an externally managed active pass.
    ///
    /// # Safety
    /// The owner must keep the Canvas alive and recording throughout
    /// `'session`, serialize access, and check the pass result when closing it.
    pub unsafe fn borrow_raw(raw: *mut sys::optics_gfx_canvas) -> Self {
        Self {
            raw,
            _session: PhantomData,
        }
    }
}

/// Exclusively borrows both the Canvas and destination until explicit end.
/// Drop only closes recording; it does not submit GPU work or report success.
#[must_use = "call end to observe drawing errors"]
pub struct CanvasSession<'canvas, T: AsTarget> {
    pub(crate) canvas: &'canvas mut Canvas,
    pub(crate) commands: CanvasCommands<'canvas>,
    pub(crate) _target: T,
    pub(crate) ended: bool,
}
impl<T: AsTarget> CanvasSession<'_, T> {
    pub fn end(mut self) -> Result<(), Error> {
        self.ended = true;
        Error::check(unsafe { sys::optics_gfx_canvas_end(self.canvas.raw) })
    }
}
impl<'canvas, T: AsTarget> std::ops::Deref for CanvasSession<'canvas, T> {
    type Target = CanvasCommands<'canvas>;
    fn deref(&self) -> &Self::Target {
        &self.commands
    }
}
impl<T: AsTarget> Drop for CanvasSession<'_, T> {
    fn drop(&mut self) {
        if !self.ended {
            unsafe { sys::optics_gfx_canvas_end(self.canvas.raw) };
        }
    }
}

impl Drop for Target {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_target_release(self.raw) };
    }
}
