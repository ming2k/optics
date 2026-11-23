//! The 2D canvas.

use super::*;

/// A successfully submitted frame waiting to be presented.
#[must_use = "a submitted frame must be presented"]
pub struct SubmittedFrame<'surface> {
    pub(crate) raw: *mut sys::optics_gfx_frame,
    pub(crate) _surface: PhantomData<&'surface Surface>,
}

impl SubmittedFrame<'_> {
    /// Present the frame and consume the surface-owned frame-slot borrow.
    pub fn present(mut self) -> Result<(), Error> {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        Error::check(unsafe { sys::optics_gfx_frame_present(raw) })
    }

    /// Release the submitted frame without presenting or advancing the
    /// offscreen ring slot (used when an external KMS page flip is deferred or
    /// rejected so the next frame reuses the same free backbuffer slot).
    pub fn abandon(mut self) {
        let raw = std::mem::replace(&mut self.raw, std::ptr::null_mut());
        if !raw.is_null() {
            unsafe { sys::optics_gfx_frame_abandon(raw) };
        }
    }

    pub fn as_raw(&self) -> *mut sys::optics_gfx_frame {
        self.raw
    }
}

impl Drop for SubmittedFrame<'_> {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::optics_gfx_frame_abandon(self.raw) };
        }
    }
}

/// A 2D canvas bound to a [`Surface`]. Records draws into a [`Frame`] between
/// [`Canvas::begin_session`] and [`CanvasSession::end`].
pub struct Canvas {
    pub(crate) raw: *mut sys::optics_gfx_canvas,
    /// When `true`, this handle does **not** own the canvas and must not
    /// destroy it on drop. Set by [`Canvas::borrow_raw`] for views over a
    /// canvas another owner created and retains (e.g. window's paint callback).
    borrowed: bool,
}

/// Load policy for one [`Canvas`] pass.
///
/// Not `Copy`: the damage scope is an owned list. Cloning a pass policy should be
/// an explicit decision rather than something a `Copy` bound does silently.
#[derive(Debug, Default, Clone, PartialEq)]
pub struct CanvasPassOptions {
    /// `Some(color)` clears the destination; `None` preserves its contents.
    pub clear: Option<u32>,
    /// Optional damage scope: the rectangles this pass may write, in physical
    /// framebuffer pixels. Empty uses the complete destination. Clearing and
    /// drawing are both constrained to the set while pixels outside it remain
    /// untouched, and a draw that touches none of them is dropped.
    ///
    /// A set rather than a rectangle because the bounding box of two distant
    /// bands is nearly the whole output — collapsing to one rect destroys the
    /// localisation this exists to carry (ADR-0143, ADR-0149).
    pub damage: Vec<CanvasRenderArea>,
}

/// One rectangle of a pass's damage scope, in physical framebuffer pixels.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CanvasRenderArea {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
}

/// A frame/scanout damage verdict in physical surface pixels.
///
/// This is deliberately not a canvas-pass scope: an empty pass scope means the
/// whole destination, while [`Damage::None`] means nothing changed. Keeping the
/// verdict explicit prevents the two conventions from drifting.
#[derive(Debug, Clone, PartialEq)]
pub enum Damage {
    None,
    Area(Vec<CanvasRenderArea>),
    Full,
}

impl Damage {
    pub(crate) fn raw(&self) -> (Vec<sys::optics_gfx_rect>, bool) {
        match self {
            Self::None => (Vec::new(), false),
            Self::Full => (Vec::new(), true),
            Self::Area(areas) => (
                areas
                    .iter()
                    .map(|a| sys::optics_gfx_rect {
                        x: a.x,
                        y: a.y,
                        w: a.width,
                        h: a.height,
                    })
                    .collect(),
                false,
            ),
        }
    }

    pub(crate) fn from_raw(raw: &[sys::optics_gfx_rect], count: u32, full: bool) -> Self {
        if full {
            return Self::Full;
        }
        let n = (count as usize).min(raw.len());
        if n == 0 {
            return Self::None;
        }
        Self::Area(
            raw[..n]
                .iter()
                .map(|r| CanvasRenderArea {
                    x: r.x,
                    y: r.y,
                    width: r.w,
                    height: r.h,
                })
                .collect(),
        )
    }

    pub fn is_full(&self) -> bool {
        matches!(self, Self::Full)
    }

