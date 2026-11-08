//! Safe Rust bindings for **svg** — the Optics SVG vector-document compiler
//! (ADR-0150).
//!
//! One parse, many paints. [`Document::parse`] turns SVG static vector art into
//! an immutable, refcounted document; the document can then be painted at any
//! size, any number of times, from any thread, with [`Document::draw`] or
//! rasterized to pixels with [`Document::rasterize`].
//!
//! Two properties shape the API:
//!
//! - **Paint is a value, not a mode of the document.** [`PaintMode::theme`]
//!   paints the document faithfully; [`PaintMode::monochrome`] replaces every
//!   painted shape's colour with the caller's while keeping coverage, opacity and
//!   geometry. That is what an icon tint or a cursor silhouette needs, and it is
//!   a flag rather than a reparse.
//! - **Documents are device-less.** A parsed document outlives every GPU device
//!   and is released by its owner, so it can be cached across surface
//!   re-creation and shared between the render thread and a worker.

#![deny(rust_2018_idioms)]

pub use svg_sys as sys;

use std::ffi::{CStr, CString};
use std::marker::PhantomData;

/// Library version string from the linked library.
pub fn version() -> &'static str {
    // SAFETY: the C library returns a pointer to a static NUL-terminated string.
    unsafe { CStr::from_ptr(sys::svg_version_string()) }
        .to_str()
        .unwrap_or("<invalid>")
}

/// Packed integer version of the linked library.
pub fn version_number() -> u32 {
    unsafe { sys::svg_version_number() }
}

/// True iff the linked library satisfies the major.minor.patch floor.
pub fn version_check(major: u32, minor: u32, patch: u32) -> bool {
    unsafe { sys::svg_version_check(major as i32, minor as i32, patch as i32) }
}

/// Live document count for the whole process.
///
/// This is the drain counter for the `svg` resource kind (ADR-0130): read it
/// before and after a batch of parses to prove the release path balances. A
/// document that is still counted after its last owner dropped is a leak, and
/// this is the only way to observe it from Rust.
pub fn live_documents() -> u32 {
    unsafe { sys::svg_live_documents() }
}

/// Why a document could not be parsed, or a draw could not be recorded.
///
/// `svg_parse` is total: it either returns a whole document or an error. There
/// is deliberately no "partially parsed" state — half an icon renders plausibly,
/// and a silent fraction of the art is worse than a refusal.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// The markup is malformed, a ceiling in [`Limits`] was exceeded, or the
    /// input ended mid-subtree.
    InvalidArgument,
    /// An allocation failed.
    OutOfMemory,
    /// A draw or rasterize request named an unsupported parameter.
    OutOfRange,
    /// The library returned a code this binding does not know (a newer library
    /// than the bindings). Treated as an error, never as success.
    Unsupported,
    /// Any other flux error code.
    Backend(i32),
}

impl Error {
    fn from_raw(raw: sys::flux_result) -> Option<Self> {
        use sys::flux_result::*;
        match raw {
            FLUX_OK => None,
            FLUX_ERROR_INVALID_ARGUMENT => Some(Error::InvalidArgument),
            FLUX_ERROR_OUT_OF_MEMORY => Some(Error::OutOfMemory),
            FLUX_ERROR_OUT_OF_RANGE => Some(Error::OutOfRange),
            FLUX_ERROR_UNSUPPORTED => Some(Error::Unsupported),
            other => Some(Error::Backend(other as i32)),
        }
    }

    fn check(raw: sys::flux_result) -> Result<(), Error> {
        match Self::from_raw(raw) {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::InvalidArgument => write!(f, "malformed document or exceeded parse limit"),
            Error::OutOfMemory => write!(f, "allocation failed"),
            Error::OutOfRange => write!(f, "parameter out of range"),
            Error::Unsupported => write!(f, "unsupported by the linked library"),
            Error::Backend(code) => write!(f, "library error {code}"),
        }
    }
}

