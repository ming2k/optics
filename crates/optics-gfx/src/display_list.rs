//! Display lists and the encoder that records them.

use super::*;

/// An immutable, relocatable display list owning all command payloads (ADR-0088 / ADR-0090).
///
/// Produced exclusively by consuming an [`Encoder`]. Safe to share across threads or retain across frames.
#[derive(Debug)]
pub struct DisplayList {
    raw: *mut sys::optics_gfx_display_list,
}

impl DisplayList {
    pub(crate) unsafe fn from_raw(raw: *mut sys::optics_gfx_display_list) -> Self {
        Self { raw }
    }

    pub fn as_raw(&self) -> *mut sys::optics_gfx_display_list {
        self.raw
    }

    /// Number of serialized commands in this list.
    pub fn command_count(&self) -> u32 {
        if self.raw.is_null() {
            0
        } else {
            unsafe { sys::optics_gfx_display_list_command_count(self.raw) }
        }
    }

    /// Total byte size of serialized command payloads.
    pub fn size(&self) -> usize {
        if self.raw.is_null() {
            0
        } else {
            unsafe { sys::optics_gfx_display_list_size(self.raw) }
        }
    }

    /// Bit set of dynamic image input slots this list (including children) uses.
    pub fn input_mask(&self) -> u64 {
        if self.raw.is_null() {
            0
        } else {
            unsafe { sys::optics_gfx_display_list_input_mask(self.raw) }
        }
    }

    /// Pinned image-content bytes retained by this list, including children.
    pub fn retained_bytes(&self) -> usize {
        if self.raw.is_null() {
            0
        } else {
            unsafe { sys::optics_gfx_display_list_retained_bytes(self.raw) }
        }
    }
}

/// One execution binding for a dynamic image input slot (ADR-0090).
pub struct DisplayListBinding<'a> {
    pub slot: u32,
    pub image: &'a Image,
    pub sampler: Option<&'a Sampler>,
}

impl Clone for DisplayList {
    fn clone(&self) -> Self {
        if self.raw.is_null() {
            Self {
                raw: std::ptr::null_mut(),
            }
        } else {
            unsafe { Self::from_raw(sys::optics_gfx_display_list_retain(self.raw)) }
        }
    }
}

impl Drop for DisplayList {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::optics_gfx_display_list_release(self.raw) };
        }
    }
}

unsafe impl Send for DisplayList {}
unsafe impl Sync for DisplayList {}

/// A pure CPU, memory-only command encoder operating on an arena/buffer (ADR-0088 / ADR-0090).
///
/// Records draw and state commands concurrently on any thread with zero GPU dependencies.
/// Consumed into an immutable [`DisplayList`] via [`Encoder::finish`].
pub struct Encoder {
    raw: *mut sys::optics_gfx_encoder,
    owned: bool,
}

impl Encoder {
    /// Borrow the raw encoder for unsafe integration (e.g. a `-sys` crate that
    /// records into an encoder it does not own).
    pub fn as_raw(&self) -> *mut sys::optics_gfx_encoder {
        self.raw
    }
}

impl Encoder {
    /// Create a borrowed view over a raw `optics_gfx_encoder` pointer.
    ///
    /// # Safety
    /// `raw` must point to a live `optics_gfx_encoder`. The returned view does not
    /// destroy the encoder on drop.
    pub unsafe fn borrow_raw(raw: *mut sys::optics_gfx_encoder) -> Encoder {
        Encoder { raw, owned: false }
    }

    /// Create a new command encoder with the default budget (64 MiB).
    pub fn new() -> Result<Self, Error> {
        Self::with_limits(0, 0)
    }

    /// Bound owned storage and expanded execution, including shared children.
    /// A zero limit selects the C library default.
    pub fn with_limits(max_bytes: usize, max_commands: u32) -> Result<Self, Error> {
        Self::with_budget(max_bytes, max_commands, 0)
    }