    pub fn is_empty(&self) -> bool {
        matches!(self, Self::None)
    }
}

/// Build the raw pass descriptor and invoke `f` while every referenced value is
/// alive.
fn with_raw_canvas_pass_desc<T>(
    options: CanvasPassOptions,
    f: impl FnOnce(&sys::optics_gfx_canvas_pass_desc) -> T,
) -> T {
    let clear = options.clear;
    // The C descriptor reads this array during `optics_gfx_canvas_begin` and copies
    // it into canvas-owned storage before returning; it is not retained for the
    // session lifetime. Keeping the Vec local is therefore correct and avoids
    // forcing every session to own a second copy of its damage scope.
    let damage_rects: Vec<sys::optics_gfx_rect> = options
        .damage
        .iter()
        .map(|a| sys::optics_gfx_rect {
            x: a.x,
            y: a.y,
            w: a.width,
            h: a.height,
        })
        .collect();
    let damage = sys::optics_gfx_damage {
        rects: if damage_rects.is_empty() {
            std::ptr::null()
        } else {
            damage_rects.as_ptr()
        },
        count: damage_rects.len() as u32,
    };
    let desc = sys::optics_gfx_canvas_pass_desc {
        type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
        frame: std::ptr::null_mut(),
        attachment: Default::default(),
        next: std::ptr::null(),
        clear_color: clear
            .as_ref()
            .map(|color| color as *const u32)
            .unwrap_or(std::ptr::null()),
        damage,
        clear_extent: false,
    };
    f(&desc)
}

impl Canvas {
    pub fn new(surface: &Surface) -> Result<Canvas, Error> {
        let desc = sys::optics_gfx_canvas_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_CANVAS_DESC,
            surface: surface.raw,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_canvas = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_canvas_create(&desc, &mut out) })?;
        Ok(Canvas {
            raw: out,
            borrowed: false,
        })
    }

    /// Create a headless **software (CPU)** canvas with a `width`x`height`
    /// framebuffer (physical pixels) and content `scale`. No GPU, device, or
    /// surface required. Record between [`begin_session`](Self::begin_session)
    /// and [`CanvasSession::end`], then read the result with
    /// [`read_pixels`](Self::read_pixels).
    ///
    /// Supported: solid fills, paths, rounded rects, gradients, clipping, and
    /// glyph runs (with host coverage). Image draws require a GPU canvas.
    pub fn new_cpu(width: u32, height: u32, scale: f32) -> Result<Canvas, Error> {
        let desc = sys::optics_gfx_canvas_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_CANVAS_DESC,
            backend: sys::optics_gfx_canvas_backend_kind::OPTICS_GFX_CANVAS_BACKEND_CPU,
            scale,
            width,
            height,
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::optics_gfx_canvas = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_canvas_create(&desc, &mut out) })?;
        Ok(Canvas {
            raw: out,
            borrowed: false,
        })
    }

    /// Begin a pass, borrowing all writable state until the session ends.
    pub fn begin_session<'c, T: AsTarget>(
        &'c mut self,
        mut target: T,
        options: CanvasPassOptions,
    ) -> Result<CanvasSession<'c, T>, Error> {
        with_raw_canvas_pass_desc(options, |desc| {
            let mut desc = *desc;
            target.configure_pass(&mut desc);
            Error::check(unsafe { sys::optics_gfx_canvas_begin(self.raw, &desc) })
        })?;
        let raw = self.raw;
        Ok(CanvasSession {
            canvas: self,
            commands: CanvasCommands {
                raw,
                _session: PhantomData,
            },
            _target: target,
            ended: false,
        })
    }

    /// Snapshot the canvas' pixels as premultiplied RGBA8 (row-major). Returns
    /// `(width, height, stride_bytes, pixels)` on the CPU backend; `None` on the
    /// GPU backend (use an offscreen surface / target for GPU readback). The
    /// returned vector owns its pixels and is unaffected by later canvas calls.
    pub fn read_pixels(&self) -> Option<(u32, u32, u32, Vec<u8>)> {
        let (mut w, mut h, mut stride) = (0u32, 0u32, 0u32);
        let ptr =
            unsafe { sys::optics_gfx_canvas_read_pixels(self.raw, &mut w, &mut h, &mut stride) };
        if ptr.is_null() {
            return None;
        }
        let len = (h as usize) * (stride as usize);
        // SAFETY: ptr is a valid buffer of `len` bytes owned by the canvas and
        // stable until the next read_pixels / destroy.
        let slice = unsafe { std::slice::from_raw_parts(ptr, len) };
        Some((w, h, stride, slice.to_vec()))
    }

    /// Set the content scale (device-pixel ratio). The canvas then draws in
    /// logical units mapped onto the physical surface, and `optics-text`
    /// rasterises glyphs to match — set this once when the surface scale
    /// changes instead of applying a per-frame [`CanvasCommands::scale`].
    pub fn set_scale(&mut self, scale: f32) {
        unsafe { sys::optics_gfx_canvas_set_scale(self.raw, scale) };
    }

    /// The current content scale (device-pixel ratio).
    pub fn content_scale(&self) -> f32 {
        unsafe { sys::optics_gfx_canvas_get_scale(self.raw) }
    }

    /// Borrow the raw Canvas for unsafe integration.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_canvas {
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
        Self { offset, color }
    }
}

