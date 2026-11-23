//! Paints, strokes, fill rules and paths.

use super::*;

// =====================================================================
//  Paint + Path
// =====================================================================

/// Fixed-function blend mode used by Canvas paints.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub enum BlendMode {
    #[default]
    SrcOver,
    Src,
    Plus,
    Multiply,
}

impl BlendMode {
    pub(crate) fn raw(self) -> sys::optics_gfx_blend_mode {
        match self {
            Self::SrcOver => sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            Self::Src => sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC,
            Self::Plus => sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_PLUS,
            Self::Multiply => sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_MULTIPLY,
        }
    }
}

/// How the open ends of a stroked subpath are rendered
/// (mirrors `optics_gfx_line_cap`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LineCap {
    /// Flat edge at the endpoint.
    #[default]
    Butt,
    /// Semicircular cap extending past the endpoint.
    Round,
    /// Square cap extending past the endpoint.
    Square,
}

impl LineCap {
    pub(crate) fn raw(self) -> sys::optics_gfx_line_cap {
        match self {
            LineCap::Butt => sys::optics_gfx_line_cap::OPTICS_GFX_CAP_BUTT,
            LineCap::Round => sys::optics_gfx_line_cap::OPTICS_GFX_CAP_ROUND,
            LineCap::Square => sys::optics_gfx_line_cap::OPTICS_GFX_CAP_SQUARE,
        }
    }
}

/// How the corners of a stroked path are rendered
/// (mirrors `optics_gfx_line_join`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum LineJoin {
    /// Sharp corner, clipped at the miter limit.
    #[default]
    Miter,
    /// Rounded corner.
    Round,
    /// Bevelled corner.
    Bevel,
}

impl LineJoin {
    pub(crate) fn raw(self) -> sys::optics_gfx_line_join {
        match self {
            LineJoin::Miter => sys::optics_gfx_line_join::OPTICS_GFX_JOIN_MITER,
            LineJoin::Round => sys::optics_gfx_line_join::OPTICS_GFX_JOIN_ROUND,
            LineJoin::Bevel => sys::optics_gfx_line_join::OPTICS_GFX_JOIN_BEVEL,
        }
    }
}

/// Surface coloring, independent of geometry and stroke topology.
#[derive(Clone, Copy)]
pub struct Brush {
    pub(crate) raw: sys::optics_gfx_brush,
}
impl Brush {
    pub fn solid(color: u32) -> Self {
        Self {
            raw: sys::optics_gfx_brush {
                kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_SOLID,
                blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
                opacity: 1.0,
                __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                    solid: sys::optics_gfx_brush_solid_data { color },
                },
            },
        }
    }
    pub fn with_blend(mut self, blend: BlendMode) -> Self {
        self.raw.blend = blend.raw();
        self
    }
    pub fn with_opacity(mut self, opacity: f32) -> Self {
        self.raw.opacity = opacity;
        self
    }
    pub fn as_raw(&self) -> &sys::optics_gfx_brush {
        &self.raw
    }
}
#[derive(Clone, Copy, Debug)]
pub struct StrokeStyle {
    pub width: f32,
    pub miter_limit: f32,
    pub cap: LineCap,
    pub join: LineJoin,
}
impl Default for StrokeStyle {
    fn default() -> Self {
        Self {
            width: 1.0,
            miter_limit: 4.0,
            cap: LineCap::Butt,
            join: LineJoin::Miter,
        }
    }
}
impl StrokeStyle {
    pub(crate) fn raw(self) -> sys::optics_gfx_stroke_style {
        sys::optics_gfx_stroke_style {
            width: self.width,
            miter_limit: self.miter_limit,
            cap: self.cap.raw(),
            join: self.join.raw(),
        }
    }
}
#[derive(Clone, Copy, Debug, Default)]
pub enum FillRule {
    #[default]
    NonZero,
    EvenOdd,
}
impl FillRule {
    pub(crate) fn raw(self) -> sys::optics_gfx_fill_rule {
        match self {
            Self::NonZero => sys::optics_gfx_fill_rule::OPTICS_GFX_FILL_NON_ZERO,
            Self::EvenOdd => sys::optics_gfx_fill_rule::OPTICS_GFX_FILL_EVEN_ODD,
        }
    }
}
/// Image modulation. Zero tint and opacity remain transparent.
#[derive(Clone, Copy, Debug)]
pub struct ImageStyle {
    pub tint: u32,
    pub blend: BlendMode,
}
impl Default for ImageStyle {
    fn default() -> Self {
        Self {
            tint: 0xFFFFFFFF,
            blend: BlendMode::SrcOver,
        }
    }
}

