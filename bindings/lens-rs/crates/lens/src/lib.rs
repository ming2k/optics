//! Safe Rust bindings to **lens** — an immediate-mode UI façade over a
//! retained-mode core, drawing through flux's Vulkan canvas.
//!
//! Three ways in, lowest to highest level:
//!
//! 1. **Headless** ([`Ui::headless`]) — logic/layout/interaction with no GPU.
//! 2. **Embedded** ([`Ui::with_device`] + [`Ui::render`]) — you own a flux
//!    device and canvas; lens draws the UI into your frame. This is the
//!    seam for putting UI inside an existing flux/Vulkan app.
//! 3. **Windowed** — use the `lens-shell-wayland` crate, which owns the window,
//!    device, and event loop and just hands you a [`Frame`] each frame.
//!
//! ```no_run
//! let mut ui = lens::Ui::headless().unwrap();
//! let input = lens::Input::default();
//! ui.frame(&input, |f| {
//!     f.column().show(|f| {
//!         f.label("Settings");
//!         let mut wrap = true;
//!         f.checkbox("Wrap", &mut wrap);
//!         if f.button("Save") { /* ... */ }
//!     });
//! });
//! ```

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

/// Raw-bindings escape hatch (see the `flux` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
/// Raw-bindings escape hatch (see the `flux` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
pub use lens_sys as sys;

mod input;
pub mod patterns;
pub mod reactive;
mod types;
pub mod view;

pub use input::{Input, MouseButton, key, mods};
pub use types::{Align, Band, ButtonVariant, CheckboxAppearance, Color, CursorHint, FontFamily, Icon, LayoutOpts, PlaceMode, PlaceOpts, Rect, Response, SkinFn, Style, StyleResolved, TextLine, TextMetrics, Theme, WidgetContent, WidgetKind, WidgetRecord, WidgetState};

/// The retained UI context. Owns the persistent tree, layout, and draw list.
/// Dropping a `Ui` calls `lens_destroy`.
pub struct Ui {
    raw: *mut sys::lens,
    /// True when this handle was borrowed (`borrow_raw`) rather than created;
    /// borrowed handles do NOT destroy the context on drop.
    borrowed: bool,
}

impl Ui {
    /// Create a headless context (no flux device): immediate-mode logic,
    /// layout, and interaction all run, but [`Ui::render`] is unavailable.
    /// Ideal for tests and any logic that never touches the GPU.
    pub fn headless() -> Result<Ui, Error> {
        Self::create(ptr::null_mut())
    }

    /// Wrap a live `lens*` you do not own as a borrowed (non-owning) view.
    /// The returned `Ui` does **not** destroy the context on drop — the
    /// owner (e.g. iris, for its app context) stays in charge. Use this
    /// when a host hands you a live lens context (inside a start/setup
    /// callback) and you want the safe surface without taking ownership.
    ///
    /// # Safety
    /// `raw` must be a live `lens*` that remains valid for as long as the
    /// returned `Ui` (or any `&Ui` derived from it) is used, and must not
    /// be used through any other mutable view simultaneously.
    pub unsafe fn borrow_raw(raw: *mut sys::lens) -> Ui {
        Ui {
            raw,
            borrowed: true,
        }
    }

    /// Create a context bound to an existing flux device. The device is
    /// retained for the lifetime of the `Ui`. Use this to embed lens into
    /// an app that already manages its own flux/Vulkan setup; pair with
    /// [`Ui::render`] inside your canvas envelope.
    ///
    /// # Safety
    /// `device` must be a live `flux_device` obtained from the flux API and
    /// must remain valid until this `Ui` is dropped.
    pub unsafe fn with_device(device: *mut sys::flux_device) -> Result<Ui, Error> {
        Self::create(device)
    }

    fn create(device: *mut sys::flux_device) -> Result<Ui, Error> {
        // SAFETY: a zeroed desc with the given device is valid; theme is set to
        // the dark token set, all other fields fall back to library defaults.
        let desc = sys::lens_desc {
            device,
            theme: unsafe { sys::lens_theme_dark() },
            ..unsafe { std::mem::zeroed() }
        };
        let mut out: *mut sys::lens = ptr::null_mut();
        // SAFETY: desc is fully initialized; out is a valid slot.
        let rc = unsafe { sys::lens_create(&desc, &mut out) };
        if rc != sys::flux_result::FLUX_OK || out.is_null() {
            return Err(Error::Create(rc));
        }
        Ok(Ui {
            raw: out,
            borrowed: false,
        })
    }

    /// Whether the retained store overflowed: an id ring wrapped or the
    /// node pool filled, so some widgets from earlier frames may have lost
    /// their retained state. Long-lived hosts can surface this in
    /// diagnostics; ordinary UIs never hit it.
    pub fn overflowed(&self) -> bool {
        // SAFETY: self.raw is live; the call only reads state.
        unsafe { sys::lens_overflowed(self.raw as *const sys::lens) }
    }

    /// Whether one parent received the same retained widget id twice in the
    /// last frame. Repeated data rows should use an explicit widget id or an
    /// id scope; otherwise Lens can only retain one of the duplicate nodes.
    pub fn has_duplicate_ids(&self) -> bool {
        // SAFETY: self.raw is live; the call only reads frame diagnostics.
        unsafe { sys::lens_has_duplicate_ids(self.raw as *const sys::lens) }
    }

    /// Run one immediate-mode frame. The closure receives a [`Frame`] that
    /// borrows the context; widget calls on it build the tree, which is
    /// reconciled and laid out when the closure returns.
    pub fn frame<R>(&mut self, input: &Input, build: impl FnOnce(&mut Frame) -> R) -> R {
        // SAFETY: self.raw is live; input outlives the call.
        unsafe { sys::lens_begin(self.raw, input.as_raw()) };
        // SAFETY: raw is live and inside a begin/end pair.
        let mut f = unsafe { Frame::from_raw(self.raw) };
        let r = build(&mut f);
        // SAFETY: matched begin/end on the same live context.
        unsafe { sys::lens_end(self.raw) };
        r
    }

    /// Draw the last built frame into a flux canvas. Call between
    /// `flux_canvas_begin` and `flux_canvas_end` on your own frame.
    ///
    /// # Safety
    /// `canvas` must be a live `flux_canvas` inside an open canvas envelope,
    /// and this `Ui` must have been created with [`Ui::with_device`].
    pub unsafe fn render(&mut self, canvas: *mut sys::flux_canvas) -> Result<(), Error> {
        // SAFETY: the caller guarantees the canvas state; `self.raw` remains
        // live for the duration of this mutable borrow.
        let rc = unsafe { sys::lens_render(self.raw, canvas) };
        if rc != sys::flux_result::FLUX_OK {
            return Err(Error::Render(rc));
        }
        Ok(())
    }

