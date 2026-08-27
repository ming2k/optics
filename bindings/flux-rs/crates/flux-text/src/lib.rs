//! Text rendering on flux — safe Rust bindings to the **flux-text** sibling.
//!
//! Wraps the raw FFI in [`flux_text_sys`] with an RAII context and
//! Rust-native types. Callers create a [`Text`] context from a
//! [`flux::Device`], then [`measure`](Text::measure) or
//! [`draw`](Text::draw) UTF-8 runs through a [`flux::Canvas`] using a
//! per-frame [`flux::Arena`]. Caret/selection mapping mirrors the C Layer-0
//! API and is BiDi-correct.
//!
//! This crate mirrors only the engine's Layer-0 surface (single-run
//! shaping), matching the "API layers" note in `<flux-text/text.h>`. Line
//! wrapping / paragraph composition is Layer-1 and lives in the separate
//! `flux-text-layout` crate.
//!
//! flux core handles (`Device`, `Canvas`, `Arena`) come from the `flux`
//! crate; their raw pointers are ABI-identical to the opaque types
//! `flux_text_sys` declares, and are cast at the call seam.
//!
//! Per ADR-0016, flux-text is a sibling to libflux: it links HarfBuzz and
//! feeds `flux_canvas_draw_glyph_run`. libflux itself stays Vulkan-only.

#![deny(rust_2018_idioms)]

use std::fmt;

use flux::{Arena, Canvas, Device};

/// A flux result code surfaced as a Rust error. This is the SAME type as
/// [`flux::Error`] (the whole stack speaks one `flux_result`); re-exported
/// here so `flux_text` users need one fewer import.
pub use flux::Error;

pub(crate) fn check(rc: flux_text_sys::flux_result) -> Result<(), Error> {
    Error::check_raw(rc)
}

/// How a run looks: size, weight, family, and (premultiplied) colour.
/// `weight == 0.0` selects the regular weight; `color` is ignored by
/// measure/caret calls. `family == FLUX_TEXT_FAMILY_DEFAULT` (the zero
/// value) defers to the context's default family
/// ([`Text::set_default_family`]).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
    pub size_px: f32,
    pub weight: f32,
    pub color: u32,
    pub family: flux_text_sys::flux_text_family,
    pub italic: bool,
}

/// Typeface family, re-exported from the raw bindings.
/// `Family::FLUX_TEXT_FAMILY_DEFAULT` means "use the context default".
pub use flux_text_sys::flux_text_family as Family;

impl Style {
    /// A run at `size_px` in the given packed colour, regular weight, using
    /// the context's default family.
    pub fn new(size_px: f32, color: u32) -> Self {
        Self {
            size_px,
            weight: 0.0,
            color,
            family: flux_text_sys::flux_text_family::FLUX_TEXT_FAMILY_DEFAULT,
            italic: false,
        }
    }

    /// Set the family (builder).
    pub fn with_family(mut self, family: Family) -> Self {
        self.family = family;
        self
    }

    /// Set italic (builder).
    pub fn with_italic(mut self, italic: bool) -> Self {
        self.italic = italic;
        self
    }

    /// Set weight (builder).
    pub fn with_weight(mut self, weight: f32) -> Self {
        self.weight = weight;
        self
    }

    fn to_sys(self) -> flux_text_sys::flux_text_style {
        flux_text_sys::flux_text_style {
            size_px: self.size_px,
            weight: self.weight,
            color: self.color,
            family: self.family,
            italic: self.italic,
        }
    }
}

/// Shaped extent of a run, in logical pixels. `baseline` is measured from the
/// top of the run.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Metrics {
    pub width: f32,
    pub height: f32,
    pub baseline: f32,
}

impl From<flux_text_sys::flux_text_metrics> for Metrics {
    fn from(m: flux_text_sys::flux_text_metrics) -> Self {
        Self {
            width: m.width,
            height: m.height,
            baseline: m.baseline,
        }
    }
}

/// A horizontal span `[x0, x1)` in logical pixels — one per visual line of a
/// selected byte range.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct XRange {
    pub x0: f32,
    pub x1: f32,
}

/// The shaping / layout / atlas context. One per device; thread-affine.
pub struct Text {
    raw: *mut flux_text_sys::flux_text,
}

impl Text {
    /// Create a context that uploads its glyph atlas through `device` (so it
    /// can both measure and draw).
    pub fn new(device: &Device) -> Result<Text, Error> {
        Self::create(device.as_raw() as *mut flux_text_sys::flux_device, 1.0)
    }

    /// Create a measure-only context (no device, no atlas uploads). Useful for
    /// layout passes off the render thread.
    pub fn measure_only() -> Result<Text, Error> {
        Self::create(std::ptr::null_mut(), 1.0)
    }

    fn create(device: *mut flux_text_sys::flux_device, scale: f32) -> Result<Text, Error> {
        let desc = flux_text_sys::flux_text_desc { device, scale };
        let mut out: *mut flux_text_sys::flux_text = std::ptr::null_mut();
        check(unsafe { flux_text_sys::flux_text_create(&desc, &mut out) })?;
        debug_assert!(!out.is_null());
        Ok(Text { raw: out })
    }

    /// Set the device-pixel scale used to rasterise glyphs (default 1.0). Set
    /// once per frame when the surface scale changes.
    pub fn set_scale(&self, scale: f32) {
        unsafe { flux_text_sys::flux_text_set_scale(self.raw, scale) };
    }