/// A vector path allocated from an [`Arena`] (mirrors `optics_gfx_path`).
///
/// The path borrows its arena: it is freed wholesale by [`Arena::reset`]
/// or drop, so a `Path` must not outlive the frame's arena cycle. Mutators
/// take `&self` because the path is a C-side growable buffer; overflow is
/// reported by [`Path::dropped_count`].
///
/// Paths cannot outlive their arena:
/// ```compile_fail
/// use optics_gfx::{Arena, Path};
/// let path = {
///     let arena = Arena::with_capacity(4096).unwrap();
///     Path::new(&arena).unwrap()
/// };
/// path.move_to(1.0, 2.0);
/// ```
/// Nor can an arena be reset while a path is still in use:
/// ```compile_fail
/// use optics_gfx::{Arena, Path};
/// let mut arena = Arena::with_capacity(4096).unwrap();
/// let path = Path::new(&arena).unwrap();
/// arena.reset();
/// path.move_to(1.0, 2.0);
/// ```
pub struct Path<'arena> {
    pub(crate) raw: *mut sys::optics_gfx_path,
    _arena: std::marker::PhantomData<&'arena Arena>,
}

impl<'arena> Path<'arena> {
    /// Create an empty path in `arena`.
    pub fn new(arena: &'arena Arena) -> Result<Path<'arena>, Error> {
        let mut out = std::ptr::null_mut();
        Error::check(unsafe { sys::optics_gfx_path_create(&mut out, arena.as_raw()) })?;
        Ok(Path {
            raw: out,
            _arena: std::marker::PhantomData,
        })
    }

    /// Begin a new subpath at `(x, y)`.
    pub fn move_to(&self, x: f32, y: f32) -> &Self {
        unsafe { sys::optics_gfx_path_move_to(self.raw, x, y) };
        self
    }

    /// Append a straight segment to `(x, y)`.
    pub fn line_to(&self, x: f32, y: f32) -> &Self {
        unsafe { sys::optics_gfx_path_line_to(self.raw, x, y) };
        self
    }

    /// Append a quadratic Bézier with control point `(cx, cy)`.
    pub fn quad_to(&self, cx: f32, cy: f32, x: f32, y: f32) -> &Self {
        unsafe { sys::optics_gfx_path_quad_to(self.raw, cx, cy, x, y) };
        self
    }

    /// Append a cubic Bézier with control points `(c1x, c1y)`, `(c2x, c2y)`.
    pub fn cubic_to(&self, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) -> &Self {
        unsafe { sys::optics_gfx_path_cubic_to(self.raw, c1x, c1y, c2x, c2y, x, y) };
        self
    }

    /// Close the current subpath with a straight segment to its start.
    pub fn close(&self) -> &Self {
        unsafe { sys::optics_gfx_path_close(self.raw) };
        self
    }

    /// Append an axis-aligned rectangle subpath.
    pub fn add_rect(&self, x: f32, y: f32, w: f32, h: f32) -> &Self {
        unsafe { sys::optics_gfx_path_add_rect(self.raw, sys::optics_gfx_rect { x, y, w, h }) };
        self
    }

    /// Append a rounded-rectangle subpath.
    pub fn add_round_rect(&self, x: f32, y: f32, w: f32, h: f32, radius: f32) -> &Self {
        unsafe {
            sys::optics_gfx_path_add_round_rect(
                self.raw,
                sys::optics_gfx_rect { x, y, w, h },
                radius,
            )
        };
        self
    }

    /// Append a circle subpath.
    pub fn add_circle(&self, cx: f32, cy: f32, radius: f32) -> &Self {
        unsafe { sys::optics_gfx_path_add_circle(self.raw, cx, cy, radius) };
        self
    }

    /// Segments rejected due to arena exhaustion; non-zero means draws
    /// with this path silently dropped data (grow the arena).
    pub fn dropped_count(&self) -> u32 {
        unsafe { sys::optics_gfx_path_dropped_count(self.raw) }
    }
}