    /// The raw context pointer, for reaching past the safe surface.
    pub fn as_raw(&mut self) -> *mut sys::lens {
        self.raw
    }

    /// Set the device-pixel (HiDPI) scale. Layout, input, and
    /// [`Input::set_display_size`] stay in logical pixels; [`Ui::render`]
    /// scales the canvas transform so 1 logical pixel maps to `scale` device
    /// pixels and rasterises text crisply. Report the window-system scale here.
    pub fn set_scale(&mut self, scale: f32) {
        // SAFETY: raw is live.
        unsafe { sys::lens_set_scale(self.raw, scale) };
    }

    /// Accessibility text scale — the OS "make text bigger" preference. A
    /// pure multiplier on every font-size token: glyphs, caret metrics, and
    /// every widget height derived from a font token grow together, so text
    /// scales without clipping. Orthogonal to [`Ui::set_scale`] (device
    /// pixels): a 1.25 text scale at 2x DPI renders 1.25x taller glyphs at
    /// the same 2x raster density. Pure-px geometry (padding, stroke widths)
    /// stays put. Non-finite and non-positive values are ignored. Default
    /// 1.0.
    pub fn set_text_scale(&mut self, factor: f32) {
        // SAFETY: raw is live.
        unsafe { sys::lens_set_text_scale(self.raw, factor) };
    }

    /// The current accessibility text-scale factor (default 1.0).
    pub fn text_scale(&self) -> f32 {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::lens_text_scale(self.raw as *const sys::lens) }
    }

    /// Replace the active theme token set (e.g. light/dark) live.
    pub fn set_theme(&mut self, theme: Theme) {
        // SAFETY: raw is live; theme is a value type.
        unsafe { sys::lens_set_theme(self.raw, theme.0) };
    }

    // ---- transient placement (host-side open/close, ADR-0060) --------------
    //
    // `Frame::place_open` / `Frame::place_close` / `Frame::place` cover the
    // common case of driving a transient popup from inside a frame. These
    // mirror the same calls on `Ui` so a host can open or dismiss a popup
    // outside a frame — e.g. from a keyboard handler that runs between
    // frames. The state is retained per id on the context, so a `close` here
    // is visible to the next frame's `Frame::place` / `Frame::place_is_open`.

    /// Open the transient place node keyed by `id` (retained until closed).
    /// Safe to call outside a frame; the body renders on the next
    /// [`Ui::frame`].
    pub fn place_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::lens_place_open(self.raw, c.as_ptr()) };
    }

    /// Close the transient place node keyed by `id`. Safe to call outside a
    /// frame.
    pub fn place_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::lens_place_close(self.raw, c.as_ptr()) };
    }

    /// Whether the transient place node keyed by `id` is currently open.
    /// Safe to call outside a frame.
    pub fn place_is_open(&mut self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call; read-only.
        unsafe { sys::lens_place_is_open(self.raw as *const sys::lens, c.as_ptr()) }
    }

    /// Whether an eased value (hover/active fade, …) was still in transit
    /// during the frame just built. An input-driven host that paints only on
    /// events should request one more frame while this holds, so animations
    /// settle to rest instead of freezing mid-fade when input stops. Valid
    /// after [`Ui::frame`] returns, until the next frame begins.
    pub fn anim_pending(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::lens_anim_pending(self.raw as *const sys::lens) }
    }

    /// The semantic cursor shape requested by the topmost hovered control on
    /// the most recently built frame (ADR-0044). Defaults to [`CursorHint::Default`].
    pub fn cursor_hint(&self) -> CursorHint {
        // SAFETY: raw is a live context; the call only reads state.
        CursorHint::from_raw(unsafe { sys::lens_get_cursor_hint(self.raw as *const sys::lens) })
    }

    /// Accessibility reduced-motion switch. While enabled, every eased value
    /// resolves to its target within one frame — no fades or slides — and
    /// [`Ui::anim_pending`] stays false. The host owns the policy (user
    /// preference); lens executes it. Default false.
    pub fn set_reduced_motion(&mut self, reduced: bool) {
        // SAFETY: raw is a live context.
        unsafe { sys::lens_set_reduced_motion(self.raw, reduced) };
    }

    /// Whether reduced motion is currently enabled.
    pub fn reduced_motion(&self) -> bool {
        // SAFETY: raw is a live context; the call only reads state.
        unsafe { sys::lens_reduced_motion(self.raw as *const sys::lens) }
    }

    // ---- clipboard & IME (host side, ADR-0013) ----------------------------

    /// Caret rect of the focused text widget, in UI-space; zero-sized when no
    /// text widget is focused. The host forwards this to the platform IME
    /// (e.g. `zwp_text_input`) so the candidate window can position itself.
    pub fn caret_rect(&self) -> Rect {
        // SAFETY: raw is a live context; the call only reads state.
        Rect::from_raw(unsafe { sys::lens_caret_rect(self.raw) })
    }

    /// Place text on the system clipboard (via the host clipboard interface, if
    /// one was supplied to the context). No-op otherwise.
    pub fn copy(&mut self, text: &str) {
        // SAFETY: raw is live; text/len describe a valid byte range.
        unsafe { sys::lens_copy(self.raw, text.as_ptr() as *const c_char, text.len()) };
    }

    /// Ask the host for clipboard text; it is later delivered via [`Ui::paste`].
    pub fn request_paste(&mut self) {
        // SAFETY: raw is live.
        unsafe { sys::lens_request_paste(self.raw) };
    }

    /// Deliver clipboard text into lens; the focused text widget consumes it
    /// on the next frame's build.
    pub fn paste(&mut self, text: &str) {
        // SAFETY: raw is live; text/len describe a valid byte range.
        unsafe { sys::lens_paste(self.raw, text.as_ptr() as *const c_char, text.len()) };
    }
}

impl Drop for Ui {
    fn drop(&mut self) {
        if self.borrowed {
            return; // the owner (e.g. iris) destroys the context
        }
        // SAFETY: raw was created by lens_create and not yet destroyed.
        unsafe { sys::lens_destroy(self.raw) };
    }
}

/// Builder handle valid only for the duration of one frame. It borrows the
/// context and must not escape the frame closure.
pub struct Frame {
    ui: *mut sys::lens,
}

