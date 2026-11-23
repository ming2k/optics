//! Colour spaces, transfer functions and ICC profiles.

use super::*;

/// Named RGB primaries (mirrors `optics_gfx_color_primaries`). All named
/// primaries are D65; [`ColorPrimaries::Custom`] carries its own
/// chromaticities and white point (see [`ColorSpace::custom`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorPrimaries {
    /// sRGB gamut, D65.
    Bt709,
    DisplayP3,
    Bt2020,
    AdobeRgb,
    /// Chromaticities from [`PrimariesXy`].
    Custom,
}

impl ColorPrimaries {
    const fn raw(self) -> sys::optics_gfx_color_primaries {
        match self {
            Self::Bt709 => sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_BT709,
            Self::DisplayP3 => sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_DISPLAY_P3,
            Self::Bt2020 => sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_BT2020,
            Self::AdobeRgb => sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_ADOBE_RGB,
            Self::Custom => sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_CUSTOM,
        }
    }

    fn from_raw(raw: sys::optics_gfx_color_primaries) -> Self {
        match raw {
            sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_BT709 => Self::Bt709,
            sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_DISPLAY_P3 => Self::DisplayP3,
            sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_BT2020 => Self::Bt2020,
            sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_ADOBE_RGB => Self::AdobeRgb,
            sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_CUSTOM => Self::Custom,
        }
    }
}

/// Transfer function between linear-light and encoded values (mirrors
/// `optics_gfx_transfer_func`). PQ and HLG are absolute/nominal HDR curves
/// (ITU-R BT.2100): PQ linear values are in units of 10000 cd/m²
/// (1.0 = 10000 nits); HLG is nominal 0..1 scene light. The working-space
/// luminance scale follows scRGB: 1.0 = 80 cd/m².
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TransferFunction {
    Linear,
    Srgb,
    /// Pure power law; the exponent comes from [`ColorSpace`]'s gamma.
    Gamma,
    /// ST 2084.
    Pq,
    Hlg,
}

impl TransferFunction {
    const fn raw(self) -> sys::optics_gfx_transfer_func {
        match self {
            Self::Linear => sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_LINEAR,
            Self::Srgb => sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_SRGB,
            Self::Gamma => sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_GAMMA,
            Self::Pq => sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_PQ,
            Self::Hlg => sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_HLG,
        }
    }

    fn from_raw(raw: sys::optics_gfx_transfer_func) -> Self {
        match raw {
            sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_LINEAR => Self::Linear,
            sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_SRGB => Self::Srgb,
            sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_GAMMA => Self::Gamma,
            sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_PQ => Self::Pq,
            sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_HLG => Self::Hlg,
        }
    }

    /// Encode a linear-light value into this function's encoded domain.
    /// `gamma` is read only for [`TransferFunction::Gamma`]. The result
    /// clamps to the representable range.
    pub fn encode(self, gamma: f32, linear: f32) -> f32 {
        unsafe { sys::optics_gfx_transfer_encode(self.raw(), gamma, linear) }
    }

    /// Decode an encoded value back to linear light; the exact inverse of
    /// [`TransferFunction::encode`].
    pub fn decode(self, gamma: f32, encoded: f32) -> f32 {
        unsafe { sys::optics_gfx_transfer_decode(self.raw(), gamma, encoded) }
    }
}

/// CIE 1931 xy chromaticities of the red/green/blue primaries and the
/// white point — custom primaries for [`ColorSpace::custom`] and
/// mastering-display metadata for [`Hdr10Metadata`].
#[derive(Debug, Default, Clone, Copy, PartialEq)]
pub struct PrimariesXy {
    pub rx: f32,
    pub ry: f32,
    pub gx: f32,
    pub gy: f32,
    pub bx: f32,
    pub by: f32,
    /// White point.
    pub wx: f32,
    pub wy: f32,
}