    /// The current device-pixel scale.
    pub fn scale(&self) -> f32 {
        unsafe { flux_text_sys::flux_text_scale(self.raw) }
    }

    /// The family a style with [`Family::FLUX_TEXT_FAMILY_DEFAULT`] resolves
    /// to. Sans-serif at creation.
    pub fn default_family(&self) -> Family {
        unsafe { flux_text_sys::flux_text_default_family(self.raw) }
    }

    /// Set the family that [`Family::FLUX_TEXT_FAMILY_DEFAULT`] resolves to.
    /// The family's faces load lazily on first use. Cheap to call every
    /// frame; the engine only queries fontconfig when a new family is first
    /// shaped.
    pub fn set_default_family(&self, family: Family) {
        unsafe { flux_text_sys::flux_text_set_default_family(self.raw, family) };
    }

    /// Release the per-context scratch high-water marks. After a one-off
    /// megabyte paste (or whenever the host goes idle), call this so the
    /// engine does not hold on to the peak layout / run allocations
    /// indefinitely. The next measure / draw / caret call reallocates to
    /// whatever size it needs. Cheap to call every frame.
    pub fn compact(&self) {
        unsafe { flux_text_sys::flux_text_compact(self.raw) };
    }

    /// Shape `text` in `style` and report its extent.
    pub fn measure(&self, text: &str, style: &Style) -> Metrics {
        let s = style.to_sys();
        unsafe {
            flux_text_sys::flux_text_measure(self.raw, text.as_ptr() as *const i8, text.len(), &s)
        }
        .into()
    }

    /// Shape `text` and paint it as one batched glyph run with its top-left at
    /// `(x, y)` (logical pixels). `arena` supplies per-frame scratch for the
    /// glyph quads. No-op on a measure-only context.
    pub fn draw(&self, canvas: &Canvas, arena: &Arena, x: f32, y: f32, text: &str, style: &Style) {
        let s = style.to_sys();
        unsafe {
            flux_text_sys::flux_text_draw(
                self.raw,
                canvas.as_raw() as *mut flux_text_sys::flux_canvas,
                arena.as_raw() as *mut flux_text_sys::flux_arena,
                x,
                y,
                text.as_ptr() as *const i8,
                text.len(),
                &s,
            )
        };
    }

    /// Logical x of the glyph boundary before byte offset `byte` (BiDi-correct;
    /// not the same as the width of the prefix substring).
    pub fn x_for_byte(&self, text: &str, byte: usize, style: &Style) -> f32 {
        let s = style.to_sys();
        unsafe {
            flux_text_sys::flux_text_x_for_byte(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                byte,
                &s,
            )
        }
    }

    /// Source byte offset of the glyph boundary nearest logical x `local_x`.
    pub fn byte_for_x(&self, text: &str, local_x: f32, style: &Style) -> usize {
        let s = style.to_sys();
        unsafe {
            flux_text_sys::flux_text_byte_for_x(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                local_x,
                &s,
            )
        }
    }

    /// On-screen spans covering byte range `[lo, hi)`, one per visual line.
    pub fn selection_rects(&self, text: &str, lo: usize, hi: usize, style: &Style) -> Vec<XRange> {
        let s = style.to_sys();
        let mut buf = [flux_text_sys::flux_text_xrange { x0: 0.0, x1: 0.0 }; 16];
        let n = unsafe {
            flux_text_sys::flux_text_selection_rects(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                lo,
                hi,
                &s,
                buf.as_mut_ptr(),
                buf.len() as i32,
            )
        };
        buf.iter()
            .take(n.max(0) as usize)
            .map(|r| XRange { x0: r.x0, x1: r.x1 })
            .collect()
    }

    /// Move the caret one glyph in visual order (`forward` = rightward on
    /// screen) and return the resulting source byte offset.
    pub fn visual_move(&self, text: &str, byte: usize, forward: bool, style: &Style) -> usize {
        let s = style.to_sys();
        unsafe {
            flux_text_sys::flux_text_visual_move(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                byte,
                forward,
                &s,
            )
        }
    }

    pub fn as_raw(&self) -> *mut flux_text_sys::flux_text {
        self.raw
    }
}

impl Drop for Text {
    fn drop(&mut self) {
        unsafe { flux_text_sys::flux_text_destroy(self.raw) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Build a measure-only context for engine-dependent tests. Returns
    /// `None` if the engine cannot be initialised (e.g. no fontconfig in
    /// CI), in which case engine-dependent tests skip themselves.
    fn engine() -> Option<Text> {
        Text::measure_only().ok()
    }

    fn style() -> Style {
        Style::new(17.0, 0xFFFFFFFF)
    }

    #[test]
    fn compact_releases_and_keeps_working() {
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "the quick brown fox";
        let before = engine.measure(text, &st);
        engine.compact();
        let _x = engine.x_for_byte(text, 5, &st);
        let _x = engine.x_for_byte(text, 6, &st);
        engine.compact();
        let after = engine.measure(text, &st);
        assert_eq!(before, after, "compact must not change measure output");
    }

    #[test]
    fn caret_calls_reuse_cached_layout() {
        let Some(engine) = engine() else { return };
        let st = style();
        let text = "hello world";
        let x5 = engine.x_for_byte(text, 5, &st);
        let x6 = engine.x_for_byte(text, 6, &st);
        let x5_again = engine.x_for_byte(text, 5, &st);
        assert!(x5 <= x6, "caret x must increase with byte for LTR");
        assert_eq!(x5, x5_again, "cache hit must return identical x");
    }
}
