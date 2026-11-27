//! Safe Rust bindings for **text** — HarfBuzz text shaping, layout metrics, and dynamic glyph atlas runtime (ADR-0016, ADR-0170).

#![deny(rust_2018_idioms)]

use optics_gfx::{Arena, CanvasCommands, Device};

pub use optics_gfx as gfx;
pub use optics_gfx::Error;
pub use optics_text_sys as sys;

pub(crate) fn check(rc: optics_text_sys::optics_gfx_result) -> Result<(), Error> {
    Error::check_raw(rc)
}

/// How a run looks: size, weight, family, and (premultiplied) colour.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Style {
    pub size_px: f32,
    pub weight: f32,
    pub color: u32,
    pub family: optics_text_sys::optics_text_family,
    pub italic: bool,
}

pub use optics_text_sys::optics_text_family as Family;

impl Style {
    pub fn new(size_px: f32, color: u32) -> Self {
        Self {
            size_px,
            weight: 0.0,
            color,
            family: optics_text_sys::optics_text_family::OPTICS_TEXT_FAMILY_DEFAULT,
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

    fn to_sys(self) -> optics_text_sys::optics_text_style {
        optics_text_sys::optics_text_style {
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

impl From<optics_text_sys::optics_text_metrics> for Metrics {
    fn from(m: optics_text_sys::optics_text_metrics) -> Self {
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
    raw: *mut optics_text_sys::optics_text_ctx,
}

impl Text {
    pub fn new(device: &Device) -> Result<Text, Error> {
        Self::create(device.as_raw(), 1.0)
    }

    pub fn measure_only() -> Result<Text, Error> {
        Self::create(std::ptr::null_mut(), 1.0)
    }

    fn create(device: *mut optics_text_sys::optics_gfx_device, scale: f32) -> Result<Text, Error> {
        let desc = optics_text_sys::optics_text_desc { device, scale };
        let mut out: *mut optics_text_sys::optics_text_ctx = std::ptr::null_mut();
        check(unsafe { optics_text_sys::optics_text_create(&desc, &mut out) })?;
        debug_assert!(!out.is_null());
        Ok(Text { raw: out })
    }

    pub fn set_scale(&self, scale: f32) {
        unsafe { optics_text_sys::optics_text_set_scale(self.raw, scale) };
    }

    pub fn scale(&self) -> f32 {
        unsafe { optics_text_sys::optics_text_scale(self.raw) }
    }

    pub fn default_family(&self) -> Family {
        unsafe { optics_text_sys::optics_text_default_family(self.raw) }
    }

    pub fn set_default_family(&self, family: Family) {
        unsafe { optics_text_sys::optics_text_set_default_family(self.raw, family) };
    }

    pub fn compact(&self) {
        unsafe { optics_text_sys::optics_text_compact(self.raw) };
    }

    pub fn measure(&self, text: &str, style: &Style) -> Metrics {
        let s = style.to_sys();
        let opts = optics_text_sys::optics_text_measure_opts {
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            style: &s,
        };
        unsafe { optics_text_sys::optics_text_measure(self.raw, &opts) }.into()
    }

    pub fn draw(
        &self,
        canvas: &CanvasCommands<'_>,
        arena: &Arena,
        x: f32,
        y: f32,
        text: &str,
        style: &Style,
    ) {
        let s = style.to_sys();
        let opts = optics_text_sys::optics_text_draw_opts {
            canvas: canvas.as_raw(),
            arena: arena.as_raw(),
            x,
            y,
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            style: &s,
            outline_color: 0,
            outline_width: 0.0,
        };
        unsafe { optics_text_sys::optics_text_draw(self.raw, &opts) };
    }

    pub fn x_for_byte(&self, text: &str, byte: usize, style: &Style) -> f32 {
        let s = style.to_sys();
        let opts = optics_text_sys::optics_text_x_for_byte_opts {
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            byte,
            style: &s,
        };
        unsafe { optics_text_sys::optics_text_x_for_byte(self.raw, &opts) }
    }

    pub fn byte_for_x(&self, text: &str, local_x: f32, style: &Style) -> usize {
        let s = style.to_sys();
        let opts = optics_text_sys::optics_text_byte_for_x_opts {
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            local_x,
            style: &s,
        };
        unsafe { optics_text_sys::optics_text_byte_for_x(self.raw, &opts) }
    }

    pub fn selection_rects(&self, text: &str, lo: usize, hi: usize, style: &Style) -> Vec<XRange> {
        let s = style.to_sys();
        let mut buf = [optics_text_sys::optics_text_xrange { x0: 0.0, x1: 0.0 }; 16];
        let opts = optics_text_sys::optics_text_selection_rects_opts {
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            lo,
            hi,
            style: &s,
            out: buf.as_mut_ptr(),
            max: buf.len() as i32,
        };
        let n = unsafe { optics_text_sys::optics_text_selection_rects(self.raw, &opts) };
        buf.iter()
            .take(n.max(0) as usize)
            .map(|r| XRange { x0: r.x0, x1: r.x1 })
            .collect()
    }

    pub fn visual_move(&self, text: &str, byte: usize, forward: bool, style: &Style) -> usize {
        let s = style.to_sys();
        let opts = optics_text_sys::optics_text_visual_move_opts {
            utf8: text.as_ptr() as *const i8,
            len: text.len(),
            byte,
            forward,
            style: &s,
        };
        unsafe { optics_text_sys::optics_text_visual_move(self.raw, &opts) }
    }

    pub fn as_raw(&self) -> *mut optics_text_sys::optics_text_ctx {
        self.raw
    }
}

impl Drop for Text {
    fn drop(&mut self) {
        unsafe { optics_text_sys::optics_text_release(self.raw) };
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