const ZERO_XY: sys::optics_gfx_color_space__bindgen_ty_1 =
    sys::optics_gfx_color_space__bindgen_ty_1 {
        rx: 0.0,
        ry: 0.0,
        gx: 0.0,
        gy: 0.0,
        bx: 0.0,
        by: 0.0,
        wx: 0.0,
        wy: 0.0,
    };

/// A parametric color space: `{ primaries, transfer function }` — the
/// model VkColorSpaceKHR, DXGI, the Wayland color-management protocol and
/// CSS Color 4 have converged on (ADR-0069). Use the associated presets
/// ([`ColorSpace::SRGB`], [`ColorSpace::BT2020_PQ`], ...) or compose one
/// with [`ColorSpace::new`] / [`ColorSpace::custom`].
///
/// The C presets are brace-initializer macros bindgen cannot see, so they
/// are provided here as `const` values with identical contents. Equality
/// follows the C `optics_gfx_color_space_equal` semantics, not field-wise
/// comparison.
#[derive(Clone, Copy)]
pub struct ColorSpace {
    raw: sys::optics_gfx_color_space,
}

impl ColorSpace {
    /// sRGB (BT.709 primaries + sRGB transfer).
    pub const SRGB: Self = Self::new(ColorPrimaries::Bt709, TransferFunction::Srgb);
    /// Linear-light sRGB (BT.709 primaries, no transfer encoding).
    pub const SRGB_LINEAR: Self = Self::new(ColorPrimaries::Bt709, TransferFunction::Linear);
    /// scRGB: the fixed working space (extended linear BT.709,
    /// 1.0 = 80 nits). Same `{primaries, transfer}` as
    /// [`ColorSpace::SRGB_LINEAR`]; the distinct preset names the
    /// extended-range surface intent.
    pub const SCRGB: Self = Self::SRGB_LINEAR;
    pub const DISPLAY_P3: Self = Self::new(ColorPrimaries::DisplayP3, TransferFunction::Srgb);
    pub const DISPLAY_P3_LINEAR: Self =
        Self::new(ColorPrimaries::DisplayP3, TransferFunction::Linear);
    /// Adobe RGB (1998): the official profile's TRC is a pure 563/256 power.
    pub const ADOBE_RGB: Self =
        Self::new(ColorPrimaries::AdobeRgb, TransferFunction::Gamma).with_gamma(563.0 / 256.0);
    /// SDR BT.2020 per BT.1886 practice: pure 2.4 power.
    pub const BT2020: Self =
        Self::new(ColorPrimaries::Bt2020, TransferFunction::Gamma).with_gamma(2.4);
    /// HDR10 (BT.2020 primaries + ST 2084).
    pub const BT2020_PQ: Self = Self::new(ColorPrimaries::Bt2020, TransferFunction::Pq);
    pub const BT2020_HLG: Self = Self::new(ColorPrimaries::Bt2020, TransferFunction::Hlg);

    /// A space from named primaries and a transfer function. For
    /// [`TransferFunction::Gamma`] chain [`ColorSpace::with_gamma`] — a
    /// zero gamma is invalid.
    pub const fn new(primaries: ColorPrimaries, transfer: TransferFunction) -> Self {
        Self {
            raw: sys::optics_gfx_color_space {
                primaries: primaries.raw(),
                transfer: transfer.raw(),
                gamma: 0.0,
                xy: ZERO_XY,
            },
        }
    }

    /// Set the pure-power-law exponent (meaningful only with
    /// [`TransferFunction::Gamma`]).
    pub const fn with_gamma(mut self, gamma: f32) -> Self {
        self.raw.gamma = gamma;
        self
    }

    /// A space with custom CIE xy primaries and white point. Valid only
    /// with sane chromaticities (xy in (0,1), non-degenerate triangle,
    /// non-zero white) — see [`ColorSpace::is_valid`].
    pub const fn custom(xy: PrimariesXy, transfer: TransferFunction) -> Self {
        Self {
            raw: sys::optics_gfx_color_space {
                primaries: sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_CUSTOM,
                transfer: transfer.raw(),
                gamma: 0.0,
                xy: sys::optics_gfx_color_space__bindgen_ty_1 {
                    rx: xy.rx,
                    ry: xy.ry,
                    gx: xy.gx,
                    gy: xy.gy,
                    bx: xy.bx,
                    by: xy.by,
                    wx: xy.wx,
                    wy: xy.wy,
                },
            },
        }
    }