impl std::error::Error for Error {}

/// Resource ceilings for one parse. Every field is an allocation bound, not a
/// policy knob: a document from a theme directory or a download must not be able
/// to make the parser allocate without limit. `None` selects the library
/// defaults (4096 shapes, 65536 commands, 256 gradients, 16384 elements, depth
/// 32).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Limits {
    pub max_shapes: Option<u32>,
    pub max_commands: Option<u32>,
    pub max_gradients: Option<u32>,
    pub max_elements: Option<u32>,
    pub max_depth: Option<u32>,
}

impl Limits {
    fn raw(self) -> sys::svg_parse_limits {
        sys::svg_parse_limits {
            max_shapes: self.max_shapes.unwrap_or(0),
            max_commands: self.max_commands.unwrap_or(0),
            max_gradients: self.max_gradients.unwrap_or(0),
            max_elements: self.max_elements.unwrap_or(0),
            max_depth: self.max_depth.unwrap_or(0),
        }
    }
}

/// An immutable parsed document. Refcounted; cheap to clone and safe to share
/// across threads.
///
/// Dropping the last clone releases the document, which is what
/// [`live_documents`] observes.
pub struct Document {
    raw: *mut sys::svg_document,
}

// SAFETY: the underlying document is immutable after parsing and its refcount is
// atomic, which is exactly the contract the C header documents. Painting a
// document does not mutate it, so sharing one across threads is sound.
unsafe impl Send for Document {}
unsafe impl Sync for Document {}

impl Document {
    /// Parse `text` into a document.
    ///
    /// `text` is UTF-8 SVG source; it may contain interior NULs only if you pass
    /// it by length, which this binding does (see [`Document::parse_bytes`]).
    pub fn parse(text: &str) -> Result<Self, Error> {
        Self::parse_bytes(text.as_bytes(), Limits::default())
    }

    /// Parse with explicit [`Limits`].
    pub fn parse_with_limits(text: &str, limits: Limits) -> Result<Self, Error> {
        Self::parse_bytes(text.as_bytes(), limits)
    }

    /// Parse a byte slice. The slice does not need to be NUL-terminated.
    pub fn parse_bytes(bytes: &[u8], limits: Limits) -> Result<Self, Error> {
        // `length` is a real byte count, and a real byte count of zero is an
        // empty document; the C entry point refuses it rather than measuring.
        // Since it once treated 0 as "measure the buffer with strlen", an empty
        // slice read past the allocation, so this guard stays as a local
        // short-circuit even though the C side no longer depends on it.
        if bytes.is_empty() {
            return Err(Error::InvalidArgument);
        }
        let raw_limits = limits.raw();
        let mut out: *mut sys::svg_document = std::ptr::null_mut();
        let opts = sys::svg_parse_opts {
            text: bytes.as_ptr().cast(),
            length: bytes.len(),
            limits: &raw_limits,
        };
        Error::check(unsafe { sys::svg_parse(&opts, &mut out) })?;
        if out.is_null() {
            // The C contract says success implies a document; a null one means
            // the library and these bindings disagree, which must not be UB.
            return Err(Error::Unsupported);
        }
        Ok(Document { raw: out })
    }

    /// The document's intrinsic size in user units. Never zero: an empty
    /// document reports 1x1 so a caller can use it as an aspect ratio
    /// unconditionally.
    pub fn viewport(&self) -> [f32; 4] {
        let rect = unsafe { sys::svg_viewport(self.raw) };
        [rect.x, rect.y, rect.w, rect.h]
    }

    /// Value of a root `<svg>` attribute (e.g. `"data-hotspot-x"`), or `None`.
    ///
    /// SVG has no vocabulary for application metadata, so carriers stamp it on
    /// `data-*` attributes and read it back through here.
    pub fn attribute(&self, name: &str) -> Option<&str> {
        let name = CString::new(name).ok()?;
        let value = unsafe { sys::svg_attribute(self.raw, name.as_ptr()) };
        if value.is_null() {
            return None;
        }
        // SAFETY: the pointer is into storage owned by the document, which
        // outlives this borrow.
        unsafe { CStr::from_ptr(value) }.to_str().ok()
    }

