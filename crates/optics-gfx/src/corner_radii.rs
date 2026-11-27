//! Per-corner radii value type mirroring `<optics/gfx/math.h>` (`optics_gfx_corner_radii`).

use super::*;

/// Per-corner radii in clockwise order: top-left, top-right, bottom-right,
/// bottom-left (CSS `border-radius` / Skia `SkRRect` convention).
#[repr(C)]
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct CornerRadii {
    pub tl: f32,
    pub tr: f32,
    pub br: f32,
    pub bl: f32,
}

impl CornerRadii {
    /// Sharp corners (all radii zero).
    pub const ZERO: Self = Self {
        tl: 0.0,
        tr: 0.0,
        br: 0.0,
        bl: 0.0,
    };

    /// Construct from explicit `(tl, tr, br, bl)` radii in clockwise order.
    #[inline]
    pub const fn new(tl: f32, tr: f32, br: f32, bl: f32) -> Self {
        Self { tl, tr, br, bl }
    }

    /// Construct with the same radius on all four corners.
    #[inline]
    pub const fn uniform(r: f32) -> Self {
        Self {
            tl: r,
            tr: r,
            br: r,
            bl: r,
        }
    }

    /// Round only the top-left and top-right corners (bottom edge flush 90°).
    #[inline]
    pub const fn top(r: f32) -> Self {
        Self {
            tl: r,
            tr: r,
            br: 0.0,
            bl: 0.0,
        }
    }

    /// Round only the bottom-right and bottom-left corners (top edge flush 90°).
    #[inline]
    pub const fn bottom(r: f32) -> Self {
        Self {
            tl: 0.0,
            tr: 0.0,
            br: r,
            bl: r,
        }
    }

    /// Round only the top-left and bottom-left corners (right edge flush 90°).
    #[inline]
    pub const fn left(r: f32) -> Self {
        Self {
            tl: r,
            tr: 0.0,
            br: 0.0,
            bl: r,
        }
    }

    /// Round only the top-right and bottom-right corners (left edge flush 90°).
    #[inline]
    pub const fn right(r: f32) -> Self {
        Self {
            tl: 0.0,
            tr: r,
            br: r,
            bl: 0.0,
        }
    }

    /// Largest corner radius across the four corners.
    #[inline]
    pub fn max_radius(self) -> f32 {
        self.tl.max(self.tr).max(self.br).max(self.bl)
    }

    /// True when all four corners are zero or non-positive.
    #[inline]
    pub fn is_zero(self) -> bool {
        !(self.tl > 0.0 || self.tr > 0.0 || self.br > 0.0 || self.bl > 0.0)
    }

    /// CSS Backgrounds & Borders Level 3 / Skia proportional radius
    /// normalization against a `width × height` box.
    #[inline]
    pub fn normalize(self, width: f32, height: f32) -> Self {
        let raw = unsafe { sys::optics_gfx_corner_radii_normalize(self.to_raw(), width, height) };
        Self::from_raw(raw)
    }

    /// Convert to the sanitized C value type (non-finite / negative entries become `0.0`).
    #[inline]
    pub fn to_raw(self) -> sys::optics_gfx_corner_radii {
        let finite_pos = |v: f32| if v.is_finite() && v > 0.0 { v } else { 0.0 };
        sys::optics_gfx_corner_radii {
            tl: finite_pos(self.tl),
            tr: finite_pos(self.tr),
            br: finite_pos(self.br),
            bl: finite_pos(self.bl),
        }
    }

    /// Construct from the C value type.
    #[inline]
    pub const fn from_raw(raw: sys::optics_gfx_corner_radii) -> Self {
        Self {
            tl: raw.tl,
            tr: raw.tr,
            br: raw.br,
            bl: raw.bl,
        }
    }
}

impl From<f32> for CornerRadii {
    #[inline]
    fn from(r: f32) -> Self {
        Self::uniform(r)
    }
}

impl From<[f32; 4]> for CornerRadii {
    #[inline]
    fn from([tl, tr, br, bl]: [f32; 4]) -> Self {
        Self { tl, tr, br, bl }
    }
}

pub(crate) fn clamp_curvature(curvature: f32) -> f32 {
    if curvature.is_finite() {
        curvature.clamp(0.0, 1.0)
    } else {
        0.0
    }
}