    /// The RGB primaries chromaticities this surface is tagged with.
    pub fn primaries(&self) -> ColorPrimaries {
        ColorPrimaries::from_raw(self.raw.primaries)
    }

    /// The transfer function (gamma encoding) this surface is tagged with.
    pub fn transfer(&self) -> TransferFunction {
        TransferFunction::from_raw(self.raw.transfer)
    }

    /// Pure-power-law exponent; 0 unless the transfer is
    /// [`TransferFunction::Gamma`].
    pub fn gamma(&self) -> f32 {
        self.raw.gamma
    }

    /// Whether gfx can convert to and from this space: a known
    /// primaries/transfer pair, gamma > 0 for [`TransferFunction::Gamma`],
    /// and sane chromaticities for [`ColorPrimaries::Custom`].
    pub fn is_valid(&self) -> bool {
        unsafe { sys::optics_gfx_color_space_is_valid(self.raw) }
    }

    /// The 3×3 matrix converting linear-light RGB in `self` to
    /// linear-light RGB in `to` (primaries only — transfer functions are
    /// applied separately). Bradford-adapts when the white points differ.
    /// `None` when either space is invalid.
    pub fn transform_matrix(&self, to: ColorSpace) -> Option<Mat3> {
        let mut raw = sys::optics_gfx_mat3::default();
        let ok =
            unsafe { sys::optics_gfx_color_space_transform_matrix(self.raw, to.raw, &mut raw) };
        ok.then_some(Mat3::from_raw(raw))
    }

    /// Encode a linear-light value with this space's transfer function
    /// and gamma.
    pub fn encode(&self, linear: f32) -> f32 {
        self.transfer().encode(self.raw.gamma, linear)
    }

    /// Decode an encoded value back to linear light; the exact inverse of
    /// [`ColorSpace::encode`].
    pub fn decode(&self, encoded: f32) -> f32 {
        self.transfer().decode(self.raw.gamma, encoded)
    }

    /// The raw C value, for desc assembly.
    pub fn as_raw(&self) -> sys::optics_gfx_color_space {
        self.raw
    }

    /// Wrap a raw C value (e.g. from [`SurfaceInfo`]) without validation.
    pub fn from_raw(raw: sys::optics_gfx_color_space) -> Self {
        Self { raw }
    }
}

impl PartialEq for ColorSpace {
    fn eq(&self, other: &Self) -> bool {
        unsafe { sys::optics_gfx_color_space_equal(self.raw, other.raw) }
    }
}

impl fmt::Debug for ColorSpace {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("ColorSpace")
            .field("primaries", &self.primaries())
            .field("transfer", &self.transfer())
            .field("gamma", &self.raw.gamma)
            .finish_non_exhaustive()
    }
}

/// Column-major 3×3 matrix for linear-light color-space transforms
/// (mirrors `optics_gfx_mat3`; element layout is `m[col * 3 + row]`).
#[derive(Debug, Clone, Copy)]
pub struct Mat3 {
    raw: sys::optics_gfx_mat3,
}

impl Mat3 {
    /// The 3x3 identity matrix.
    pub fn identity() -> Mat3 {
        Mat3 {
            raw: unsafe { sys::optics_gfx_mat3_identity() },
        }
    }

    /// `self · rhs` — the product applies `rhs` first.
    pub fn multiply(self, rhs: Mat3) -> Mat3 {
        Mat3 {
            raw: unsafe { sys::optics_gfx_mat3_multiply(self.raw, rhs.raw) },
        }
    }

    /// Apply the matrix to a column vector.
    pub fn transform_vec3(self, v: [f32; 3]) -> [f32; 3] {
        let out = unsafe { sys::optics_gfx_mat3_transform_vec3(self.raw, vec3(v)) };
        [out.x, out.y, out.z]
    }

