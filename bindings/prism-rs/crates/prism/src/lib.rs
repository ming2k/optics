//! Safe Rust bindings to **prism** — the material library of the optics
//! stack, built on flux's public effect runtime. flux owns rendering
//! mechanism, prism owns material identity, and the caller owns policy.
//!
//! Today the library covers one material: the analytic **liquid glass**
//! compositor ([`LiquidGlassFilter`]). `input` is the sharp backdrop capture
//! and `blurred` is normally [`flux::BlurFilter::apply`] for the same
//! capture; both inputs and the returned image share the same extent. The
//! output is transparent outside the exact analytic SDF plus the drop-shadow
//! falloff, so drawing it over the sharp backdrop performs the complete
//! glass composite without a separate rectangular clip.
//!
//! The raw bindings live in [`prism_sys`] (re-exported as [`sys`]). prism's
//! C API speaks flux types; those are shared with the [`flux`] crate through
//! `flux-sys` rather than duplicated, so [`flux::Error`] is reused verbatim.

#![deny(rust_2018_idioms)]

use std::marker::PhantomData;

pub use prism_sys as sys;

/// A prism failure, surfaced through flux's result codes: every fallible
/// prism call returns `flux_result`, so the error type is flux's own.
pub use flux::Error;

/// Map a raw result to `Ok(())` on `FLUX_OK`, else `Err`.
fn check(rc: sys::flux_result) -> Result<(), Error> {
    if rc == sys::flux_result::FLUX_OK {
        Ok(())
    } else {
        Err(Error(rc))
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
///
/// The five `Option` knobs override dispatch-wide [`LiquidGlassParams`]
/// policy per body and carry the adaptive-plate inputs (`None` maps to the
/// C header's `<0` sentinel — inherit / legacy behaviour):
/// - `frost_strength` / `tint_strength` / `saturation`: per-body override of
///   the same params knob;
/// - `plate_polarity`: the caller-chosen adaptive-tint polarity in `[0, 1]`
///   — `0.0` pins the smoke plate, `1.0` the pearl plate — uniform for the
///   whole body instead of the legacy per-pixel polarity. Text-bearing
///   surfaces pin the polarity that opposes their text tone;
///   backdrop-derived polarity can be computed from
///   [`LiquidGlassFilter::stats`];
/// - `backdrop_energy`: region high-frequency energy in `[0, 1]`, boosting
///   the body tint over busy backdrops.
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
    pub frost_strength: Option<f32>,
    pub tint_strength: Option<f32>,
    pub saturation: Option<f32>,
    pub plate_polarity: Option<f32>,
    pub backdrop_energy: Option<f32>,
}

impl LiquidGlassGroup {
    /// Map to the raw C struct. `None` becomes the `-1.0` inherit/disabled
    /// sentinel; `Some` values are passed verbatim (the C side validates
    /// finiteness and the `[0, 1]` range of the adaptive inputs).
    pub fn as_raw(&self) -> sys::prism_liquid_glass_group {
        let raw_shape = |shape: LiquidGlassShape| sys::prism_liquid_glass_shape {
            bounds: sys::flux_rect {
                x: shape.x,
                y: shape.y,
                w: shape.width,
                h: shape.height,
            },
            corner_radius: shape.corner_radius,
        };
        let sentinel = |value: Option<f32>| value.unwrap_or(-1.0);
        let mut shapes = [raw_shape(self.primary), raw_shape(self.primary)];
        let shape_count = if let Some(merged) = self.merged {
            shapes[1] = raw_shape(merged);
            2
        } else {
            1
        };
        sys::prism_liquid_glass_group {
            shapes,
            shape_count,
            blend_radius: self.blend_radius,
            opacity: self.opacity,
            shadow_alpha: self.shadow_alpha,
            shadow_blur: self.shadow_blur,
            shadow_offset_y: self.shadow_offset_y,
            tint_color: (u32::from(self.tint_color[0]) << 16)
                | (u32::from(self.tint_color[1]) << 8)
                | u32::from(self.tint_color[2]),
            focus: self
                .focus
                .map(|focus| raw_shape(focus.shape))
                .unwrap_or_else(|| raw_shape(self.primary)),
            focus_strength: self.focus.map_or(0.0, |focus| focus.strength),
            frost_strength: sentinel(self.frost_strength),
            tint_strength: sentinel(self.tint_strength),
            saturation: sentinel(self.saturation),
            plate_polarity: sentinel(self.plate_polarity),
            backdrop_energy: sentinel(self.backdrop_energy),
        }
    }
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
    pub rim_light: f32,
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
            rim_light: 0.55,
            light_direction: (-0.45, -0.89),
            opacity: 1.0,
            size_reference: 72.0,
            size_scale_min: 0.15,
            tint_strength: 1.0,
            frost_strength: 1.0,
        }
    }
}