    /// Number of painted shapes, in document order.
    pub fn shape_count(&self) -> u32 {
        unsafe { sys::svg_shape_count(self.raw) }
    }

    /// Borrow shape `index`, or `None` if the index is out of range.
    pub fn shape(&self, index: u32) -> Option<Shape<'_>> {
        let mut raw: sys::svg_shape_view = unsafe { std::mem::zeroed() };
        match Error::check(unsafe { sys::svg_shape_at(self.raw, index, &mut raw) }) {
            Ok(()) => Some(Shape {
                raw,
                _document: PhantomData,
            }),
            Err(_) => None,
        }
    }

    /// Iterate the document's shapes in order.
    pub fn shapes(&self) -> impl Iterator<Item = Shape<'_>> + '_ {
        (0..self.shape_count()).filter_map(move |index| self.shape(index))
    }

    /// Resolve gradient `index` (a [`Paint::gradient`] value).
    pub fn gradient(&self, index: u32) -> Option<Gradient> {
        let mut raw: sys::svg_gradient_view = unsafe { std::mem::zeroed() };
        Error::check(unsafe { sys::svg_gradient_at(self.raw, index, &mut raw) }).ok()?;
        Some(Gradient { raw })
    }

    /// Paint the document onto a canvas or an encoder.
    pub fn draw(&self, sink: Sink<'_>, options: DrawOptions) -> Result<(), Error> {
        let (canvas, encoder) = match sink {
            Sink::Canvas(commands) => (commands.as_raw(), std::ptr::null_mut()),
            Sink::Encoder(encoder) => (std::ptr::null_mut(), encoder.as_raw()),
        };
        let raw = sys::svg_draw_opts {
            document: self.raw,
            canvas,
            encoder,
            bounds: sys::flux_rect {
                x: options.bounds[0],
                y: options.bounds[1],
                w: options.bounds[2],
                h: options.bounds[3],
            },
            fit: options.fit.raw(),
            paint: options.paint.raw(),
            color: options.paint.color(),
            opacity: options.opacity.unwrap_or(0.0),
            stroke_width: options.stroke_width,
        };
        Error::check(unsafe { sys::svg_draw(&raw) })
    }

    /// Rasterize the document headlessly, using flux's device-less software
    /// canvas. No GPU, device or window is involved, so this is safe on a worker
    /// thread and in CI.
    pub fn rasterize(&self, options: RasterizeOptions) -> Result<Pixmap, Error> {
        let mut out: sys::svg_pixmap = unsafe { std::mem::zeroed() };
        let raw = sys::svg_rasterize_opts {
            document: self.raw,
            width: options.width,
            height: options.height,
            supersample: options.supersample,
            format: options.format.raw(),
            paint: options.paint.raw(),
            color: options.paint.color(),
            stroke_width: options.stroke_width,
        };
        Error::check(unsafe { sys::svg_rasterize(&raw, &mut out) })?;
        Ok(Pixmap {
            width: out.width,
            height: out.height,
            stride: out.stride,
            format: options.format,
            pixels: out.pixels,
            len: out.stride as usize * out.height as usize,
        })
    }

    /// Borrow the raw document for unsafe integration.
    pub fn as_raw(&self) -> *mut sys::svg_document {
        self.raw
    }
}

impl Clone for Document {
    fn clone(&self) -> Self {
        // SAFETY: retaining returns the same pointer (or null only for a null
        // input, which this type never holds).
        let raw = unsafe { sys::svg_retain(self.raw) };
        Document { raw }
    }
}

impl Drop for Document {
    fn drop(&mut self) {
        unsafe { sys::svg_release(self.raw) };
    }
}