    /// The inverse, or the identity when the matrix is singular.
    pub fn invert(self) -> Mat3 {
        Mat3 {
            raw: unsafe { sys::optics_gfx_mat3_invert(self.raw) },
        }
    }

    /// Column-major elements (`m[col * 3 + row]`).
    pub fn as_array(&self) -> [f32; 9] {
        self.raw.m
    }

    /// Adopt a raw C-side matrix value.
    pub fn from_raw(raw: sys::optics_gfx_mat3) -> Mat3 {
        Mat3 { raw }
    }

    /// The C-side matrix value (for FFI edges).
    pub fn as_raw(&self) -> sys::optics_gfx_mat3 {
        self.raw
    }
}

impl PartialEq for Mat3 {
    /// Exact element-wise equality (the C side defines no epsilon
    /// comparison for matrices).
    fn eq(&self, other: &Self) -> bool {
        self.raw.m == other.raw.m
    }
}

/// A parsed ICC v2/v4 display/scanner-class RGB profile (ADR-0070).
/// Parsing implements the bounded subset gfx consumes — matrix + TRC
/// profiles (curveType gamma, parametricCurveType 0–4) and A2B0 LUT
/// profiles (mft1/mft2/mAB) — and never touches the GPU.
///
/// Refcounted in C; this handle owns one reference, [`Clone`] retains.
pub struct IccProfile {
    raw: *mut sys::optics_gfx_icc_profile,
}

impl IccProfile {
    /// Parse and validate `data` as an ICC profile.
    pub fn new(data: &[u8]) -> Result<IccProfile, Error> {
        let mut out: *mut sys::optics_gfx_icc_profile = std::ptr::null_mut();
        Error::check(unsafe {
            sys::optics_gfx_icc_profile_create(
                data.as_ptr() as *const std::os::raw::c_void,
                data.len(),
                &mut out,
            )
        })?;
        debug_assert!(!out.is_null());
        Ok(IccProfile { raw: out })
    }

    /// The parametric color space when the profile is exactly
    /// representable in the `{primaries, transfer}` model (matrix + a
    /// transfer curve from the gfx set). LUT-only profiles return
    /// `None`; they are still usable on images, where they are baked into
    /// a 3D LUT at creation (see [`ImageColorSpace`]).
    pub fn color_space(&self) -> Option<ColorSpace> {
        let mut raw = sys::optics_gfx_color_space {
            primaries: sys::optics_gfx_color_primaries::OPTICS_GFX_PRIMARIES_BT709,
            transfer: sys::optics_gfx_transfer_func::OPTICS_GFX_TRANSFER_LINEAR,
            gamma: 0.0,
            xy: ZERO_XY,
        };
        let ok = unsafe { sys::optics_gfx_icc_profile_color_space(self.raw, &mut raw) };
        ok.then_some(ColorSpace::from_raw(raw))
    }

    /// Raw `optics_gfx_icc_profile` pointer. Borrowed; the `IccProfile` retains
    /// ownership. For desc assembly in sibling binding crates.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_icc_profile {
        self.raw
    }
}

impl Clone for IccProfile {
    fn clone(&self) -> Self {
        // SAFETY: retain returns the same pointer with the refcount
        // incremented; the new handle owns that reference.
        IccProfile {
            raw: unsafe { sys::optics_gfx_icc_profile_retain(self.raw) },
        }
    }
}

impl Drop for IccProfile {
    fn drop(&mut self) {
        // SAFETY: we own one reference taken at create/clone.
        unsafe { sys::optics_gfx_icc_profile_release(self.raw) };
    }
}

// SAFETY: optics_gfx_icc_profile is immutable after creation and refcounted atomically.
unsafe impl Send for IccProfile {}
unsafe impl Sync for IccProfile {}

impl fmt::Debug for IccProfile {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("IccProfile")
            .field("raw", &self.raw)
            .finish()
    }
}
