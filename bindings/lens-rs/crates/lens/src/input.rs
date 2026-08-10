//! Per-frame input snapshot and a small builder over the C `lens_input`.
//!
//! A host (your event loop, or `lens-shell-wayland`) fills one of these each frame
//! from platform events and hands it to [`crate::Ui::frame`]. The C side reads
//! it read-only for the duration of the frame.

use lens_sys as sys;

/// Mouse buttons, matching `lens_mouse_button`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
    Left,
    Right,
    Middle,
}

impl MouseButton {
    fn index(self) -> usize {
        match self {
            MouseButton::Left => sys::lens_mouse_button::LENS_MOUSE_LEFT as usize,
            MouseButton::Right => sys::lens_mouse_button::LENS_MOUSE_RIGHT as usize,
            MouseButton::Middle => sys::lens_mouse_button::LENS_MOUSE_MIDDLE as usize,
        }
    }
}

/// A per-frame input snapshot. Wraps the C `lens_input`.
///
/// `Default` is an all-zero snapshot with the `size` forward-compat guard set —
/// valid for an idle frame. Build it up with the setters; the host is expected
/// to compute edge events (`*_pressed` / `*_released`) from its own state.
#[derive(Clone)]
#[repr(transparent)]
pub struct Input(sys::lens_input);

impl Default for Input {
    fn default() -> Self {
        Input(sys::lens_input {
            // ADR-0013 forward-compat guard.
            size: std::mem::size_of::<sys::lens_input>() as u32,
            ..sys::lens_input::default()
        })
    }
}

impl Input {
    /// A fresh snapshot for a frame of the given logical size and delta time.
    pub fn new(display_size: (f32, f32), dt_seconds: f32) -> Input {
        let mut i = Input::default();
        i.set_display_size(display_size.0, display_size.1);
        i.0.dt_seconds = dt_seconds;
        i
    }

    /// Pointer position, in UI-space (logical) pixels.
    pub fn set_cursor(&mut self, x: f32, y: f32) -> &mut Self {
        self.0.cursor = sys::flux_point { x, y };
        self
    }

    /// Whether a button is currently held. The host should also call
    /// [`Input::set_mouse_pressed`] / [`Input::set_mouse_released`] on the
    /// edges, which is what most widgets actually trigger on.
    pub fn set_mouse_down(&mut self, b: MouseButton, down: bool) -> &mut Self {
        self.0.mouse_down[b.index()] = down;
        self
    }

    /// Mark a press edge (button went down this frame).
    pub fn set_mouse_pressed(&mut self, b: MouseButton, pressed: bool) -> &mut Self {
        self.0.mouse_pressed[b.index()] = pressed;
        self
    }

    /// Mark a release edge (button went up this frame).
    pub fn set_mouse_released(&mut self, b: MouseButton, released: bool) -> &mut Self {
        self.0.mouse_released[b.index()] = released;
        self
    }

    /// Scroll deltas accumulated this frame.
    pub fn set_scroll(&mut self, x: f32, y: f32) -> &mut Self {
        self.0.scroll_x = x;
        self.0.scroll_y = y;
        self
    }

    /// Precise touchpad/continuous scroll distance in logical pixels.
    ///
    /// Unlike [`Input::set_scroll`], widgets consume these values directly
    /// instead of multiplying them by the configured wheel-step distance.
    pub fn set_scroll_pixels(&mut self, x: f32, y: f32) -> &mut Self {
        self.0.scroll_pixels_x = x;
        self.0.scroll_pixels_y = y;
        self
    }

    /// Active modifier mask (see [`mods`]).
    pub fn set_mods(&mut self, mask: u32) -> &mut Self {
        self.0.mods = mask;
        self
    }

    /// Layout root extent, in logical pixels.
    pub fn set_display_size(&mut self, w: f32, h: f32) -> &mut Self {
        self.0.display_size = sys::flux_point { x: w, y: h };
        self
    }

    /// Frame delta in seconds; drives animation.
    pub fn set_dt(&mut self, dt_seconds: f32) -> &mut Self {
        self.0.dt_seconds = dt_seconds;
        self
    }

    /// Set the committed UTF-8 text for this frame (e.g. typed characters).
    /// Truncated to the C buffer (31 bytes + NUL).
    pub fn set_text(&mut self, text: &str) -> &mut Self {
        let cap = self.0.text_utf8.len() - 1;
        let n = text.len().min(cap);
        for (dst, &src) in self.0.text_utf8.iter_mut().zip(&text.as_bytes()[..n]) {
            *dst = src as std::os::raw::c_char;
        }
        self.0.text_utf8[n] = 0;
        self
    }