impl std::fmt::Debug for Document {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Document")
            .field("viewport", &self.viewport())
            .field("shapes", &self.shape_count())
            .finish()
    }
}

/// One painted shape, borrowed from its document.
#[derive(Debug, Clone, Copy)]
pub struct Shape<'doc> {
    raw: sys::svg_shape_view,
    _document: PhantomData<&'doc Document>,
}

impl Shape<'_> {
    /// The shape's path commands, already in document user space.
    pub fn commands(&self) -> &[sys::svg_path_cmd] {
        if self.raw.cmds.is_null() || self.raw.cmd_count == 0 {
            return &[];
        }
        // SAFETY: the document owns `cmd_count` contiguous commands at `cmds`.
        unsafe { std::slice::from_raw_parts(self.raw.cmds, self.raw.cmd_count as usize) }
    }

    pub fn fill(&self) -> Paint {
        Paint::from_raw(self.raw.fill)
    }

    pub fn stroke(&self) -> Paint {
        Paint::from_raw(self.raw.stroke)
    }

    /// Stroke width in user units; zero when the shape is fill-only.
    pub fn stroke_width(&self) -> f32 {
        self.raw.stroke_width
    }

    pub fn miter_limit(&self) -> f32 {
        self.raw.miter_limit
    }

    pub fn cap(&self) -> sys::flux_line_cap {
        self.raw.cap
    }

    pub fn join(&self) -> sys::flux_line_join {
        self.raw.join
    }

    pub fn fill_rule(&self) -> sys::flux_fill_rule {
        self.raw.fill_rule
    }

    /// Cascaded element opacity (this element's `opacity` times every ancestor
    /// group's), separate from the paints' own opacity so recolouring keeps the
    /// shape's alpha.
    pub fn opacity(&self) -> f32 {
        self.raw.opacity
    }

    /// Geometric bounds in user space, stroke excluded.
    pub fn bounds(&self) -> [f32; 4] {
        let b = self.raw.bounds;
        [b.x, b.y, b.w, b.h]
    }
}

/// A resolved paint on a shape.
#[derive(Debug, Clone, Copy)]
pub struct Paint {
    raw: sys::svg_paint,
}

impl Paint {
    fn from_raw(raw: sys::svg_paint) -> Self {
        Paint { raw }
    }

    pub fn kind(&self) -> sys::svg_paint_kind {
        self.raw.kind
    }

    /// True when this half of the shape is not painted at all (`none`).
    pub fn is_none(&self) -> bool {
        matches!(self.raw.kind, sys::svg_paint_kind::SVG_PAINT_NONE)
    }

    /// True when the document said `currentColor`, i.e. the caller's colour.
    pub fn is_current_color(&self) -> bool {
        matches!(self.raw.kind, sys::svg_paint_kind::SVG_PAINT_CURRENT_COLOR)
    }

    /// Literal colour as straight (non-premultiplied) `0xAARRGGBB`, valid for
    /// [`Self::kind`]` == SVG_PAINT_SOLID`.
    pub fn color(&self) -> u32 {
        self.raw.color
    }

    /// The paint's own alpha multiplier (`fill-opacity` / `stroke-opacity`).
    pub fn opacity(&self) -> f32 {
        self.raw.opacity
    }

    /// Gradient index for a gradient paint, resolved through
    /// [`Document::gradient`].
    pub fn gradient(&self) -> Option<u32> {
        match self.raw.kind {
            sys::svg_paint_kind::SVG_PAINT_LINEAR_GRADIENT
            | sys::svg_paint_kind::SVG_PAINT_RADIAL_GRADIENT => Some(self.raw.gradient),
            _ => None,
        }
    }
}

/// A gradient resolved in document user space, with `objectBoundingBox` units
/// already baked against the shape's bounds.
#[derive(Debug, Clone)]
pub struct Gradient {
    raw: sys::svg_gradient_view,
}