    /// As [`Encoder::with_limits`], plus the pinned image-content budget.
    /// A zero retained budget is unbounded.
    pub fn with_budget(
        max_bytes: usize,
        max_commands: u32,
        max_retained_bytes: usize,
    ) -> Result<Self, Error> {
        let desc = sys::optics_gfx_encoder_desc {
            max_bytes,
            max_commands,
            max_retained_bytes,
        };
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_encoder_create(&desc, &mut raw) })?;
        Ok(Self { raw, owned: true })
    }

    /// Retain a published child under the current transform and clip.
    /// Changes to state inside the child cannot leak into following commands.
    pub fn draw_display_list(&mut self, list: &DisplayList) -> Result<(), Error> {
        // SAFETY: self uniquely owns the recorder; list is a live immutable owner.
        Error::check(unsafe { sys::optics_gfx_encoder_draw_display_list(self.raw, list.raw) })
    }

    /// Record a rectangle whose image is bound at execution time by `slot`.
    /// The list carries no image reference for this draw.
    pub fn draw_image_slot(
        &mut self,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        slot: u32,
        style: ImageStyle,
    ) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rect: sys::optics_gfx_geom_rect_data {
                    rect: sys::optics_gfx_rect { x, y, w, h },
                },
            },
        };
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_IMAGE_PATTERN,
            blend: style.blend.raw(),
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                image: sys::optics_gfx_brush_image_data {
                    image: std::ptr::null_mut(),
                    sampler: std::ptr::null_mut(),
                    src_rect: sys::optics_gfx_rect {
                        x: 0.0,
                        y: 0.0,
                        w: 0.0,
                        h: 0.0,
                    },
                    opaque_only: false,
                    tint: style.tint,
                    clip_rect: sys::optics_gfx_rect {
                        x: 0.0,
                        y: 0.0,
                        w: 0.0,
                        h: 0.0,
                    },
                    clip_radii: CornerRadii::ZERO.to_raw(),
                    clip_curvature: 0.0,
                },
            },
        };
        let opts = sys::optics_gfx_encoder_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
            slot,
            use_slot: true,
        };
        unsafe { sys::optics_gfx_encoder_draw_geometry(self.raw, &opts) };
    }

    /// Push canvas state (transform and scissor).
    pub fn save(&mut self) {
        unsafe { sys::optics_gfx_encoder_save(self.raw) };
    }

    /// Pop canvas state.
    pub fn restore(&mut self) {
        unsafe { sys::optics_gfx_encoder_restore(self.raw) };
    }

    /// Push a rectangular clip region.
    pub fn clip_rect(&mut self, rect: (f32, f32, f32, f32)) {
        let r = sys::optics_gfx_rect {
            x: rect.0,
            y: rect.1,
            w: rect.2,
            h: rect.3,
        };
        unsafe { sys::optics_gfx_encoder_clip_rect(self.raw, r) };
    }

    /// Push an isolated opacity layer (ADR-0091).
    pub fn save_layer(&mut self, bounds: Option<(f32, f32, f32, f32)>, opacity: f32) {
        let b = bounds.map(|(x, y, w, h)| sys::optics_gfx_rect { x, y, w, h });
        let b_ptr = b
            .as_ref()
            .map(|r| r as *const _)
            .unwrap_or(std::ptr::null());
        let opts = sys::optics_gfx_encoder_save_layer_opts {
            bounds: b_ptr,
            opacity,
        };
        unsafe { sys::optics_gfx_encoder_save_layer(self.raw, &opts) };
    }

    /// Apply an affine translation.
    pub fn translate(&mut self, x: f32, y: f32) {
        let opts = sys::optics_gfx_encoder_translate_opts { x, y };
        unsafe { sys::optics_gfx_encoder_translate(self.raw, &opts) };
    }

    /// Apply an affine scale.
    pub fn scale(&mut self, sx: f32, sy: f32) {
        let opts = sys::optics_gfx_encoder_scale_opts { sx, sy };
        unsafe { sys::optics_gfx_encoder_scale(self.raw, &opts) };
    }

    /// Apply a rotation in radians.
    pub fn rotate(&mut self, radians: f32) {
        unsafe { sys::optics_gfx_encoder_rotate(self.raw, radians) };
    }

    /// Finish recording and publish an immutable, relocatable [`DisplayList`] (ADR-0090).
    ///
    /// Consumes the encoder. An encoder cannot be recorded into after finish:
    /// ```compile_fail
    /// use optics_gfx::Encoder;
    /// let mut encoder = Encoder::new().unwrap();
    /// let dl = encoder.finish().unwrap();
    /// encoder.save();
    /// let _ = dl;
    /// ```
    pub fn finish(mut self) -> Result<DisplayList, Error> {
        let mut dl = std::ptr::null_mut();
        let res = Error::check(unsafe { sys::optics_gfx_encoder_finish(self.raw, &mut dl) });
        if self.owned {
            unsafe { sys::optics_gfx_encoder_destroy(self.raw) };
        }
        self.raw = std::ptr::null_mut();
        res?;
        Ok(unsafe { DisplayList::from_raw(dl) })
    }
}

impl Drop for Encoder {
    fn drop(&mut self) {
        if self.owned && !self.raw.is_null() {
            unsafe { sys::optics_gfx_encoder_destroy(self.raw) };
        }
    }
}