impl Frame {
    /// Wrap a raw `lens` that the caller has already `lens_begin`'d. Used by
    /// `lens-shell-wayland`, whose host owns the begin/end envelope.
    ///
    /// # Safety
    /// `ui` must be a live context currently inside a `lens_begin` /
    /// `lens_end` pair, and the returned `Frame` must not outlive it.
    pub unsafe fn from_raw(ui: *mut sys::lens) -> Frame {
        Frame { ui }
    }

    /// The raw context pointer, for widgets not yet wrapped here.
    pub fn as_raw(&mut self) -> *mut sys::lens {
        self.ui
    }

    /// The active theme token set for this frame. Use it to pick colours that
    /// stay correct when the app runs in light or dark mode.
    pub fn theme(&self) -> Theme {
        // SAFETY: ui is live inside a frame; lens_get_theme only reads state.
        Theme(unsafe { sys::lens_get_theme(self.ui) })
    }

    /// Replace the active theme token set for subsequent widgets in this frame.
    pub fn set_theme(&mut self, theme: Theme) {
        // SAFETY: ui is live inside a frame; theme is a value type.
        unsafe { sys::lens_set_theme(self.ui, theme.0) };
    }

    /// Frame-scoped opacity switch (0..1, clamped; default 1.0): the single
    /// fade knob for enter/exit motion. Every node built while an opacity is
    /// in effect carries it as a build-time stamp, and emission bakes it into
    /// each draw command's colour alpha — rects, borders, text, icons, host
    /// images and scrollbars fade together, with no per-colour work by the
    /// caller. The switch resets to 1.0 at every frame begin, so a forgotten
    /// restore cannot dim the next frame; within a frame, set it back after
    /// building the faded subtree.
    pub fn set_opacity(&mut self, opacity: f32) {
        // SAFETY: ui is live inside a frame.
        unsafe { sys::lens_set_opacity(self.ui, opacity) };
    }

    /// The opacity currently applied to built widgets.
    pub fn opacity(&self) -> f32 {
        // SAFETY: ui is live inside a frame; the call only reads state.
        unsafe { sys::lens_opacity(self.ui as *const sys::lens) }
    }

    /// Set the typeface family for subsequently built widgets in this frame.
    /// Set it, build the widgets that need another voice, then set it back —
    /// the same pattern as [`Frame::set_theme`].
    pub fn set_text_family(&mut self, family: FontFamily) {
        // SAFETY: ui is live inside a frame; family is a value type.
        unsafe { sys::lens_set_text_family(self.ui, family.raw()) };
    }

    /// The typeface family currently applied to built widgets.
    pub fn text_family(&self) -> FontFamily {
        // SAFETY: ui is live inside a frame; the call only reads state.
        match unsafe { sys::lens_get_text_family(self.ui as *const sys::lens) } {
            sys::lens_text_family::LENS_TEXT_FAMILY_SANS => FontFamily::Sans,
            sys::lens_text_family::LENS_TEXT_FAMILY_SERIF => FontFamily::Serif,
            sys::lens_text_family::LENS_TEXT_FAMILY_MONO => FontFamily::Mono,
            _ => FontFamily::Sans,
        }
    }