/// Per-group backdrop statistics measured by
/// [`LiquidGlassFilter::apply`]: the mean Rec.709 luminance of the blurred
/// backdrop over the group's primary body, and the mean |sharp − blurred|
/// luminance (high-frequency energy). What a caller does with the numbers —
/// mapping them onto [`LiquidGlassGroup::plate_polarity`] /
/// [`LiquidGlassGroup::backdrop_energy`], temporal smoothing — is caller
/// policy; prism only measures.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct BackdropStats {
    pub mean_luminance: f32,
    pub high_freq_energy: f32,
}

// The stats readback memcpy's the C array into `&mut [BackdropStats]`, so
// the two types must stay layout-identical (bindgen's own layout tests pin
// the C side).
const _: () = assert!(
    std::mem::size_of::<BackdropStats>() == std::mem::size_of::<sys::prism_backdrop_stat>()
);
const _: () = assert!(
    std::mem::align_of::<BackdropStats>() == std::mem::align_of::<sys::prism_backdrop_stat>()
);

/// Reusable analytic liquid-glass compositor with one output per frame slot.
///
/// Threading: filters are not thread-safe per device. Serialize calls per
/// device, as you would for any other flux recording API.
pub struct LiquidGlassFilter {
    raw: *mut sys::prism_liquid_glass_filter,
}

impl LiquidGlassFilter {
    pub fn new(device: &flux::Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::prism_liquid_glass_filter_create(device.as_raw(), &mut raw) })?;
        Ok(Self { raw })
    }

    /// Refract `input` through analytic rounded SDFs, mixing in the matching
    /// realtime `blurred` capture for local frost. The returned image is
    /// transparent outside the SDF and its drop-shadow falloff, and borrows
    /// this filter's frame slot. An empty `groups` slice clears footprints
    /// retained by that slot after all glass bodies disappear.
    ///
    /// Requires a recording frame with no active pass.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &flux::Frame<'_>,
        input: &flux::Image,
        blurred: &flux::BlurredImage<'_>,
        groups: &[LiquidGlassGroup],
        params: LiquidGlassParams,
    ) -> Result<LiquidGlassImage<'filter>, Error> {
        let raw_groups: Vec<sys::prism_liquid_glass_group> =
            groups.iter().map(LiquidGlassGroup::as_raw).collect();
        let desc = sys::prism_liquid_glass_desc {
            type_: sys::prism_struct_type::PRISM_TYPE_LIQUID_GLASS_DESC,
            input: input.as_raw(),
            blurred_input: blurred.as_raw(),
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
            rim_light: params.rim_light,
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
        check(unsafe {
            sys::prism_liquid_glass_filter_apply(self.raw, frame.as_raw(), &desc, &mut raw)
        })?;
        Ok(LiquidGlassImage {
            raw,
            _filter: PhantomData,
        })
    }

    /// Read the backdrop statistics this frame slot last submitted —
    /// `FLUX_MAX_FRAMES_IN_FLIGHT` frames ago: `begin_frame` has waited
    /// that slot's fence, so the mapped buffer is stable while `frame`
    /// records. Call before this frame's [`apply`](Self::apply) on the same
    /// slot; the numbers always describe the slot's previous submission.
    ///
    /// Returns the number of group stats copied into `out` (the minimum of
    /// `out.len()` and the group count that submission carried). Fails with
    /// `FLUX_ERROR_INVALID_STATE` when the slot has never been applied with
    /// stats.
    pub fn stats(
        &mut self,
        frame: &flux::Frame<'_>,
        out: &mut [BackdropStats],
    ) -> Result<usize, Error> {
        let mut count = 0u32;
        check(unsafe {
            sys::prism_liquid_glass_filter_stats(
                self.raw,
                frame.as_raw(),
                out.as_mut_ptr().cast::<sys::prism_backdrop_stat>(),
                u32::try_from(out.len()).unwrap_or(u32::MAX),
                &mut count,
            )
        })?;
        Ok(count as usize)
    }
}

impl Drop for LiquidGlassFilter {
    fn drop(&mut self) {
        // Release only after every submission that references the filter's
        // outputs has completed (its frame-slot fence has signalled, or
        // after device idle): release destroys the compute pipelines inline.
        unsafe { sys::prism_liquid_glass_filter_release(self.raw) };
    }
}

/// Borrowed full-capture liquid-glass composite.
pub struct LiquidGlassImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut LiquidGlassFilter>,
}

impl LiquidGlassImage<'_> {
    /// Draw the composite through the Canvas image pipeline.
    pub fn draw(&self, canvas: &flux::Canvas, x: f32, y: f32, width: f32, height: f32) {
        let destination = sys::flux_rect {
            x,
            y,
            w: width,
            h: height,
        };
        unsafe {
            flux_sys::flux_canvas_draw_image(
                canvas.as_raw(),
                self.raw,
                destination,
                std::ptr::null(),
            )
        };
    }
}