    /// Set the in-progress IME composition (preedit) for this frame: the
    /// uncommitted text, the caret byte-offset within it, and the active-clause
    /// byte range `[sel_lo, sel_hi)`. The committed result still arrives through
    /// [`Input::set_text`]. Pass an empty string to clear. Truncated to the C
    /// buffer (63 bytes + NUL).
    pub fn set_preedit(&mut self, text: &str, cursor: u32, sel_lo: u32, sel_hi: u32) -> &mut Self {
        let cap = self.0.preedit_utf8.len() - 1;
        let n = text.len().min(cap);
        for (dst, &src) in self.0.preedit_utf8.iter_mut().zip(&text.as_bytes()[..n]) {
            *dst = src as std::os::raw::c_char;
        }
        self.0.preedit_utf8[n] = 0;
        self.0.preedit_cursor = cursor;
        self.0.preedit_sel_lo = sel_lo;
        self.0.preedit_sel_hi = sel_hi;
        self
    }

    /// Push a key edge event. `key` is a platform keycode or one of the
    /// portable [`key`] sentinels. No-op once the per-frame key array is full.
    pub fn push_key(&mut self, key: i32, pressed: bool, repeat: bool) -> &mut Self {
        let n = self.0.key_count as usize;
        if n < self.0.keys.len() {
            self.0.keys[n] = sys::lens_key_event {
                key,
                pressed,
                repeat,
            };
            self.0.key_count += 1;
        }
        self
    }

    /// Borrow the raw C snapshot.
    pub fn as_raw(&self) -> &sys::lens_input {
        &self.0
    }

    /// Mutably borrow the raw C snapshot for fields not covered above.
    pub fn as_raw_mut(&mut self) -> &mut sys::lens_input {
        &mut self.0
    }

    /// Wrap a borrowed raw `lens_input` as an [`Input`] reference.
    ///
    /// `Input` is a transparent wrapper over `lens_input`, so a `&lens_input`
    /// and a `&Input` have identical layout and lifetime. This is the
    /// borrowed-input constructor a host (e.g. iris's per-frame build
    /// callback) uses to surface the platform-assembled input snapshot to
    /// application code without copying.
    ///
    /// # Safety
    /// `raw` must point to a valid `lens_input` initialised by the C side
    /// (typically `lens_begin`'s caller) and remain valid for the duration
    /// the returned reference is used.
    pub unsafe fn from_raw_ref<'a>(raw: *const sys::lens_input) -> &'a Input {
        // SAFETY: the caller guarantees `raw` is valid for `'a`; `Input` is
        // transparent over `lens_input`, so alignment and layout are identical.
        unsafe { &*(raw as *const Input) }
    }
}

/// Modifier-mask bits for [`Input::set_mods`], mirroring `LENS_MOD_*`.
pub mod mods {
    pub const SHIFT: u32 = super::sys::LENS_MOD_SHIFT;
    pub const CTRL: u32 = super::sys::LENS_MOD_CTRL;
    pub const ALT: u32 = super::sys::LENS_MOD_ALT;
    pub const SUPER: u32 = super::sys::LENS_MOD_SUPER;
}

/// Portable key sentinels for [`Input::push_key`], mirroring `LENS_KEY_*`.
/// Platform codes outside this range pass through untouched.
pub mod key {
    pub const ESCAPE: i32 = super::sys::LENS_KEY_ESCAPE as i32;
    pub const TAB: i32 = super::sys::LENS_KEY_TAB as i32;
    pub const RETURN: i32 = super::sys::LENS_KEY_RETURN as i32;
    pub const BACKSPACE: i32 = super::sys::LENS_KEY_BACKSPACE as i32;
    pub const DELETE: i32 = super::sys::LENS_KEY_DELETE as i32;
    pub const LEFT: i32 = super::sys::LENS_KEY_LEFT as i32;
    pub const RIGHT: i32 = super::sys::LENS_KEY_RIGHT as i32;
    pub const HOME: i32 = super::sys::LENS_KEY_HOME as i32;
    pub const END: i32 = super::sys::LENS_KEY_END as i32;
    pub const UP: i32 = super::sys::LENS_KEY_UP as i32;
    pub const DOWN: i32 = super::sys::LENS_KEY_DOWN as i32;
}
