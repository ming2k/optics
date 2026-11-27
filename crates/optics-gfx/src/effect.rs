//! Blur and shadow image effects.

use super::*;

/// Reusable fixed-cost blur with a two-level Dual-Kawase pyramid per
/// frame-in-flight slot. This is the live-compositor path: blur width changes
/// sample offsets rather than dynamic kernel length, repeated calls do not
/// grow the transient effect pool, and no device-wide wait is required.
pub struct BlurFilter {
    raw: *mut sys::optics_gfx_blur_filter,
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
        Error::check(unsafe { sys::optics_gfx_blur_filter_create(device.raw, &mut raw) })?;
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
        let raw_regions: Vec<sys::optics_gfx_effect_region> = regions
            .iter()
            .map(|region| sys::optics_gfx_effect_region {
                x: region.x,
                y: region.y,
                width: region.width,
                height: region.height,
            })
            .collect();
        let region_count = u32::try_from(raw_regions.len())
            .map_err(|_| Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_OUT_OF_RANGE))?;
        let region_desc = sys::optics_gfx_effect_blur_regions_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_EFFECT_BLUR_REGIONS_DESC,
            regions: if raw_regions.is_empty() {
                std::ptr::null()
            } else {
                raw_regions.as_ptr()
            },
            region_count,
            ..Default::default()
        };
        // Keep the struct update even though all current fields are set:
        // it future-proofs against optics_gfx_effect_blur_desc growing fields.
        #[allow(clippy::needless_update)]
        let desc = sys::optics_gfx_effect_blur_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_EFFECT_BLUR_DESC,
            next: if raw_regions.is_empty() {
                std::ptr::null()
            } else {
                (&region_desc as *const sys::optics_gfx_effect_blur_regions_desc).cast()
            },
            input: input.raw,
            sigma,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        let opts = sys::optics_gfx_blur_filter_apply_opts {
            frame: frame.raw,
            desc: &desc,
            out: &mut raw,
        };
        Error::check(unsafe { sys::optics_gfx_blur_filter_apply(self.raw, &opts) })?;
        Ok(BlurredImage {
            raw,
            _filter: PhantomData,
        })
    }

    /// Borrow the slot's current valid blurred output without re-recording
    /// Dual-Kawase pyramid passes into this frame's command buffer.
    ///
    /// Returns [`Error`] if the frame slot has no recorded blur yet or if its
    /// targets were invalidated. Used by material-only updates (such as backdrop
    /// cover fade) where the source scene capture and blur sigma are unchanged.
    pub fn current<'filter>(
        &'filter mut self,
        frame: &Frame<'_>,
    ) -> Result<BlurredImage<'filter>, Error> {
        let mut raw = std::ptr::null_mut();
        let opts = sys::optics_gfx_blur_filter_current_opts {
            frame: frame.raw,
            out: &mut raw,
        };
        Error::check(unsafe { sys::optics_gfx_blur_filter_current(self.raw, &opts) })?;
        Ok(BlurredImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for BlurFilter {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_blur_filter_release(self.raw) };
    }
}

/// Borrowed output of a [`BlurFilter`] application.
pub struct BlurredImage<'filter> {
    raw: *mut sys::optics_gfx_image,
    _filter: PhantomData<&'filter mut BlurFilter>,
}

impl BlurredImage<'_> {
    /// Draw the blurred output through the Canvas image pipeline.
    pub fn draw(&self, canvas: &CanvasCommands<'_>, x: f32, y: f32, width: f32, height: f32) {
        canvas.draw_image_rect_internal(
            self.raw,
            std::ptr::null_mut(),
            sys::optics_gfx_rect {
                x,
                y,
                w: width,
                h: height,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            CornerRadii::ZERO,
            0.0,
            CornerRadii::ZERO,
            0.0,
            0xFFFFFFFF,
            sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            false,
        );
    }

    /// Raw `optics_gfx_image` pointer of the borrowed blur output. Borrowed: it
    /// stays valid only while this `BlurredImage` (its filter's frame slot)
    /// is alive. For consumers that pass the blurred image to their own
    /// compute or effect stage.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_image {
        self.raw
    }
}

