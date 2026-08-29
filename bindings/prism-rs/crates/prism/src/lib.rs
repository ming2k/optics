//! Safe Rust bindings to **prism** — the material library of the optics
//! stack, built on flux's public effect runtime. flux owns rendering
//! mechanism, prism owns material identity, and the caller owns policy.
//!
//! Materials:
//! - [`LiquidGlassFilter`]: analytic convex-lens liquid glass compositor.
//! - [`FrostedFilter`]: classic non-distorting frosted glass compositor.
//! - [`AcrylicFilter`]: tactile acrylic material with procedural grain & luminance plate.
//! - [`BackdropLayerFilter`]: layered backdrop compositor — frosted sheet(s)
//!   carrying liquid-glass bodies, nested in one dispatch.

#![deny(rust_2018_idioms)]

use std::marker::PhantomData;

/// Raw-bindings escape hatch (see the `flux` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
/// Raw-bindings escape hatch (see the `flux` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
pub use prism_sys as sys;

/// A prism failure, surfaced through flux's result codes: every fallible
/// prism call returns `flux_result`, so the error type is flux's own.
pub use flux::Error;

/// Library version string ("0.0.29" at the time of writing), derived from
/// the `PRISM_VERSION_*` macros — not a hardcoded literal.
pub fn version() -> &'static str {
    unsafe {
        std::ffi::CStr::from_ptr(sys::prism_version_string())
            .to_str()
            .unwrap_or("?")
    }
}

/// Map a raw result to `Ok(())` on `FLUX_OK`, else `Err`.
fn check(rc: sys::flux_result) -> Result<(), Error> {
    if rc == sys::flux_result::FLUX_OK {
        Ok(())
    } else {
        Err(Error(rc))
    }
}

// -----------------------------------------------------------------------------
// Liquid Glass
// -----------------------------------------------------------------------------

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
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LiquidGlassFocus {
    pub shape: LiquidGlassShape,
    pub strength: f32,
}

/// One independently composited glass body.
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
    pub size_reference: f32,
    pub size_scale_min: f32,
    pub tint_strength: f32,
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

/// Per-group backdrop statistics.
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct BackdropStats {
    pub mean_luminance: f32,
    pub high_freq_energy: f32,
}

const _: () = assert!(
    std::mem::size_of::<BackdropStats>() == std::mem::size_of::<sys::prism_backdrop_stat>()
);
const _: () = assert!(
    std::mem::align_of::<BackdropStats>() == std::mem::align_of::<sys::prism_backdrop_stat>()
);

/// Reusable analytic liquid-glass compositor.
pub struct LiquidGlassFilter {
    raw: *mut sys::prism_liquid_glass_filter,
}

impl LiquidGlassFilter {
    pub fn new(device: &flux::Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::prism_liquid_glass_filter_create(device.as_raw(), &mut raw) })?;
        Ok(Self { raw })
    }

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
        unsafe { sys::prism_liquid_glass_filter_release(self.raw) };
    }
}

pub struct LiquidGlassImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut LiquidGlassFilter>,
}

impl LiquidGlassImage<'_> {
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

// -----------------------------------------------------------------------------
// Layered Backdrop (frost + glass)
// -----------------------------------------------------------------------------

/// One frosted rectangle of a [`BackdropLayerFilter`] layer stack: the
/// blurred backdrop written with rounded-rect SDF coverage. Frost rects
/// always paint beneath every glass body — the layer order is the
/// material's identity, not caller policy.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BackdropFrost {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub corner_radius: f32,
    /// `[0, 1]`; blends frosted-vs-sharp (the rect writes an opaque
    /// resolve, never a transparent fragment).
    pub opacity: f32,
    /// `0xRRGGBB` wash blended into the frost, beneath the glass.
    pub tint_color: [u8; 3],
    /// `[0, 1]`; `0.0` keeps the plain blurred backdrop.
    pub tint_strength: f32,
}