impl Gradient {
    pub fn stops(&self) -> &[sys::flux_gradient_stop] {
        let count = (self.raw.stop_count as usize).min(self.raw.stops.len());
        &self.raw.stops[..count]
    }

    /// Linear: the first endpoint. Radial: the centre.
    pub fn start(&self) -> [f32; 2] {
        [self.raw.start.x, self.raw.start.y]
    }

    /// Linear: the second endpoint. Radial: unused (equals `start`).
    pub fn end(&self) -> [f32; 2] {
        [self.raw.end.x, self.raw.end.y]
    }

    /// Radial radius; zero for a linear gradient.
    pub fn radius(&self) -> f32 {
        self.raw.radius
    }
}

/// Where a document is painted.
///
/// A canvas is only recordable while a pass is open, and the safe flux API hands
/// out [`flux::CanvasCommands`] — never the `Canvas` itself — for exactly that
/// window. Taking `&CanvasCommands` therefore makes the borrow checker enforce
/// the pass lifetime, instead of leaving the caller to promise it.
pub enum Sink<'a> {
    /// Record into the live pass these commands belong to.
    Canvas(&'a flux::CanvasCommands<'a>),
    /// Record into an immutable display list.
    Encoder(&'a flux::Encoder),
}

/// How a document is fitted into the destination box.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Fit {
    /// Uniform scale, centred, aspect preserved.
    #[default]
    Contain,
    /// Independent x/y scale onto the box.
    Stretch,
}

impl Fit {
    fn raw(self) -> sys::svg_fit {
        match self {
            Fit::Contain => sys::svg_fit::SVG_FIT_CONTAIN,
            Fit::Stretch => sys::svg_fit::SVG_FIT_STRETCH,
        }
    }
}

/// How paints are chosen.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PaintMode {
    /// Paint the document faithfully; `currentColor` resolves to the theme
    /// colour. This is the default because it is the least surprising: what the
    /// document says is what appears.
    Theme { color: u32 },
    /// Draw every painted shape in `color`, keeping per-shape coverage, opacity
    /// and geometry. The monochrome mask bake: an icon tint, a cursor
    /// silhouette, a coverage mask.
    Monochrome { color: u32 },
}

impl PaintMode {
    /// Faithful painting, resolving `currentColor` to `color`.
    pub fn theme(color: u32) -> Self {
        PaintMode::Theme { color }
    }

    /// Single-colour painting.
    pub fn monochrome(color: u32) -> Self {
        PaintMode::Monochrome { color }
    }

    fn raw(self) -> sys::svg_paint_mode {
        match self {
            PaintMode::Theme { .. } => sys::svg_paint_mode::SVG_PAINT_THEME,
            PaintMode::Monochrome { .. } => sys::svg_paint_mode::SVG_PAINT_MONOCHROME,
        }
    }

    fn color(self) -> u32 {
        match self {
            PaintMode::Theme { color } | PaintMode::Monochrome { color } => color,
        }
    }
}

impl Default for PaintMode {
    fn default() -> Self {
        PaintMode::Theme { color: 0 }
    }
}

/// Options for [`Document::draw`].
#[derive(Debug, Clone, Copy)]
pub struct DrawOptions {
    /// Destination box `[x, y, width, height]` in the target's current
    /// coordinate space. Width and height must be positive.
    pub bounds: [f32; 4],
    pub fit: Fit,
    pub paint: PaintMode,
    /// Global alpha multiplier in `(0, 1]`; `None` means 1.0.
    pub opacity: Option<f32>,
    /// Non-zero replaces every stroke width with this value in destination
    /// units — the theme-weight pin that keeps an icon set visually
    /// interchangeable. Zero keeps the document's own widths.
    pub stroke_width: f32,
}

impl Default for DrawOptions {
    fn default() -> Self {
        Self {
            bounds: [0.0, 0.0, 1.0, 1.0],
            fit: Fit::Contain,
            paint: PaintMode::default(),
            opacity: None,
            stroke_width: 0.0,
        }
    }
}

