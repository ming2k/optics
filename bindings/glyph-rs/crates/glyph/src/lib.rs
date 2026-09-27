//! Safe Rust bindings for **glyph** — text shaping, layout, and dynamic glyph atlas runtime (ADR-0016, ADR-0106).

#![deny(rust_2018_idioms)]

use flux::{Arena, CanvasCommands, Device};

pub use flux::Error;
pub use glyph_sys as sys;

pub(crate) fn check(rc: glyph_sys::flux_result) -> Result<(), Error> {
    Error::check_raw(rc)
}

/// How a run looks: size, weight, family, and (premultiplied) colour.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
    pub size_px: f32,
    pub weight: f32,
    pub color: u32,
    pub family: glyph_sys::glyph_family,
    pub italic: bool,
}

pub use glyph_sys::glyph_family as Family;

impl Style {
    pub fn new(size_px: f32, color: u32) -> Self {
        Self {
            size_px,
            weight: 0.0,
            color,
            family: glyph_sys::glyph_family::GLYPH_FAMILY_DEFAULT,
            italic: false,
        }
    }

    pub fn with_family(mut self, family: Family) -> Self {
        self.family = family;
        self
    }

    pub fn with_italic(mut self, italic: bool) -> Self {
        self.italic = italic;
        self
    }

    pub fn with_weight(mut self, weight: f32) -> Self {
        self.weight = weight;
        self
    }

    fn to_sys(self) -> glyph_sys::glyph_style {
        glyph_sys::glyph_style {
            size_px: self.size_px,
            weight: self.weight,
            color: self.color,
            family: self.family,
            italic: self.italic,
        }
    }
}

/// Shaped extent of a run, in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Metrics {
    pub width: f32,
    pub height: f32,
    pub baseline: f32,
}

impl From<glyph_sys::glyph_metrics> for Metrics {
    fn from(m: glyph_sys::glyph_metrics) -> Self {
        Self {
            width: m.width,
            height: m.height,
            baseline: m.baseline,
        }
    }
}

/// A horizontal span `[x0, x1)` in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct XRange {
    pub x0: f32,
    pub x1: f32,
}

/// The shaping / layout / atlas context. One per device; thread-affine.
pub struct Text {
    raw: *mut glyph_sys::glyph_ctx,
}

impl Text {
    pub fn new(device: &Device) -> Result<Text, Error> {
        Self::create(device.as_raw(), 1.0)
    }

    pub fn measure_only() -> Result<Text, Error> {
        Self::create(std::ptr::null_mut(), 1.0)
    }

    fn create(device: *mut glyph_sys::flux_device, scale: f32) -> Result<Text, Error> {
        let desc = glyph_sys::glyph_desc { device, scale };
        let mut out: *mut glyph_sys::glyph_ctx = std::ptr::null_mut();
        check(unsafe { glyph_sys::glyph_create(&desc, &mut out) })?;
        debug_assert!(!out.is_null());
        Ok(Text { raw: out })
    }

    pub fn set_scale(&self, scale: f32) {
        unsafe { glyph_sys::glyph_set_scale(self.raw, scale) };
    }

    pub fn scale(&self) -> f32 {
        unsafe { glyph_sys::glyph_scale(self.raw) }
    }

    pub fn default_family(&self) -> Family {
        unsafe { glyph_sys::glyph_default_family(self.raw) }
    }

    pub fn set_default_family(&self, family: Family) {
        unsafe { glyph_sys::glyph_set_default_family(self.raw, family) };
    }

    pub fn compact(&self) {
        unsafe { glyph_sys::glyph_compact(self.raw) };
    }

    pub fn measure(&self, text: &str, style: &Style) -> Metrics {
        let s = style.to_sys();
        unsafe {
            glyph_sys::glyph_measure(self.raw, text.as_ptr() as *const i8, text.len(), &s)
        }
        .into()
    }

    pub fn draw(&self, canvas: &CanvasCommands<'_>, arena: &Arena, x: f32, y: f32, text: &str, style: &Style) {
        let s = style.to_sys();
        unsafe {
            glyph_sys::glyph_draw(
                self.raw,
                canvas.as_raw(),
                arena.as_raw(),
                x,
                y,
                text.as_ptr() as *const i8,
                text.len(),
                &s,
            )
        };
    }

    pub fn x_for_byte(&self, text: &str, byte: usize, style: &Style) -> f32 {
        let s = style.to_sys();
        unsafe {
            glyph_sys::glyph_x_for_byte(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                byte,
                &s,
            )
        }
    }

    pub fn byte_for_x(&self, text: &str, local_x: f32, style: &Style) -> usize {
        let s = style.to_sys();
        unsafe {
            glyph_sys::glyph_byte_for_x(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                local_x,
                &s,
            )
        }
    }

    pub fn selection_rects(&self, text: &str, lo: usize, hi: usize, style: &Style) -> Vec<XRange> {
        let s = style.to_sys();
        let mut buf = [glyph_sys::glyph_xrange { x0: 0.0, x1: 0.0 }; 16];
        let n = unsafe {
            glyph_sys::glyph_selection_rects(
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

    pub fn visual_move(&self, text: &str, byte: usize, forward: bool, style: &Style) -> usize {
        let s = style.to_sys();
        unsafe {
            glyph_sys::glyph_visual_move(
                self.raw,
                text.as_ptr() as *const i8,
                text.len(),
                byte,
                forward,
                &s,
            )
        }
    }

    pub fn as_raw(&self) -> *mut glyph_sys::glyph_ctx {
        self.raw
    }
}

impl Drop for Text {
    fn drop(&mut self) {
        unsafe { glyph_sys::glyph_release(self.raw) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