    /// Push an ID seed string onto the UI's ID generator stack.
    pub fn push_id(&mut self, seed: &str) {
        let c = cstr(seed);
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_push_id(self.ui, c.as_ptr()) };
    }

    /// Push an allocation-free integer scope for repeated data rows.
    pub fn push_id_int(&mut self, seed: i64) {
        // SAFETY: ui is live and the integer is copied by value.
        unsafe { sys::lens_push_id_int(self.ui, seed) };
    }

    /// Pop the most recent [`Self::push_id`].
    pub fn pop_id(&mut self) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_pop_id(self.ui) };
    }

    /// Shape `text` with the active theme font and return its logical extent.
    /// This is the same measurement seam used by Lens widgets during their
    /// intrinsic-size pass.
    pub fn measure_text(&self, text: &str, size: f32) -> TextMetrics {
        let text = cstr(text);
        let theme = self.theme();
        // SAFETY: ui and the theme font are live for this frame; text is a
        // NUL-terminated string that outlives the call.
        TextMetrics::from_raw(unsafe {
            sys::lens_text_measure(self.ui, theme.0.font, text.as_ptr(), size.max(1.0))
        })
    }

    // ---- clipboard & IME (app-owned editing surfaces) -----------------

    /// Place text on the system clipboard (via the host clipboard
    /// interface, if one was supplied). No-op otherwise.
    pub fn copy(&mut self, text: &str) {
        // SAFETY: ui is live inside a frame; text/len describe a valid range.
        unsafe { sys::lens_copy(self.ui, text.as_ptr() as *const c_char, text.len()) };
    }

    /// Ask the host for clipboard text. The payload is delivered into
    /// lens's paste queue (same frame or a later one, platform-dependent);
    /// drain it with [`Frame::take_paste`].
    pub fn request_paste(&mut self) {
        // SAFETY: ui is live inside a frame.
        unsafe { sys::lens_request_paste(self.ui) };
    }

    /// Drain a pending paste payload (one-shot). For app-owned editing
    /// surfaces that render text outside lens widgets; lens text widgets
    /// consume their own paste internally, so only call this while the
    /// app's own surface is the paste target.
    pub fn take_paste(&mut self) -> Option<String> {
        // The C staging buffer caps payloads at 1 MiB (LENSI_PASTE_MAX).
        let mut buf = vec![0u8; 1024 * 1024];
        // SAFETY: ui is live; buf is writable for buf.len() bytes.
        let n = unsafe {
            sys::lens_take_paste(self.ui, buf.as_mut_ptr() as *mut c_char, buf.len() as u32)
        };
        if n == 0 {
            return None;
        }
        buf.truncate(n as usize);
        // The payload is host-supplied clipboard text; a non-UTF-8 payload
        // is not a paste we can use.
        String::from_utf8(buf).ok()
    }

    /// Report the caret rect of an app-owned editing surface so the
    /// platform IME can position its candidate window at the caret.
    /// UI-space logical pixels.
    pub fn set_caret_rect(&mut self, rect: Rect) {
        // SAFETY: ui is live inside a frame; rect is a value type.
        unsafe { sys::lens_set_caret_rect(self.ui, rect.to_raw()) };
    }

    // ---- containers -------------------------------------------------------

    /// Start a horizontal flex container (row) with a fluent builder.
    #[inline]
    pub fn row(&mut self) -> FlexBuilder<'_> {
        FlexBuilder::new(self, false)
    }

    /// Start a horizontal flex container (row) with preconfigured options.
    #[inline]
    pub fn row_ex<R>(&mut self, opts: &LayoutOpts, body: impl FnOnce(&mut Frame) -> R) -> (Response, R) {
        self.row().with_opts(opts).show(body)
    }

    /// Start a vertical flex container (column) with a fluent builder.
    #[inline]
    pub fn col(&mut self) -> FlexBuilder<'_> {
        FlexBuilder::new(self, true)
    }

    /// Start a vertical flex container (column) with preconfigured options.
    #[inline]
    pub fn col_ex<R>(&mut self, opts: &LayoutOpts, body: impl FnOnce(&mut Frame) -> R) -> (Response, R) {
        self.col().with_opts(opts).show(body)
    }

    /// Start a vertical flex container (column) with a fluent builder (alias for [`Self::col`]).
    #[inline]
    pub fn column(&mut self) -> FlexBuilder<'_> {
        FlexBuilder::new(self, true)
    }

    /// Start a vertical flex container (column) with preconfigured options (alias for [`Self::col_ex`]).
    #[inline]
    pub fn column_ex<R>(&mut self, opts: &LayoutOpts, body: impl FnOnce(&mut Frame) -> R) -> (Response, R) {
        self.col().with_opts(opts).show(body)
    }

    /// Center `body` on both axes inside a `width` × `height` box.
    ///
    /// Lens containers align only on the cross axis ([`LayoutOpts::cross`]);
    /// the main axis always packs from the start, so a bare label inside a
    /// fixed-size column sits at the top and a bare label inside a fixed-size
    /// row sits at the left. `centered` supplies the missing half of the
    /// idiom: a fixed-size row centres the content vertically on its cross
    /// axis, and a matched pair of flexible spacers shares the remaining
    /// main-axis space equally around it. Use it for button labels, icons,
    /// and any fixed-size tile whose content must sit at the optical centre.
    pub fn centered<R>(
        &mut self,
        width: f32,
        height: f32,
        body: impl FnOnce(&mut Frame) -> R,
    ) -> R {
        self.row()
            .width(width)
            .height(height)
            .items_center()
            .show_flat(|frame| {
                frame.flex(1.0);
                frame.spacer(0.0);
                let r = body(frame);
                frame.flex(1.0);
                frame.spacer(0.0);
                r
            })
    }

    /// Open a flex container configured via a [`FlexBuilder`].
    pub(crate) fn show_flex<R>(
        &mut self,
        is_col: bool,
        opts: &LayoutOpts,
        id: Option<(&str, &str)>,
        body: impl FnOnce(&mut Frame) -> R,
    ) -> (Response, R) {
        if let Some((id_str, label_str)) = id {
            let id = cstr(id_str);
            let label = cstr(label_str);
            let raw_opts = sys::lens_pressable_opts {
                box_: sys::lens_box {
                    id: id.as_ptr(),
                    ..Default::default()
                },
                label: label.as_ptr(),
                layout: opts.to_raw(),
                ..Default::default()
            };
            let response = Response::from_raw(unsafe {
                sys::lens_pressable_begin(self.ui, &raw_opts)
            });
            let result = body(self);
            unsafe { sys::lens_pressable_end(self.ui) };
            (response, result)
        } else if is_col {
            let raw_opts = opts.to_raw();
            unsafe { sys::lens_column_begin(self.ui, &raw_opts) };
            let r = body(self);
            unsafe { sys::lens_close(self.ui) };
            (Response::default(), r)
        } else {
            let raw_opts = opts.to_raw();
            unsafe { sys::lens_row_begin(self.ui, &raw_opts) };
            let r = body(self);
            unsafe { sys::lens_close(self.ui) };
            (Response::default(), r)
        }
    }

    pub fn pressable_row<R>(
        &mut self,
        id: &str,
        label: &str,
        opts: &LayoutOpts,
        body: impl FnOnce(&mut Frame, Response) -> R,
    ) -> (Response, R) {
        let id_c = cstr(id);
        let label_c = cstr(label);
        let raw_opts = sys::lens_pressable_opts {
            box_: sys::lens_box {
                id: id_c.as_ptr(),
                ..Default::default()
            },
            label: label_c.as_ptr(),
            layout: opts.to_raw(),
            ..Default::default()
        };
        let response = Response::from_raw(unsafe {
            sys::lens_pressable_begin(self.ui, &raw_opts)
        });
        let result = body(self, response);
        unsafe { sys::lens_pressable_end(self.ui) };
        (response, result)
    }

    
    /// A fixed empty gap along the main axis.
    pub fn spacer(&mut self, size: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_spacer(self.ui, size) };
    }

    /// A horizontal rule.
    pub fn separator(&mut self) {
        unsafe { sys::lens_separator(self.ui, std::ptr::null()); }
    }

    /// Fix the next node's size (pass 0 for an axis to keep its intrinsic size).
    pub fn size_next(&mut self, w: f32, h: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_size(self.ui, w, h) };
    }

    /// Give the next node a main-axis grow factor (0 = don't grow). Use it to
    /// fill remaining space (e.g. a flexible spacer, or the main content area
    /// beside a fixed-width sidebar).
    pub fn flex(&mut self, grow: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_flex(self.ui, grow) };
    }

    /// Push a style override for child widgets built within this scope.
    pub fn push_style(&mut self, style: Style) {
        unsafe { sys::lens_push_style(self.ui, style.0) };
    }

    /// Pop the most recent style override pushed with [`Frame::push_style`].
    pub fn pop_style(&mut self) {
        unsafe { sys::lens_pop_style(self.ui) };
    }

    /// Draw an icon glyph at `size` logical pixels. A contour behind the
    /// glyph is a style atom now (ADR-0061): wrap the call in
    /// [`Frame::push_style`] with [`Style::with_outline_color`] /
    /// [`Style::with_outline_width`].
    pub fn icon(&mut self, id: Icon, size: f32) {
        self.icon_raw(id.raw(), size);
    }

    pub fn icon_raw(&mut self, id: sys::lens_icon_id, size: f32) {
        let opts = sys::lens_icon_opts {
            id,
            size,
            ..Default::default()
        };
        unsafe { sys::lens_icon(self.ui, &opts); }
    }

    pub unsafe fn image(&mut self, image: *mut sys::flux_image, w: f32, h: f32) {
        let opts = sys::lens_image_opts {
            image,
            width: w,
            height: h,
            ..Default::default()
        };
        unsafe { sys::lens_image(self.ui, &opts); }
    }

    pub unsafe fn image_tinted(
        &mut self,
        image: *mut sys::flux_image,
        w: f32,
        h: f32,
        tint: Color,
    ) {
        let opts = sys::lens_image_opts {
            image,
            width: w,
            height: h,
            tint: tint.raw(),
            ..Default::default()
        };
        unsafe { sys::lens_image(self.ui, &opts); }
    }

    pub fn icon_button_raw(&mut self, id: sys::lens_icon_id) -> bool {
        let opts = sys::lens_button_opts {
            icon: id,
            ..Default::default()
        };
        unsafe { sys::lens_button(self.ui, &opts).clicked }
    }

    pub fn icon_button_raw_active(&mut self, id: sys::lens_icon_id, active: bool) -> bool {
        let opts = sys::lens_button_opts {
            icon: id,
            active,
            ..Default::default()
        };
        unsafe { sys::lens_button(self.ui, &opts).clicked }
    }

    pub fn icon_button(&mut self, id: Icon) -> bool {
        self.icon_button_raw(id.raw())
    }

    pub fn icon_button_active(&mut self, id: Icon, active: bool) -> bool {
        self.icon_button_raw_active(id.raw(), active)
    }

    pub fn button_primary(&mut self, label: &str) -> bool {
        let c = cstr(label);
        let opts = sys::lens_button_opts {
            label: c.as_ptr(),
            variant: sys::lens_button_variant::LENS_BUTTON_PRIMARY,
            ..Default::default()
        };
        unsafe { sys::lens_button(self.ui, &opts).clicked }
    }

    pub fn button_subtle(&mut self, label: &str) -> bool {
        let c = cstr(label);
        let opts = sys::lens_button_opts {
            label: c.as_ptr(),
            variant: sys::lens_button_variant::LENS_BUTTON_SUBTLE,
            ..Default::default()
        };
        unsafe { sys::lens_button(self.ui, &opts).clicked }
    }

    pub fn button(&mut self, label: &str) -> bool {
        let c = cstr(label);
        let opts = sys::lens_button_opts {
            label: c.as_ptr(),
            ..Default::default()
        };
        unsafe { sys::lens_button(self.ui, &opts).clicked }
    }

    pub fn checkbox(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        let opts = sys::lens_checkbox_opts {
            label: c.as_ptr(),
            value: value as *mut bool,
            ..Default::default()
        };
        unsafe { sys::lens_checkbox(self.ui, &opts).changed }
    }

    pub fn switch(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        let opts = sys::lens_checkbox_opts {
            label: c.as_ptr(),
            value: value as *mut bool,
            appearance: sys::lens_checkbox_appearance::LENS_CHECKBOX_SWITCH,
            ..Default::default()
        };
        unsafe { sys::lens_checkbox(self.ui, &opts).changed }
    }

    pub fn radio(&mut self, label: &str, value: &mut i32, option: i32) -> bool {
        let c = cstr(label);
        let mut is_on = *value == option;
        let opts = sys::lens_checkbox_opts {
            label: c.as_ptr(),
            value: &mut is_on as *mut bool,
            appearance: sys::lens_checkbox_appearance::LENS_CHECKBOX_RADIO,
            ..Default::default()
        };
        let r = unsafe { sys::lens_checkbox(self.ui, &opts) };
        if r.changed && is_on {
            *value = option;
            true
        } else {
            false
        }
    }

    pub fn slider(&mut self, label: &str, value: &mut f32, min: f32, max: f32) -> bool {
        let c = cstr(label);
        let opts = sys::lens_slider_opts {
            label: c.as_ptr(),
            value: value as *mut f32,
            min,
            max,
            ..Default::default()
        };
        unsafe { sys::lens_slider(self.ui, &opts).changed }
    }

    pub fn slider_vertical(&mut self, label: &str, value: &mut f32, min: f32, max: f32, step: f32) -> bool {
        let c = cstr(label);
        let opts = sys::lens_slider_opts {
            label: c.as_ptr(),
            value: value as *mut f32,
            min,
            max,
            step,
            axis: sys::lens_axis::LENS_COLUMN,
            ..Default::default()
        };
        unsafe { sys::lens_slider(self.ui, &opts).changed }
    }

    pub fn dropdown(&mut self, label: &str, selected: &mut i32, items: &[&str]) -> bool {
        let mut changed = false;
        let menu_id = format!("{}_dropdown_menu", label);
        let curr_label = if *selected >= 0 && (*selected as usize) < items.len() {
            items[*selected as usize]
        } else {
            label
        };
        if self.button(curr_label) {
            self.place_toggle(&menu_id);
        }
        let theme = self.theme();
        let opts = PlaceOpts {
            band: Band::Popup,
            mode: PlaceMode::Anchored,
            transient: true,
            layout: LayoutOpts {
                bg: theme.bg(),
                border: theme.border(),
                border_width: 1.0,
                pad: 4.0,
                gap: 2.0,
                ..Default::default()
            },
            ..Default::default()
        };
        self.place(&menu_id, &opts, |frame| {
            for (i, &item) in items.iter().enumerate() {
                if frame.selectable(item, *selected == i as i32) {
                    *selected = i as i32;
                    changed = true;
                    frame.place_close(&menu_id);
                }
            }
        });
        changed
    }

    pub fn setting_switch(
        &mut self,
        id: &str,
        label: &str,
        description: &str,
        value: &mut bool,
        disabled: bool,
    ) -> Response {
        let (resp, _) = self.row().id(id).cross(Align::Center).show(|frame| {
            frame.col().flex(1.0).show(|frame| {
                frame.label(label);
                if !description.is_empty() {
                    let muted_color = frame.theme().fg().with_alpha(160);
                    frame.push_style(Style::new().with_fg(muted_color));
                    frame.label_sized(description, frame.theme().font_size_sm());
                    frame.pop_style();
                }
            });
            if !disabled {
                frame.switch(label, value);
            }
        });
        resp
    }

    pub fn label(&mut self, text: &str) {
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            text: c.as_ptr(),
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn label_sized(&mut self, text: &str, size: f32) {
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            text: c.as_ptr(),
            size,
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn label_compact(&mut self, text: &str) {
        self.label(text);
    }

    pub fn label_compact_sized(&mut self, text: &str, size: f32) {
        self.label_sized(text, size);
    }

    pub fn label_compact_weighted(&mut self, text: &str, size: f32, weight: f32) {
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            text: c.as_ptr(),
            size,
            weight,
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn heading(&mut self, text: &str, level: i32) {
        let theme = self.theme();
        let size = match level {
            1 => theme.0.font_size_h1,
            2 => theme.0.font_size_h2,
            3 => theme.0.font_size_h3,
            _ => theme.0.font_size_h3,
        };
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            text: c.as_ptr(),
            size,
            weight: theme.0.font_weight_bold,
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn label_wrapped(&mut self, text: &str, max_width: f32) {
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            box_: sys::lens_box {
                max_width: max_width.max(0.0),
                ..Default::default()
            },
            text: c.as_ptr(),
            wrap: true,
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn label_wrapped_sized(&mut self, text: &str, size: f32, max_width: f32) {
        let c = cstr(text);
        let opts = sys::lens_label_opts {
            box_: sys::lens_box {
                max_width: max_width.max(0.0),
                ..Default::default()
            },
            text: c.as_ptr(),
            size,
            wrap: true,
            ..Default::default()
        };
        unsafe { sys::lens_label(self.ui, &opts); }
    }

    pub fn selectable(&mut self, label: &str, selected: bool) -> bool {
        let c = cstr(label);
        let opts = sys::lens_selectable_opts {
            label: c.as_ptr(),
            selected,
            ..Default::default()
        };
        unsafe { sys::lens_selectable(self.ui, &opts).clicked }
    }

    pub fn selectable_icon(&mut self, label: &str, icon: impl Into<sys::lens_icon_id>, selected: bool) -> bool {
        let c = cstr(label);
        let opts = sys::lens_selectable_opts {
            label: c.as_ptr(),
            icon: icon.into(),
            selected,
            ..Default::default()
        };
        unsafe { sys::lens_selectable(self.ui, &opts).clicked }
    }

    pub fn textfield(&mut self, label: &str, buf: &mut TextBuf) -> bool {
        self.textedit(label, buf, false)
    }

    pub fn textfield_set_caret(&mut self, label: &str, caret: u32) {
        self.textedit_set_caret(label, caret);
    }

    pub fn response(&self) -> Response {
        Response::from_raw(unsafe { sys::lens_get_response(self.ui as *const sys::lens) })
    }

    pub fn textfield_placeholder(&mut self, label: &str, buf: &mut TextBuf, placeholder: &str) -> bool {
        let c = cstr(label);
        let p = cstr(placeholder);
        let opts = sys::lens_textedit_opts {
            box_: sys::lens_box { id: c.as_ptr(), ..Default::default() },
            buf: buf.as_mut_ptr(),
            cap: buf.cap(),
            multiline: false,
            placeholder: p.as_ptr(),
            ..Default::default()
        };
        unsafe { sys::lens_textedit(self.ui, &opts).changed }
    }

    pub fn textfield_password(&mut self, label: &str, buf: &mut TextBuf, placeholder: &str) -> bool {
        let c = cstr(label);
        let p = cstr(placeholder);
        let opts = sys::lens_textedit_opts {
            box_: sys::lens_box { id: c.as_ptr(), ..Default::default() },
            buf: buf.as_mut_ptr(),
            cap: buf.cap(),
            multiline: false,
            placeholder: p.as_ptr(),
            password: true,
            ..Default::default()
        };
        unsafe { sys::lens_textedit(self.ui, &opts).changed }
    }

    pub fn scroll<R>(&mut self, id: &str, body: impl FnOnce(&mut Frame) -> R) -> R {
        let c = cstr(id);
        let opts = sys::lens_scroll_opts {
            box_: sys::lens_box {
                id: c.as_ptr(),
                ..Default::default()
            },
            ..Default::default()
        };
        unsafe { sys::lens_scroll_begin(self.ui, &opts) };
        let r = body(self);
        unsafe { sys::lens_scroll_end(self.ui) };
        r
    }

    pub fn place<R>(&mut self, id: &str, opts: &PlaceOpts, body: impl FnOnce(&mut Frame) -> R) -> Option<R> {
        let c = cstr(id);
        let mut raw = opts.to_raw();
        raw.box_.id = c.as_ptr();
        let open = unsafe { sys::lens_place_begin(self.ui, &raw) };
        if open {
            let r = body(self);
            unsafe { sys::lens_place_end(self.ui) };
            Some(r)
        } else {
            None
        }
    }

    pub fn place_open(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::lens_place_open(self.ui, c.as_ptr()) };
    }

    pub fn place_close(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::lens_place_close(self.ui, c.as_ptr()) };
    }

    pub fn place_toggle(&mut self, id: &str) {
        let c = cstr(id);
        unsafe { sys::lens_place_toggle(self.ui, c.as_ptr()) };
    }

    pub fn place_is_open(&self, id: &str) -> bool {
        let c = cstr(id);
        unsafe { sys::lens_place_is_open(self.ui as *const sys::lens, c.as_ptr()) }
    }

    pub fn place_close_all(&mut self) {
        unsafe { sys::lens_place_close_all(self.ui) };
    }

    pub fn node_bounds(&self, id: &str) -> Option<Rect> {
        let c = cstr(id);
        let raw_id = unsafe { sys::lens_current_id(self.ui, c.as_ptr()) };
        if raw_id == 0 {
            return None;
        }
        let node = unsafe { sys::lens_find(self.ui, raw_id) };
        if node.is_null() {
            return None;
        }
        Some(Rect::from_raw(unsafe { sys::lens_node_bounds(node) }))
    }

    pub fn clear_focus(&mut self) {
        unsafe { sys::lens_set_focus(self.ui, 0) };
    }

    pub fn textedit(&mut self, label: &str, buf: &mut TextBuf, multiline: bool) -> bool {
        let c = cstr(label);
        let opts = sys::lens_textedit_opts {
            box_: sys::lens_box { id: c.as_ptr(), ..Default::default() },
            buf: buf.as_mut_ptr(),
            cap: buf.cap(),
            multiline,
            ..Default::default()
        };
        unsafe { sys::lens_textedit(self.ui, &opts).changed }
    }

    pub fn textedit_set_caret(&mut self, label: &str, caret: u32) {
        let c = cstr(label);
        unsafe { sys::lens_textedit_set_caret(self.ui, c.as_ptr(), caret) };
    }

    pub fn textedit_set_selection(&mut self, label: &str, anchor: u32, caret: u32) {
        let c = cstr(label);
        unsafe { sys::lens_textedit_set_selection(self.ui, c.as_ptr(), anchor, caret) };
    }
}

/// A fixed-capacity, NUL-terminated UTF-8 edit buffer for [`Frame::textfield`].
///
/// lens edits the buffer in place (C string semantics), so Rust's `String`
/// can't back it directly. `TextBuf` owns a `Vec<u8>` of fixed capacity and
/// hands the C side a stable pointer.
pub struct TextBuf {
    bytes: Vec<u8>,
}

impl TextBuf {
    /// A new buffer of `capacity` bytes (including the NUL terminator),
    /// pre-filled with `initial`.
    pub fn new(capacity: usize, initial: &str) -> TextBuf {
        assert!(capacity >= 1, "capacity must leave room for a NUL");
        let mut bytes = vec![0u8; capacity];
        let src = initial.as_bytes();
        let n = src.len().min(capacity - 1);
        bytes[..n].copy_from_slice(&src[..n]);
        TextBuf { bytes }
    }

    fn as_mut_ptr(&mut self) -> *mut c_char {
        self.bytes.as_mut_ptr() as *mut c_char
    }

    fn cap(&self) -> usize {
        self.bytes.len()
    }

    /// Replace the buffer contents with `text`, preserving the capacity.
    /// Truncated to `capacity - 1` bytes (room for the NUL terminator). Use to
    /// reset a persistent buffer the host owns alongside a textfield — lens
    /// keeps its cursor clamped to the new length on the next frame. Follow
    /// with [`Frame::textfield_set_caret`] when the caret should land
    /// somewhere specific instead (e.g. the end of a Tab completion).
    pub fn set(&mut self, text: &str) {
        let cap = self.bytes.len();
        if cap == 0 {
            return;
        }
        let n = text.len().min(cap - 1);
        // Wipe first so a shorter replacement can't leave stale tail bytes
        // before the new NUL terminator.
        self.bytes.fill(0);
        self.bytes[..n].copy_from_slice(&text.as_bytes()[..n]);
        self.bytes[n] = 0;
    }

    /// The current contents up to the NUL terminator, as a `&str` (lossy if the
    /// C side ever wrote invalid UTF-8, which the text widgets do not).
    pub fn as_str(&self) -> std::borrow::Cow<'_, str> {
        let end = self
            .bytes
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(self.bytes.len());
        String::from_utf8_lossy(&self.bytes[..end])
    }
}

/// Errors from the safe surface.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    /// `lens_create` returned a non-OK `flux_result`.
    Create(sys::flux_result),
    /// `lens_render` returned a non-OK `flux_result`.
    Render(sys::flux_result),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Create(rc) => write!(f, "lens_create failed (flux_result = {rc:?})"),
            Error::Render(rc) => write!(f, "lens_render failed (flux_result = {rc:?})"),
        }
    }
}