/// Parameters of the [`effect_shadow`] drop-shadow operator (ADR-0074):
/// a shape mask in, a premultiplied tinted shadow out. The mask's alpha
/// channel is the shape coverage; a canvas capture of an opaque shape on
/// transparent is the typical source.
#[derive(Debug, Clone, Copy)]
pub struct ShadowParams {
    /// Gaussian sigma in pixels, clamped to `[0, 64]`. `0.0` is a hard
    /// (unblurred) offset copy of the mask.
    pub blur: f32,
    /// Shadow offset in input pixels: `+x` right, `+y` down.
    pub offset_x: f32,
    pub offset_y: f32,
    /// Straight (unpremultiplied) tint colour in the input's colour space.
    pub tint_red: f32,
    pub tint_green: f32,
    pub tint_blue: f32,
    /// Shadow opacity, clamped to `[0, 1]`.
    pub alpha: f32,
}

impl Default for ShadowParams {
    fn default() -> Self {
        ShadowParams {
            blur: 0.0,
            offset_x: 0.0,
            offset_y: 0.0,
            tint_red: 0.0,
            tint_green: 0.0,
            tint_blue: 0.0,
            alpha: 1.0,
        }
    }
}

impl ShadowParams {
    pub(crate) fn into_desc(self, input: &Image) -> sys::optics_gfx_effect_shadow_desc {
        // Keep the struct update even though all current fields are set:
        // it future-proofs against optics_gfx_effect_shadow_desc growing fields.
        #[allow(clippy::needless_update)]
        let desc = sys::optics_gfx_effect_shadow_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC,
            next: std::ptr::null(),
            input: input.raw,
            blur: self.blur,
            offset_x: self.offset_x,
            offset_y: self.offset_y,
            tint_red: self.tint_red,
            tint_green: self.tint_green,
            tint_blue: self.tint_blue,
            alpha: self.alpha,
            ..Default::default()
        };
        desc
    }
}

/// Record the drop-shadow operator into `cmd` (ADR-0074). The output is
/// leased from the device's transient effect pool and stays valid until
/// the next `effect_reset` on the same device — mirror the C lifetime
/// contract of `optics_gfx_effect_shadow` (see `docs/reference/effect.md`).
///
/// # Safety
///
/// `cmd` must be a valid, recording `VkCommandBuffer` on the device that
/// created `input`. The caller must serialize effect calls per device.
pub unsafe fn effect_shadow(
    cmd: sys::VkCommandBuffer,
    input: &Image,
    params: ShadowParams,
) -> Result<EffectImage, Error> {
    let desc = params.into_desc(input);
    let mut raw: *mut sys::optics_gfx_image = std::ptr::null_mut();
    let opts = sys::optics_gfx_effect_shadow_opts {
        cmd,
        desc: &desc,
        out: &mut raw,
    };
    // SAFETY: caller guarantees `cmd` is a valid recording command buffer on
    // input's device and effect calls are serialized per device.
    Error::check(unsafe { sys::optics_gfx_effect_shadow(&opts) })?;
    Ok(EffectImage { raw })
}

/// Transient effect output (from [`effect_shadow`]). Borrowed from the
/// per-device pool: valid until `effect_reset` on that device. It is not
/// released on drop — the pool owns it for the whole lease epoch.
pub struct EffectImage {
    raw: *mut sys::optics_gfx_image,
}