/// Pixel layout of a [`Pixmap`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PixelFormat {
    /// Premultiplied RGBA8, row-major — flux's native CPU canvas layout.
    Rgba8Premul,
    /// Premultiplied BGRA8 — the layout DRM/KMS cursor planes and
    /// `B8G8R8A8_UNORM` sampling read. Produced by channel swap, not by a second
    /// rasterization.
    Bgra8Premul,
    /// One 8-bit coverage byte per pixel — the mask-bake readback.
    Coverage,
}

impl PixelFormat {
    fn raw(self) -> sys::svg_pixel_format {
        match self {
            PixelFormat::Rgba8Premul => sys::svg_pixel_format::SVG_PIXELS_RGBA8_PREMUL,
            PixelFormat::Bgra8Premul => sys::svg_pixel_format::SVG_PIXELS_BGRA8_PREMUL,
            PixelFormat::Coverage => sys::svg_pixel_format::SVG_PIXELS_COVERAGE,
        }
    }
}

/// Options for [`Document::rasterize`].
#[derive(Debug, Clone, Copy)]
pub struct RasterizeOptions {
    pub width: u32,
    pub height: u32,
    /// Samples per axis for the software rasterizer: 0 selects the backend
    /// default (2 → 4 samples/pixel), 1 is aliased, up to 8.
    pub supersample: u32,
    pub format: PixelFormat,
    pub paint: PaintMode,
    pub stroke_width: f32,
}

impl RasterizeOptions {
    /// A premultiplied-RGBA8 rasterization of `width`x`height` at the backend's
    /// default antialiasing.
    pub fn new(width: u32, height: u32) -> Self {
        Self {
            width,
            height,
            supersample: 0,
            format: PixelFormat::Rgba8Premul,
            paint: PaintMode::default(),
            stroke_width: 0.0,
        }
    }
}

/// Rasterized pixels owned by Rust. Released on drop, including on the error
/// paths of the call that produced it.
pub struct Pixmap {
    width: u32,
    height: u32,
    stride: u32,
    format: PixelFormat,
    pixels: *mut u8,
    len: usize,
}

impl Pixmap {
    pub fn width(&self) -> u32 {
        self.width
    }

    pub fn height(&self) -> u32 {
        self.height
    }

    /// Bytes per row.
    pub fn stride(&self) -> u32 {
        self.stride
    }

    pub fn format(&self) -> PixelFormat {
        self.format
    }

    /// The pixels as a byte slice: `stride * height` bytes.
    pub fn as_bytes(&self) -> &[u8] {
        if self.pixels.is_null() || self.len == 0 {
            return &[];
        }
        // SAFETY: the C library allocated `len` bytes and hands ownership over.
        unsafe { std::slice::from_raw_parts(self.pixels, self.len) }
    }

    /// The pixels, mutably — for a caller that wants to post-process in place.
    pub fn as_bytes_mut(&mut self) -> &mut [u8] {
        if self.pixels.is_null() || self.len == 0 {
            return &mut [];
        }
        // SAFETY: unique borrow of a buffer this value owns.
        unsafe { std::slice::from_raw_parts_mut(self.pixels, self.len) }
    }
}

impl Drop for Pixmap {
    fn drop(&mut self) {
        if self.pixels.is_null() {
            return;
        }
        // Release through the library so its allocator owns the free, exactly as
        // it owned the allocation.
        let mut raw = sys::svg_pixmap {
            width: self.width,
            height: self.height,
            stride: self.stride,
            format: self.format.raw(),
            pixels: self.pixels,
        };
        unsafe { sys::svg_pixmap_deinit(&mut raw) };
        self.pixels = std::ptr::null_mut();
    }
}

impl std::fmt::Debug for Pixmap {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Pixmap")
            .field("width", &self.width)
            .field("height", &self.height)
            .field("stride", &self.stride)
            .field("format", &self.format)
            .finish()
    }
}