impl std::error::Error for Error {}

/// Register an SVG string as a runtime lens icon (see `lens_icon_register_svg`).
///
/// The returned id continues where the built-in [`Icon`] enum ends
/// (`>= LENS_ICON_COUNT`). Registration is process-global, never reclaimed,
/// and — like the rest of lens — not thread-safe: call from the UI thread.
/// The SVG's own paint colours are ignored; the glyph draws in the theme
/// colour, stroke-only icons in the 2/24 weight of the built-in set.
pub fn register_svg_icon(svg: &str) -> Option<sys::lens_icon_id> {
    let svg = cstr(svg);
    // SAFETY: svg outlives the call; lens keeps no pointer into it.
    let id = unsafe { sys::lens_icon_register_svg(svg.as_ptr()) };
    // LENS_ICON_INVALID is (lens_icon_id)-1; the newtype binding makes it
    // plain data (u32::MAX), so this comparison is sound.
    if id.0 == u32::MAX { None } else { Some(id) }
}

/// The built-in default skin for a widget kind (ADR-0059). Useful for
/// wrapping: a custom skin can call the default to keep the stock chrome
/// and then add its own. Returns `None` for kinds outside the enum.
pub fn default_skin(kind: WidgetKind) -> SkinFn {
    // SAFETY: pure table lookup.
    unsafe { sys::lens_default_skin(kind) }
}