pub(crate) fn gradient_stops(stops: &[GradientStop]) -> Vec<sys::optics_gfx_gradient_stop> {
    stops
        .iter()
        .map(|stop| sys::optics_gfx_gradient_stop {
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
            unsafe { sys::optics_gfx_canvas_release(self.raw) };
        }
    }
}

/// Convert straight RGBA components (0–255) into gfx's premultiplied
/// `optics_gfx_color` representation.
///
/// Canvas blending is premultiplied SRC_OVER, so public callers should pass
/// ordinary straight-alpha colour components here and let the binding enforce
/// the storage contract. Calling the raw `optics_gfx_color_rgba` packer with a
/// translucent colour would otherwise create bright opaque-looking fringes.
pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> u32 {
    unsafe { sys::optics_gfx_color_rgba_premul(r, g, b, a) }
}

/// Bucket an extent value to the nearest multiple of `step` (e.g. 128px),
/// preventing dynamic size sweeps from thrashing render-target allocations.
#[inline]
pub fn bucket_extent(extent: u32, step: u32) -> u32 {
    if extent == 0 || step == 0 {
        0
    } else {
        extent.div_ceil(step) * step
    }
}

/// A CPU-side bump allocator (`optics_gfx_arena`). gfx's per-frame value types —
/// paths, and the glyph quads that gfx-text emits — are allocated from one of
/// these and discarded wholesale via [`Arena::reset`] each frame.
///
/// The handle owns a stable heap allocation for the underlying `optics_gfx_arena`
/// struct so its address can be handed to C (`as_raw`) and stay put.
pub struct Arena {
    raw: *mut sys::optics_gfx_arena,
}

impl Arena {
    /// Create an arena backed by `capacity` bytes from the default (libc)
    /// allocator.
    pub fn with_capacity(capacity: usize) -> Result<Arena, Error> {
        // Box gives the optics_gfx_arena struct a fixed address for the C side.
        let raw = Box::into_raw(Box::new(unsafe {
            std::mem::zeroed::<sys::optics_gfx_arena>()
        }));
        let rc = unsafe { sys::optics_gfx_arena_init(raw, capacity, std::ptr::null()) };
        if let Err(e) = Error::check(rc) {
            // Reclaim the box; init failed so there is nothing to destroy.
            drop(unsafe { Box::from_raw(raw) });
            return Err(e);
        }
        Ok(Arena { raw })
    }

    /// Drop everything allocated since creation/last reset, keeping the buffer.
    pub fn reset(&mut self) {
        unsafe { sys::optics_gfx_arena_reset(self.raw) };
    }

    /// The underlying `optics_gfx_arena` pointer. Borrowed; the `Arena` retains
    /// ownership. Hand this to gfx-text (cast to its ABI-identical
    /// `optics_gfx_arena` type at the binding seam).
    pub fn as_raw(&self) -> *mut sys::optics_gfx_arena {
        self.raw
    }
}

impl Drop for Arena {
    fn drop(&mut self) {
        // SAFETY: raw came from Box::into_raw and was initialised by
        // optics_gfx_arena_init; destroy frees the buffer, then we reclaim the box.
        unsafe {
            sys::optics_gfx_arena_deinit(self.raw);
            drop(Box::from_raw(self.raw));
        }
    }
}