impl EffectImage {
    /// Raw `optics_gfx_image` pointer of the leased effect output.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_image {
        self.raw
    }

    /// Draw the leased effect output through the Canvas image pipeline.
    /// Same borrowed-lifetime rules as [`Self::as_raw`]: the pool owns the
    /// image until the next `effect_reset` on the device.
    pub fn draw(&self, canvas: &CanvasCommands<'_>, x: f32, y: f32, width: f32, height: f32) {
        canvas.draw_image_rect_internal(
            self.raw,
            std::ptr::null_mut(),
            sys::optics_gfx_rect {
                x,
                y,
                w: width,
                h: height,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            CornerRadii::ZERO,
            0.0,
            CornerRadii::ZERO,
            0.0,
            0xFFFFFFFF,
            sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            false,
        );
    }

    /// Promote the leased output into a caller-owned, refcounted image
    /// unaffected by `effect_reset`. Synchronous: submits a one-shot
    /// command buffer and waits. The caller must have synchronised any
    /// prior submission that wrote this image.
    pub fn promote(&self) -> Result<Image, Error> {
        let mut out: *mut sys::optics_gfx_image = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_effect_promote(self.raw, &mut out) })?;
        Ok(Image { raw: out })
    }
}

/// Reusable, frame-slot-safe realtime drop shadow (ADR-0074) — the filter
/// counterpart of [`effect_shadow`], mirroring [`BlurFilter`]'s ownership
/// model. The exact operator leases intermediates from the device's
/// transient pool per call, which a per-frame compositor would accumulate
/// until `optics_gfx_effect_reset`; this filter instead owns its intermediates and
/// output per frame-in-flight slot, so driving it every frame grows nothing.
///
/// Use one filter per surface/frame stream. Blur, offset, tint, and alpha
/// vary per call without pipeline churn; the dispatch shape is fixed per
/// input extent.
pub struct ShadowFilter {
    raw: *mut sys::optics_gfx_shadow_filter,
}

impl ShadowFilter {
    pub fn new(device: &Device) -> Result<ShadowFilter, Error> {
        let mut raw = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_shadow_filter_create(device.raw, &mut raw) })?;
        Ok(ShadowFilter { raw })
    }

    /// Record the shadow for this frame's reusable slot. The returned image
    /// borrows the filter, preventing that slot's storage from being
    /// replaced while it is composed into the frame.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &Frame<'_>,
        input: &Image,
        params: ShadowParams,
    ) -> Result<ShadowedImage<'filter>, Error> {
        let desc = params.into_desc(input);
        let mut raw: *mut sys::optics_gfx_image = std::ptr::null_mut();
        let opts = sys::optics_gfx_shadow_filter_apply_opts {
            frame: frame.raw,
            desc: &desc,
            out: &mut raw,
        };
        // SAFETY: the filter, frame, and input all belong to one device (the
        // C side verifies); the frame is at a pass boundary and recording.
        Error::check(unsafe { sys::optics_gfx_shadow_filter_apply(self.raw, &opts) })?;
        Ok(ShadowedImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for ShadowFilter {
    fn drop(&mut self) {
        unsafe { sys::optics_gfx_shadow_filter_release(self.raw) };
    }
}

/// Borrowed output of a [`ShadowFilter`] application: a premultiplied,
/// tinted shadow image valid until the same slot is applied again or the
/// filter is released.
pub struct ShadowedImage<'filter> {
    raw: *mut sys::optics_gfx_image,
    _filter: PhantomData<&'filter mut ShadowFilter>,
}

impl ShadowedImage<'_> {
    /// Draw the shadow output through the Canvas image pipeline. Draw it
    /// beneath the window/chrome it belongs to; the image is premultiplied
    /// (rgb = tint * a), so straight `draw_image` blending composites
    /// correctly.
    pub fn draw(&self, canvas: &CanvasCommands<'_>, x: f32, y: f32, width: f32, height: f32) {
        canvas.draw_image_rect_internal(
            self.raw,
            std::ptr::null_mut(),
            sys::optics_gfx_rect {
                x,
                y,
                w: width,
                h: height,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            sys::optics_gfx_rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            CornerRadii::ZERO,
            0.0,
            CornerRadii::ZERO,
            0.0,
            0xFFFFFFFF,
            sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            false,
        );
    }

    /// Raw `optics_gfx_image` pointer of the borrowed shadow output, for sibling
    /// binding crates that compose it themselves.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_image {
        self.raw
    }
}