/// Push a filled rect from inside a skin (ADR-0059). `rel` is node-local;
/// a zero w/h spans the full node box.
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
/// Borrow a node's retained skin scratch (ADR-0061): four floats, zeroed on
/// the node's first touch, living and dying with the node. Mechanism, not
/// animation — a caller-owned skin stores its own state (a spring's
/// position/velocity) here; the library never integrates anything.
///
/// # Safety
/// `node` must be a live lens node obtained inside the current frame (e.g.
/// the node passed to a skin callback); the returned pointer is valid until
/// the node is reclaimed by the store GC.
pub unsafe fn skin_scratch(ui: *mut sys::lens, node: *mut sys::lens_node) -> *mut f32 {
    // SAFETY: forwarded contract — the caller guarantees a live node.
    unsafe { sys::lens_skin_scratch(ui, node) }
}
/// Push a filled rounded rect from inside a skin.
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_rect(
    ui: *mut sys::lens,
    node: *mut sys::lens_node,
    rel: Rect,
    color: Color,
    radius: f32,
) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::lens_skin_rect(ui, node, rel.to_raw(), color.raw(), radius) };
}

/// Push a border stroke from inside a skin. Same contract as [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_border(
    ui: *mut sys::lens,
    node: *mut sys::lens_node,
    rel: Rect,
    color: Color,
    radius: f32,
    width: f32,
) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::lens_skin_border(ui, node, rel.to_raw(), color.raw(), radius, width) };
}