impl BackdropFrost {
    pub fn as_raw(&self) -> sys::prism_backdrop_frost {
        sys::prism_backdrop_frost {
            bounds: sys::flux_rect {
                x: self.x,
                y: self.y,
                w: self.width,
                h: self.height,
            },
            corner_radius: self.corner_radius,
            opacity: self.opacity,
            tint_color: ((self.tint_color[0] as u32) << 16)
                | ((self.tint_color[1] as u32) << 8)
                | (self.tint_color[2] as u32),
            tint_strength: self.tint_strength,
        }
    }
}

/// Reusable layered backdrop compositor: frosted rectangles carrying
/// analytic liquid-glass bodies, composed in one ordered dispatch into one
/// persistent transparent output. The glass lens samples the frosted image,
/// so a glass body bends and frosts the frost beneath it instead of looking
/// past it into the sharp capture — the nesting a standalone glass apply
/// cannot express.
pub struct BackdropLayerFilter {
    raw: *mut sys::prism_backdrop_layer_filter,
}

impl BackdropLayerFilter {
    /// Create the layered-backdrop filter (frost + glass compose in
    /// one pass; ADR-0079).
    pub fn new(device: &flux::Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::prism_backdrop_layer_filter_create(device.as_raw(), &mut raw) })?;
        Ok(Self { raw })
    }

    #[allow(clippy::too_many_arguments)]
    /// Frost the backdrop, then refract the glass groups through it —
    /// one call replacing a manual frost+blur+glass chain.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &flux::Frame<'_>,
        input: &flux::Image,
        blurred: &flux::BlurredImage<'_>,
        frost: &[BackdropFrost],
        groups: &[LiquidGlassGroup],
        params: LiquidGlassParams,
    ) -> Result<BackdropLayerImage<'filter>, Error> {
        let raw_frost: Vec<sys::prism_backdrop_frost> =
            frost.iter().map(BackdropFrost::as_raw).collect();
        let raw_groups: Vec<sys::prism_liquid_glass_group> =
            groups.iter().map(LiquidGlassGroup::as_raw).collect();
        let desc = sys::prism_backdrop_layer_desc {
            type_: sys::prism_struct_type::PRISM_TYPE_BACKDROP_LAYER_DESC,
            next: std::ptr::null(),
            input: input.as_raw(),
            blurred_input: blurred.as_raw(),
            frost: if raw_frost.is_empty() {
                std::ptr::null()
            } else {
                raw_frost.as_ptr()
            },
            frost_count: u32::try_from(raw_frost.len()).unwrap_or(u32::MAX),
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
        };
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::prism_backdrop_layer_filter_apply(self.raw, frame.as_raw(), &desc, &mut raw)
        })?;
        Ok(BackdropLayerImage {
            raw,
            _filter: PhantomData,
        })
    }

    /// Glass statistics of this frame slot's previous submission; see
    /// [`LiquidGlassFilter::stats`].
    pub fn stats(
        &mut self,
        frame: &flux::Frame<'_>,
        out: &mut [BackdropStats],
    ) -> Result<usize, Error> {
        let mut count = 0u32;
        check(unsafe {
            sys::prism_backdrop_layer_filter_stats(
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

impl Drop for BackdropLayerFilter {
    fn drop(&mut self) {
        unsafe { sys::prism_backdrop_layer_filter_release(self.raw) };
    }
}

/// Borrowed output of a [`BackdropLayerFilter`] application: the complete
/// frost + glass stack, transparent outside every frost rect and glass
/// silhouette. Draw it over the sharp desktop to complete the composite.
pub struct BackdropLayerImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut BackdropLayerFilter>,
}

impl BackdropLayerImage<'_> {
    /// Draw the leased output into `canvas` at `(x, y)`, `width`x`height`
    /// logical px (the canvas transform applies).
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

// -----------------------------------------------------------------------------
// Frosted Glass
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FrostedShape {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub corner_radius: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FrostedGroup {
    pub shape: FrostedShape,
    pub opacity: f32,
    pub tint_color: [u8; 3],
    pub tint_strength: Option<f32>,
    pub saturation: Option<f32>,
    pub shadow_alpha: f32,
    pub shadow_blur: f32,
    pub shadow_offset_y: f32,
    pub noise_intensity: Option<f32>,
}

impl FrostedGroup {
    /// Translate to the C descriptor (owned copy).
    pub fn as_raw(&self) -> sys::prism_frosted_group {
        let sentinel = |v: Option<f32>| v.unwrap_or(-1.0);
        sys::prism_frosted_group {
            shape: sys::prism_frosted_shape {
                bounds: sys::flux_rect {
                    x: self.shape.x,
                    y: self.shape.y,
                    w: self.shape.width,
                    h: self.shape.height,
                },
                corner_radius: self.shape.corner_radius,
            },
            opacity: self.opacity,
            tint_color: (u32::from(self.tint_color[0]) << 16)
                | (u32::from(self.tint_color[1]) << 8)
                | u32::from(self.tint_color[2]),
            tint_strength: sentinel(self.tint_strength),
            saturation: sentinel(self.saturation),
            shadow_alpha: self.shadow_alpha,
            shadow_blur: self.shadow_blur,
            shadow_offset_y: self.shadow_offset_y,
            noise_intensity: sentinel(self.noise_intensity),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FrostedParams {
    pub opacity: f32,
    pub saturation: f32,
    pub tint_strength: f32,
    pub noise_intensity: f32,
}

impl Default for FrostedParams {
    fn default() -> Self {
        Self {
            opacity: 1.0,
            saturation: 1.25,
            tint_strength: 0.15,
            noise_intensity: 0.015,
        }
    }
}

pub struct FrostedFilter {
    raw: *mut sys::prism_frosted_filter,
}

impl FrostedFilter {
    /// Create the frosted-glass filter.
    pub fn new(device: &flux::Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::prism_frosted_filter_create(device.as_raw(), &mut raw) })?;
        Ok(Self { raw })
    }

    /// Blur-and-tint `input` per `groups`; returns the leased output
    /// image (valid until next `apply` or drop).
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &flux::Frame<'_>,
        input: &flux::Image,
        blurred: &flux::BlurredImage<'_>,
        groups: &[FrostedGroup],
        params: FrostedParams,
    ) -> Result<FrostedImage<'filter>, Error> {
        let raw_groups: Vec<sys::prism_frosted_group> =
            groups.iter().map(FrostedGroup::as_raw).collect();
        let desc = sys::prism_frosted_desc {
            type_: sys::prism_struct_type::PRISM_TYPE_FROSTED_DESC,
            input: input.as_raw(),
            blurred_input: blurred.as_raw(),
            groups: if raw_groups.is_empty() {
                std::ptr::null()
            } else {
                raw_groups.as_ptr()
            },
            group_count: u32::try_from(raw_groups.len()).unwrap_or(u32::MAX),
            opacity: params.opacity,
            saturation: params.saturation,
            tint_strength: params.tint_strength,
            noise_intensity: params.noise_intensity,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::prism_frosted_filter_apply(self.raw, frame.as_raw(), &desc, &mut raw)
        })?;
        Ok(FrostedImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for FrostedFilter {
    fn drop(&mut self) {
        unsafe { sys::prism_frosted_filter_release(self.raw) };
    }
}

pub struct FrostedImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut FrostedFilter>,
}

impl FrostedImage<'_> {
    /// Draw the leased output into `canvas` at `(x, y)`, `width`x`height`
    /// logical px (the canvas transform applies).
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

// -----------------------------------------------------------------------------
// Acrylic
// -----------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AcrylicShape {
    pub x: f32,
    pub y: f32,
    pub width: f32,
    pub height: f32,
    pub corner_radius: f32,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AcrylicGroup {
    pub shape: AcrylicShape,
    pub opacity: f32,
    pub tint_color: [u8; 3],
    pub tint_strength: Option<f32>,
    pub luminance_plate: Option<f32>,
    pub noise_intensity: Option<f32>,
    pub border_width: Option<f32>,
    pub border_alpha: Option<f32>,
    pub shadow_alpha: f32,
    pub shadow_blur: f32,
    pub shadow_offset_y: f32,
}

impl AcrylicGroup {
    /// Translate to the C descriptor (owned copy).
    pub fn as_raw(&self) -> sys::prism_acrylic_group {
        let sentinel = |v: Option<f32>| v.unwrap_or(-1.0);
        sys::prism_acrylic_group {
            shape: sys::prism_acrylic_shape {
                bounds: sys::flux_rect {
                    x: self.shape.x,
                    y: self.shape.y,
                    w: self.shape.width,
                    h: self.shape.height,
                },
                corner_radius: self.shape.corner_radius,
            },
            opacity: self.opacity,
            tint_color: (u32::from(self.tint_color[0]) << 16)
                | (u32::from(self.tint_color[1]) << 8)
                | u32::from(self.tint_color[2]),
            tint_strength: sentinel(self.tint_strength),
            luminance_plate: sentinel(self.luminance_plate),
            noise_intensity: sentinel(self.noise_intensity),
            border_width: sentinel(self.border_width),
            border_alpha: sentinel(self.border_alpha),
            shadow_alpha: self.shadow_alpha,
            shadow_blur: self.shadow_blur,
            shadow_offset_y: self.shadow_offset_y,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AcrylicParams {
    pub opacity: f32,
    pub tint_strength: f32,
    pub luminance_plate: f32,
    pub noise_intensity: f32,
    pub border_width: f32,
    pub border_alpha: f32,
}

impl Default for AcrylicParams {
    fn default() -> Self {
        Self {
            opacity: 1.0,
            tint_strength: 0.25,
            luminance_plate: 0.5,
            noise_intensity: 0.02,
            border_width: 1.0,
            border_alpha: 0.12,
        }
    }
}

pub struct AcrylicFilter {
    raw: *mut sys::prism_acrylic_filter,
}

impl AcrylicFilter {
    /// Create the acrylic filter (procedural grain + luminance plate).
    pub fn new(device: &flux::Device) -> Result<Self, Error> {
        let mut raw = std::ptr::null_mut();
        check(unsafe { sys::prism_acrylic_filter_create(device.as_raw(), &mut raw) })?;
        Ok(Self { raw })
    }

    /// Render the acrylic material for `groups` from `input`.
    pub fn apply<'filter>(
        &'filter mut self,
        frame: &flux::Frame<'_>,
        input: &flux::Image,
        blurred: &flux::BlurredImage<'_>,
        groups: &[AcrylicGroup],
        params: AcrylicParams,
    ) -> Result<AcrylicImage<'filter>, Error> {
        let raw_groups: Vec<sys::prism_acrylic_group> =
            groups.iter().map(AcrylicGroup::as_raw).collect();
        let desc = sys::prism_acrylic_desc {
            type_: sys::prism_struct_type::PRISM_TYPE_ACRYLIC_DESC,
            input: input.as_raw(),
            blurred_input: blurred.as_raw(),
            groups: if raw_groups.is_empty() {
                std::ptr::null()
            } else {
                raw_groups.as_ptr()
            },
            group_count: u32::try_from(raw_groups.len()).unwrap_or(u32::MAX),
            opacity: params.opacity,
            tint_strength: params.tint_strength,
            luminance_plate: params.luminance_plate,
            noise_intensity: params.noise_intensity,
            border_width: params.border_width,
            border_alpha: params.border_alpha,
            ..Default::default()
        };
        let mut raw = std::ptr::null_mut();
        check(unsafe {
            sys::prism_acrylic_filter_apply(self.raw, frame.as_raw(), &desc, &mut raw)
        })?;
        Ok(AcrylicImage {
            raw,
            _filter: PhantomData,
        })
    }
}

impl Drop for AcrylicFilter {
    fn drop(&mut self) {
        unsafe { sys::prism_acrylic_filter_release(self.raw) };
    }
}

pub struct AcrylicImage<'filter> {
    raw: *mut sys::flux_image,
    _filter: PhantomData<&'filter mut AcrylicFilter>,
}

impl AcrylicImage<'_> {
    /// Draw the leased output into `canvas` at `(x, y)`, `width`x`height`
    /// logical px (the canvas transform applies).
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