/// Push text from inside a skin. A negative `rel.w`/`rel.h` centres the
/// run in the resolved node rect at render time. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call; lens copies the text into its frame arena.
pub unsafe fn skin_text(
    ui: *mut sys::lens,
    node: *mut sys::lens_node,
    rel: Rect,
    color: Color,
    text: &str,
    size_px: f32,
    weight: f32,
) {
    let c = cstr(text);
    // SAFETY: forwarded contract from the caller; c outlives the call.
    unsafe {
        sys::lens_skin_text(
            ui,
            node,
            rel.to_raw(),
            color.raw(),
            c.as_ptr(),
            size_px,
            weight,
        )
    };
}

/// Push a vector icon glyph from inside a skin. `icon` is a raw
/// `lens_icon_id`, so runtime-registered glyphs ([`register_svg_icon`])
/// work alongside the built-in [`Icon`] set. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_icon(
    ui: *mut sys::lens,
    node: *mut sys::lens_node,
    rel: Rect,
    color: Color,
    stroke: f32,
    icon: sys::lens_icon_id,
) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::lens_skin_icon(ui, node, rel.to_raw(), color.raw(), stroke, icon) };
}

/// Push a nested logical clip from inside a skin (e.g. per-cell clips in a
/// table skin). Balanced with [`skin_clip_pop`]; the rect is intersected
/// with the enclosing clip at render time. Same contract as [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call.
pub unsafe fn skin_clip_push(ui: *mut sys::lens, node: *mut sys::lens_node, rel: Rect) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::lens_skin_clip_push(ui, node, rel.to_raw()) };
}

/// Pop a clip pushed with [`skin_clip_push`]. Same contract as
/// [`skin_rect`].
///
/// # Safety
/// `ui` and `node` must be the values the skin was called with, during
/// that call, and every pop must balance an earlier push.
pub unsafe fn skin_clip_pop(ui: *mut sys::lens, node: *mut sys::lens_node) {
    // SAFETY: forwarded contract from the caller.
    unsafe { sys::lens_skin_clip_pop(ui, node) };
}

/// The lens version string reported by the linked C library.
pub fn version() -> &'static str {
    // SAFETY: lens_version_string returns a static NUL-terminated string.
    let p = unsafe { sys::lens_version_string() };
    unsafe { std::ffi::CStr::from_ptr(p) }
        .to_str()
        .unwrap_or("<invalid>")
}

/// A zero-allocation fluent builder for Flexbox containers (rows, columns, and
/// custom layout boxes).
///
/// Combines layout options, visual styling (background, border, radius), and
/// optional interactive target semantics (`id` / pressable) into a single,
/// unified container primitive.
pub struct FlexBuilder<'a> {
    frame: &'a mut Frame,
    is_col: bool,
    opts: LayoutOpts,
    id: Option<(&'a str, &'a str)>,
}

impl<'a> FlexBuilder<'a> {
    pub(crate) fn new(frame: &'a mut Frame, is_col: bool) -> Self {
        Self {
            frame,
            is_col,
            opts: LayoutOpts::new(),
            id: None,
        }
    }

    /// Set main-axis gap between children in logical px.
    #[inline]
    pub fn gap(mut self, px: f32) -> Self {
        self.opts.gap = px;
        self
    }

    /// Set uniform padding in logical px.
    #[inline]
    pub fn pad(mut self, px: f32) -> Self {
        self.opts.pad = px;
        self
    }

    /// Center children along the cross-axis.
    #[inline]
    pub fn items_center(mut self) -> Self {
        self.opts.cross = Align::Center;
        self
    }

    /// Set cross-axis alignment.
    #[inline]
    pub fn cross(mut self, align: Align) -> Self {
        self.opts.cross = align;
        self
    }

    /// Set flex grow factor.
    #[inline]
    pub fn flex(mut self, factor: f32) -> Self {
        self.opts.flex = factor;
        self
    }

    /// Set background fill color.
    #[inline]
    pub fn bg(mut self, color: Color) -> Self {
        self.opts.bg = color;
        self
    }

    /// Set corner radius in logical px.
    #[inline]
    pub fn radius(mut self, r: f32) -> Self {
        self.opts.radius = r;
        self
    }

    /// Set corner radius in logical px (alias for [`Self::radius`]).
    #[inline]
    pub fn rounded(self, r: f32) -> Self {
        self.radius(r)
    }

    /// Set border stroke color.
    #[inline]
    pub fn border(mut self, color: Color) -> Self {
        self.opts.border = color;
        self
    }

    /// Set border stroke width in logical px.
    #[inline]
    pub fn border_width(mut self, w: f32) -> Self {
        self.opts.border_width = w;
        self
    }

    /// Set explicit width in logical px.
    #[inline]
    pub fn width(mut self, w: f32) -> Self {
        self.opts.width = w;
        self
    }

    /// Set explicit height in logical px.
    #[inline]
    pub fn height(mut self, h: f32) -> Self {
        self.opts.height = h;
        self
    }

    /// Set minimum resolved width in logical px.
    #[inline]
    pub fn min_width(mut self, w: f32) -> Self {
        self.opts.min_width = w;
        self
    }

    /// Set minimum resolved height in logical px.
    #[inline]
    pub fn min_height(mut self, h: f32) -> Self {
        self.opts.min_height = h;
        self
    }

    /// Set maximum resolved width in logical px.
    #[inline]
    pub fn max_width(mut self, w: f32) -> Self {
        self.opts.max_width = w;
        self
    }

    /// Set maximum resolved height in logical px.
    #[inline]
    pub fn max_height(mut self, h: f32) -> Self {
        self.opts.max_height = h;
        self
    }

    /// Center children along the cross-axis (alias for [`Self::items_center`]).
    #[inline]
    pub fn center(self) -> Self {
        self.items_center()
    }

    /// Set explicit width in logical px (alias for [`Self::width`]).
    #[inline]
    pub fn w(self, w: f32) -> Self {
        self.width(w)
    }

    /// Set explicit height in logical px (alias for [`Self::height`]).
    #[inline]
    pub fn h(self, h: f32) -> Self {
        self.height(h)
    }

    /// Set minimum resolved width in logical px (alias for [`Self::min_width`]).
    #[inline]
    pub fn min_w(self, w: f32) -> Self {
        self.min_width(w)
    }

    /// Set minimum resolved height in logical px (alias for [`Self::min_height`]).
    #[inline]
    pub fn min_h(self, h: f32) -> Self {
        self.min_height(h)
    }

    /// Set maximum resolved width in logical px (alias for [`Self::max_width`]).
    #[inline]
    pub fn max_w(self, w: f32) -> Self {
        self.max_width(w)
    }

    /// Set maximum resolved height in logical px (alias for [`Self::max_height`]).
    #[inline]
    pub fn max_h(self, h: f32) -> Self {
        self.max_height(h)
    }

    /// Apply all options from a [`LayoutOpts`] descriptor.
    #[inline]
    pub fn with_opts(mut self, opts: &LayoutOpts) -> Self {
        self.opts = *opts;
        self
    }

    /// Assign an interaction ID to turn this container into an interactive
    /// click/hover target.
    #[inline]
    pub fn id(mut self, id: &'a str) -> Self {
        self.id = Some((id, ""));
        self
    }

    /// Assign an interaction ID and accessibility label.
    #[inline]
    pub fn id_label(mut self, id: &'a str, label: &'a str) -> Self {
        self.id = Some((id, label));
        self
    }

    /// Open the container scope, execute `body` with the child frame, and
    /// return a tuple of `(Response, R)`.
    pub fn show<R>(self, body: impl FnOnce(&mut Frame) -> R) -> (Response, R) {
        self.frame.show_flex(self.is_col, &self.opts, self.id, body)
    }

    /// Open the container scope, execute `body`, and return only the closure result `R`.
    #[inline]
    pub fn show_flat<R>(self, body: impl FnOnce(&mut Frame) -> R) -> R {
        self.show(body).1
    }

    /// Render an empty container node and return its interaction response.
    #[inline]
    pub fn empty(self) -> Response {
        self.show(|_| ()).0
    }
}

/// Build a `CString`, panicking on an interior NUL (a programming error in the
/// caller's literal, not a runtime condition).
fn cstr(s: &str) -> CString {
    CString::new(s).expect("string passed to a widget contains an interior NUL")
}
