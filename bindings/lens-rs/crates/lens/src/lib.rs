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
//!     f.column(|f| {
//!         f.title("Settings");
//!         let mut wrap = true;
//!         f.checkbox("Wrap", &mut wrap);
//!         if f.button("Save") { /* ... */ }
//!     });
//! });
//! ```

use std::ffi::CString;
use std::os::raw::c_char;
use std::ptr;

pub use lens_sys as sys;

mod input;
mod types;

pub use input::{Input, MouseButton, key, mods};
pub use types::{
    Align, Band, Color, CursorHint, FontFamily, GridColumn, GridRow, Icon, LayoutOpts, ModalOpts,
    PlaceMode, PlaceOpts, Rect, Response, SkinFn, Style, StyleResolved, TableColumn, TableOpts,
    TableResult, TabsOpts, TextLine, TextMetrics, Theme, WidgetContent, WidgetKind, WidgetRecord,
    WidgetState,
};

/// The retained UI context. Owns the persistent tree, layout, and draw list.
/// Dropping a `Ui` calls `lens_destroy`.
pub struct Ui {
    raw: *mut sys::lens,
}

impl Ui {
    /// Create a headless context (no flux device): immediate-mode logic,
    /// layout, and interaction all run, but [`Ui::render`] is unavailable.
    /// Ideal for tests and any logic that never touches the GPU.
    pub fn headless() -> Result<Ui, Error> {
        Self::create(ptr::null_mut())
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
        Ok(Ui { raw: out })
    }

    /// Whether the retained store overflowed: an id ring wrapped or the
    /// node pool filled, so some widgets from earlier frames may have lost
    /// their retained state. Long-lived hosts can surface this in
    /// diagnostics; ordinary UIs never hit it.
    pub fn overflowed(&self) -> bool {
        // SAFETY: self.raw is live; the call only reads state.
        unsafe { sys::lens_overflowed(self.raw as *const sys::lens) }
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
            _ => FontFamily::Default,
        }
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

    /// Open a horizontal container, run `body`, then close it.
    pub fn row<R>(&mut self, body: impl FnOnce(&mut Frame) -> R) -> R {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_row(self.ui) };
        let r = body(self);
        // SAFETY: matched lens_row / lens_close.
        unsafe { sys::lens_close(self.ui) };
        r
    }

    /// Open a vertical container, run `body`, then close it.
    pub fn column<R>(&mut self, body: impl FnOnce(&mut Frame) -> R) -> R {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_column(self.ui) };
        let r = body(self);
        // SAFETY: matched lens_column / lens_close.
        unsafe { sys::lens_close(self.ui) };
        r
    }

    /// Open a horizontal container with explicit layout options — gap between
    /// children, padding from the edges, cross-axis alignment, and an optional
    /// background surface. The descriptor form of [`Frame::row`]; use it
    /// whenever you need spacing or a surface, since the plain `row` is gap-
    /// and pad-less.
    pub fn row_ex<R>(&mut self, opts: &LayoutOpts, body: impl FnOnce(&mut Frame) -> R) -> R {
        // SAFETY: ui is live; opts is a value type built into a valid descriptor.
        unsafe { sys::lens_row_ex(self.ui, opts.to_raw()) };
        let r = body(self);
        // SAFETY: matched lens_row_ex / lens_close.
        unsafe { sys::lens_close(self.ui) };
        r
    }

    /// Open a vertical container with explicit layout options. The descriptor
    /// form of [`Frame::column`]; see [`Frame::row_ex`].
    pub fn column_ex<R>(&mut self, opts: &LayoutOpts, body: impl FnOnce(&mut Frame) -> R) -> R {
        // SAFETY: ui is live; opts is a value type built into a valid descriptor.
        unsafe { sys::lens_column_ex(self.ui, opts.to_raw()) };
        let r = body(self);
        // SAFETY: matched lens_column_ex / lens_close.
        unsafe { sys::lens_close(self.ui) };
        r
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
    pub fn centered<R>(&mut self, width: f32, height: f32, body: impl FnOnce(&mut Frame) -> R) -> R {
        self.row_ex(
            &LayoutOpts {
                width,
                height,
                cross: Align::Center,
                ..Default::default()
            },
            |frame| {
                frame.flex(1.0);
                frame.spacer(0.0);
                let r = body(frame);
                frame.flex(1.0);
                frame.spacer(0.0);
                r
            },
        )
    }

    /// A composable row whose complete bounds form one button interaction
    /// target. Children are presentation only; hover, focus, press, and click
    /// are reported by the returned [`Response`] for the row as a whole.
    pub fn pressable_row<R>(
        &mut self,
        id: &str,
        label: &str,
        opts: &LayoutOpts,
        body: impl FnOnce(&mut Frame, Response) -> R,
    ) -> (Response, R) {
        let id = cstr(id);
        let label = cstr(label);
        // SAFETY: ui is live and both strings outlive this call.
        let response = Response::from_raw(unsafe {
            sys::lens_pressable_begin(self.ui, id.as_ptr(), label.as_ptr(), opts.to_raw())
        });
        let result = body(self, response);
        // SAFETY: matched lens_pressable_begin.
        unsafe { sys::lens_pressable_end(self.ui) };
        (response, result)
    }

    /// A collapsing header. Returns `true` while expanded; put the body inside
    /// the `if`.
    pub fn collapsing(&mut self, label: &str, body: impl FnOnce(&mut Frame)) {
        let c = cstr(label);
        // SAFETY: ui is live for the frame.
        let open = unsafe { sys::lens_collapsing(self.ui, c.as_ptr()) };
        if open {
            body(self);
            // SAFETY: matched lens_collapsing / lens_close.
            unsafe { sys::lens_close(self.ui) };
        }
    }

    /// A collapsing header with a separate identity (`id`) and visible
    /// `label`. The label may change between frames (e.g. a count suffix)
    /// without resetting the expanded/collapsed state, because the
    /// underlying widget id is anchored to `id` via [`Self::push_id`].
    /// Use this when the label is dynamic; otherwise prefer
    /// [`Self::collapsing`].
    pub fn collapsing_scoped(&mut self, id: &str, label: &str, body: impl FnOnce(&mut Frame)) {
        self.push_id(id);
        self.collapsing(label, body);
        self.pop_id();
    }

    /// Push a stable id segment onto the widget-id stack so widgets declared
    /// inside the closure (or until the matching [`Self::pop_id`]) hash under
    /// a scope that doesn't depend on their label. Used by
    /// [`Self::collapsing_scoped`]; also useful for disambiguating two
    /// same-labelled widgets in a loop.
    pub fn push_id(&mut self, seed: &str) {
        let c = cstr(seed);
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_push_id(self.ui, c.as_ptr()) };
    }

    /// Pop the most recent [`Self::push_id`]. Every `push_id` must be paired
    /// with a `pop_id` before the frame ends.
    pub fn pop_id(&mut self) {
        // SAFETY: ui is live for the frame; caller balances push/pop.
        unsafe { sys::lens_pop_id(self.ui) };
    }

    /// Force the expanded state of a collapsing section. Call **before**
    /// [`Frame::collapsing`] with the same label, typically on the first
    /// frame after startup, to seed the initial open/closed state from
    /// persisted settings. On subsequent frames the user's toggles take
    /// over (lens's retained store remembers the state).
    pub fn collapsing_set_open(&mut self, label: &str, open: bool) {
        let c = cstr(label);
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_collapsing_set_open(self.ui, c.as_ptr(), open) };
    }

    /// Like [`Self::collapsing_set_open`] but for a scoped collapsing — the
    /// `id` must match what was passed to [`Self::collapsing_scoped`]. Call
    /// from within a matching [`Self::push_id`] / [`Self::pop_id`] pair (or
    /// pass the same `id` to a transient push around the call).
    pub fn collapsing_scoped_set_open(&mut self, id: &str, label: &str, open: bool) {
        self.push_id(id);
        self.collapsing_set_open(label, open);
        self.pop_id();
    }

    /// A fixed empty gap along the main axis.
    pub fn spacer(&mut self, size: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_spacer(self.ui, size) };
    }

    /// A horizontal rule.
    pub fn separator(&mut self) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_separator(self.ui) };
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

    /// Draw an icon glyph at `size` logical pixels. A contour behind the
    /// glyph is a style atom now (ADR-0061): wrap the call in
    /// [`Frame::push_style`] with [`Style::with_outline_color`] /
    /// [`Style::with_outline_width`].
    pub fn icon(&mut self, id: Icon, size: f32) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_icon(self.ui, id.raw(), size) };
    }

    /// Draw a host-owned raster image (e.g. a decoded application icon),
    /// scaled to fill a `w`×`h` logical box.
    ///
    /// # Safety
    /// `image` must be a live `flux_image` obtained from the flux API and
    /// remain valid until the next [`Ui::render`] returns. The caller owns
    /// the texture; lens borrows it for the frame. This mirrors the
    /// device/canvas borrow the embedding host already establishes.
    pub unsafe fn image(&mut self, image: *mut sys::flux_image, w: f32, h: f32) {
        // SAFETY: ui is live for the frame; image outlives render (caller's contract).
        unsafe { sys::lens_image(self.ui, image, w, h) };
    }

    /// Draw a host-owned raster image modulated by a premultiplied `tint`.
    /// Opaque white preserves the source; white with a lower alpha fades it.
    ///
    /// # Safety
    /// `image` follows the same lifetime and ownership contract as
    /// [`Frame::image`].
    pub unsafe fn image_tinted(
        &mut self,
        image: *mut sys::flux_image,
        w: f32,
        h: f32,
        tint: Color,
    ) {
        // SAFETY: ui is live for the frame; image outlives render (caller's contract).
        unsafe { sys::lens_image_tinted(self.ui, image, w, h, tint.raw()) };
    }

    /// A flat icon-only button for navigation strips and toolbars: transparent
    /// at rest, with a subtle fill on hover. Returns `true` on the frame it is
    /// clicked.
    pub fn icon_button(&mut self, id: Icon) -> bool {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_icon_button(self.ui, id.raw()) }
    }

    /// As [`Frame::icon_button`], but `active` shows a steady neutral tint
    /// (the cascade-resolved bg_pressed; theme: color_active) for the
    /// selected view — state as data, no flavor (ADR-0061). An accent glyph
    /// or rail is a scope/style-atom or custom-skin decision, not a separate
    /// API. Returns `true` on the frame it is clicked.
    pub fn icon_button_active(&mut self, id: Icon, active: bool) -> bool {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_icon_button_active(self.ui, id.raw(), active) }
    }

    /// A rounded icon tile with an explicit glyph size and optional top-right
    /// badge. An empty `badge` draws only the icon.
    pub fn icon_button_badged(
        &mut self,
        id: Icon,
        badge: &str,
        glyph_size: f32,
        active: bool,
    ) -> bool {
        let badge = cstr(badge);
        // SAFETY: ui is live; badge remains valid for the duration of the
        // call, and Lens copies text draw commands into its frame arena.
        unsafe {
            sys::lens_icon_button_badged(
                self.ui,
                id.raw(),
                badge.as_ptr(),
                glyph_size.max(1.0),
                active,
            )
        }
    }

    /// A checkable rounded icon button that swaps its glyph instead of
    /// painting a persistent selected background. The checked glyph uses the
    /// theme accent; hover feedback remains visible.
    pub fn icon_toggle_button(
        &mut self,
        unchecked_icon: Icon,
        checked_icon: Icon,
        glyph_size: f32,
        checked: bool,
    ) -> bool {
        // SAFETY: ui is live and both icon IDs are generated Lens values.
        unsafe {
            sys::lens_icon_toggle_button(
                self.ui,
                unchecked_icon.raw(),
                checked_icon.raw(),
                glyph_size.max(1.0),
                checked,
            )
        }
    }

    /// Texture-backed `icon_button`. Same hover/click behaviour, draws the
    /// host-owned raster `image` instead of a glyph.
    ///
    /// # Safety
    /// `image` must be a live `flux_image` that outlives the next
    /// [`Ui::render`]. Pass a null pointer for a background-only blank tile.
    pub unsafe fn image_button(&mut self, image: *mut sys::flux_image) -> bool {
        // SAFETY: ui is live; image outlives render (caller's contract).
        unsafe { sys::lens_image_button(self.ui, image) }
    }

    /// Texture-backed `icon_button_active`.
    ///
    /// # Safety
    /// Same lifetime contract as [`Frame::image_button`].
    pub unsafe fn image_button_active(
        &mut self,
        image: *mut sys::flux_image,
        active: bool,
    ) -> bool {
        // SAFETY: ui is live; image outlives render (caller's contract).
        unsafe { sys::lens_image_button_active(self.ui, image, active) }
    }

    /// A scroll area keyed by `id`. Build its (usually a `column`/`row`)
    /// contents in `body`; wheel events over it scroll the content. Combine
    /// with [`Frame::size_next`] to cap its visible height.
    pub fn scroll<R>(&mut self, id: &str, body: impl FnOnce(&mut Frame) -> R) -> R {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_scroll_begin(self.ui, c.as_ptr()) };
        let r = body(self);
        // SAFETY: matched scroll begin/end.
        unsafe { sys::lens_scroll_end(self.ui) };
        r
    }

    /// Set the retained offset of a scroll area in the current id scope.
    ///
    /// Call this after building the matching [`Frame::scroll`] body. Lens
    /// clamps the request to the resolved content bounds during layout;
    /// requesting an id that has not appeared yet is a harmless no-op.
    pub fn scroll_to(&mut self, id: &str, x: f32, y: f32) {
        let id = cstr(id);
        // SAFETY: ui is live and id outlives the call.
        unsafe { sys::lens_scroll_to(self.ui, id.as_ptr(), x.max(0.0), y.max(0.0)) };
    }

    /// Current offset of the scroll area `id` in the current id scope, or
    /// `None` when no such scroll area exists yet (e.g. on the first frame).
    /// Virtualized lists use this to bound their build to the visible window.
    pub fn scroll_offset(&self, id: &str) -> Option<(f32, f32)> {
        let id = cstr(id);
        let (mut x, mut y) = (0.0f32, 0.0f32);
        // SAFETY: ui is live and id outlives the call.
        let found = unsafe { sys::lens_scroll_offset(self.ui, id.as_ptr(), &mut x, &mut y) };
        found.then_some((x, y))
    }

    /// A virtualized, scrollable table. Only the visible rows are requested
    /// through `cell`, so rendering cost stays bounded for large libraries.
    /// Use [`Frame::size_next`] or [`Frame::flex`] immediately before this
    /// call to define its viewport.
    pub fn table<F>(
        &mut self,
        id: &str,
        columns: &[TableColumn<'_>],
        row_count: usize,
        opts: TableOpts,
        cell: F,
    ) -> TableResult
    where
        F: FnMut(usize, usize) -> String,
    {
        struct CallbackState<F> {
            cell: F,
            scratch: CString,
        }

        unsafe extern "C" fn cell_trampoline<F>(
            user: *mut std::ffi::c_void,
            row: i32,
            column: i32,
        ) -> *const c_char
        where
            F: FnMut(usize, usize) -> String,
        {
            if user.is_null() || row < 0 || column < 0 {
                return c"".as_ptr();
            }
            // SAFETY: `user` points to the stack-local CallbackState for the
            // synchronous duration of lens_table. The C widget copies each
            // returned string into its frame arena before invoking us again.
            let state = unsafe { &mut *user.cast::<CallbackState<F>>() };
            // A panic must not unwind across the FFI boundary (abort); report
            // an empty cell instead, matching iris-rs's trampoline policy.
            let value = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                (state.cell)(row as usize, column as usize)
            }))
            .unwrap_or_default()
            .replace('\0', "�");
            state.scratch = CString::new(value).unwrap_or_default();
            state.scratch.as_ptr()
        }

        let id = cstr(id);
        let titles = columns
            .iter()
            .map(|column| cstr(column.title))
            .collect::<Vec<_>>();
        let raw_columns = columns
            .iter()
            .zip(&titles)
            .map(|(column, title)| sys::lens_table_column {
                title: title.as_ptr(),
                width: column.width.max(0.0),
                align: column.align.raw(),
            })
            .collect::<Vec<_>>();
        let mut state = CallbackState {
            cell,
            scratch: CString::default(),
        };
        let raw = unsafe {
            sys::lens_table(
                self.ui,
                id.as_ptr(),
                raw_columns.as_ptr(),
                i32::try_from(raw_columns.len()).unwrap_or(i32::MAX),
                i32::try_from(row_count).unwrap_or(i32::MAX),
                Some(cell_trampoline::<F>),
                (&mut state as *mut CallbackState<F>).cast(),
                sys::lens_table_opts {
                    row_height: opts.row_height.max(0.0),
                    show_header: opts.show_header,
                    selectable: opts.selectable,
                    zebra: opts.zebra,
                    keyboard: opts.keyboard,
                    cursor: ptr::null_mut(),
                    icon_fn: None,
                    selected_fn: None,
                },
            )
        };
        TableResult {
            selected: usize::try_from(raw.selected).ok(),
            selection_changed: raw.selection_changed,
            clicked: raw.clicked,
            cursor: usize::try_from(raw.cursor).ok(),
            cursor_changed: raw.cursor_changed,
            activated: raw.activated,
            clicked_row: usize::try_from(raw.clicked_row).ok(),
        }
    }

    /// A virtualized, scrollable table with the ADR-0066 extensions: a
    /// keyboard cursor, per-cell icons, and a host-owned selection set.
    ///
    /// - With `opts.keyboard` the focused table moves a cursor row on
    ///   Up/Down/Home/End and activates it on Return (Space stays with the
    ///   host for typeahead/toggles); the cursor row scrolls into view. The cursor is a row INDEX carried in `cursor`
    ///   (-1 = none): the table reads it at build start and writes it back
    ///   whenever the effective cursor moves, so hosts re-seed it on model
    ///   resets by owning the variable.
    /// - `icon` yields the glyph for a cell, `None` for none. It returns
    ///   the raw `sys::lens_icon_id` — not the safe [`Icon`] enum — because
    ///   the enum surfaces only a subset of the built-in glyphs and hosts
    ///   use runtime-registered SVG ids. Only [`Align::Start`] columns draw
    ///   an icon; other alignments ignore it.
    /// - `selected` reports row membership in the host's selection set;
    ///   clicks then only report `clicked_row` and `result.selected` stays
    ///   `None`.
    #[allow(clippy::too_many_arguments)] // mirrors the C call's arity
    pub fn table_ex<F, I, S>(
        &mut self,
        id: &str,
        columns: &[TableColumn<'_>],
        row_count: usize,
        opts: TableOpts,
        cell: F,
        icon: I,
        selected: S,
        cursor: &mut i32,
    ) -> TableResult
    where
        F: FnMut(usize, usize) -> String,
        I: FnMut(usize, usize) -> Option<sys::lens_icon_id>,
        S: FnMut(usize) -> bool,
    {
        struct CallbackState<F, I, S> {
            cell: F,
            scratch: CString,
            icon: I,
            selected: S,
        }

        unsafe extern "C" fn cell_trampoline<F, I, S>(
            user: *mut std::ffi::c_void,
            row: i32,
            column: i32,
        ) -> *const c_char
        where
            F: FnMut(usize, usize) -> String,
            I: FnMut(usize, usize) -> Option<sys::lens_icon_id>,
            S: FnMut(usize) -> bool,
        {
            if user.is_null() || row < 0 || column < 0 {
                return c"".as_ptr();
            }
            // SAFETY: `user` points to the stack-local CallbackState for the
            // synchronous duration of lens_table. The C widget copies each
            // returned string into its frame arena before invoking us again.
            let state = unsafe { &mut *user.cast::<CallbackState<F, I, S>>() };
            // A panic must not unwind across the FFI boundary (abort); report
            // an empty cell instead, matching iris-rs's trampoline policy.
            let value = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                (state.cell)(row as usize, column as usize)
            }))
            .unwrap_or_default()
            .replace('\0', "");
            state.scratch = CString::new(value).unwrap_or_default();
            state.scratch.as_ptr()
        }

        unsafe extern "C" fn icon_trampoline<F, I, S>(
            user: *mut std::ffi::c_void,
            row: i32,
            column: i32,
        ) -> sys::lens_icon_id
        where
            F: FnMut(usize, usize) -> String,
            I: FnMut(usize, usize) -> Option<sys::lens_icon_id>,
            S: FnMut(usize) -> bool,
        {
            /// `LENS_ICON_INVALID` — `(lens_icon_id)-1` in C; the bindgen
            /// newtype wraps the unsigned bit pattern.
            const INVALID: sys::lens_icon_id = sys::lens_icon_id(u32::MAX);
            if user.is_null() || row < 0 || column < 0 {
                return INVALID;
            }
            // SAFETY: `user` points to the stack-local CallbackState for the
            // synchronous duration of lens_table.
            let state = unsafe { &mut *user.cast::<CallbackState<F, I, S>>() };
            // A panic must not unwind across the FFI boundary (abort);
            // report no icon instead.
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                (state.icon)(row as usize, column as usize)
            }))
            .unwrap_or_default()
            .unwrap_or(INVALID)
        }

        unsafe extern "C" fn selected_trampoline<F, I, S>(
            user: *mut std::ffi::c_void,
            row: i32,
        ) -> bool
        where
            F: FnMut(usize, usize) -> String,
            I: FnMut(usize, usize) -> Option<sys::lens_icon_id>,
            S: FnMut(usize) -> bool,
        {
            if user.is_null() || row < 0 {
                return false;
            }
            // SAFETY: `user` points to the stack-local CallbackState for the
            // synchronous duration of lens_table.
            let state = unsafe { &mut *user.cast::<CallbackState<F, I, S>>() };
            // A panic must not unwind across the FFI boundary (abort);
            // report unselected instead.
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                (state.selected)(row as usize)
            }))
            .unwrap_or_default()
        }

        let id = cstr(id);
        let titles = columns
            .iter()
            .map(|column| cstr(column.title))
            .collect::<Vec<_>>();
        let raw_columns = columns
            .iter()
            .zip(&titles)
            .map(|(column, title)| sys::lens_table_column {
                title: title.as_ptr(),
                width: column.width.max(0.0),
                align: column.align.raw(),
            })
            .collect::<Vec<_>>();
        let mut state = CallbackState {
            cell,
            scratch: CString::default(),
            icon,
            selected,
        };
        let raw = unsafe {
            sys::lens_table(
                self.ui,
                id.as_ptr(),
                raw_columns.as_ptr(),
                i32::try_from(raw_columns.len()).unwrap_or(i32::MAX),
                i32::try_from(row_count).unwrap_or(i32::MAX),
                Some(cell_trampoline::<F, I, S>),
                (&mut state as *mut CallbackState<F, I, S>).cast(),
                sys::lens_table_opts {
                    row_height: opts.row_height.max(0.0),
                    show_header: opts.show_header,
                    selectable: opts.selectable,
                    zebra: opts.zebra,
                    keyboard: opts.keyboard,
                    cursor: cursor as *mut i32,
                    icon_fn: Some(icon_trampoline::<F, I, S>),
                    selected_fn: Some(selected_trampoline::<F, I, S>),
                },
            )
        };
        TableResult {
            selected: usize::try_from(raw.selected).ok(),
            selection_changed: raw.selection_changed,
            clicked: raw.clicked,
            cursor: usize::try_from(raw.cursor).ok(),
            cursor_changed: raw.cursor_changed,
            activated: raw.activated,
            clicked_row: usize::try_from(raw.clicked_row).ok(),
        }
    }

    /// A standard tab strip keyed by `id`, tracking the selected index in
    /// `active`. Use [`Frame::tabs_ex`] to opt into another visual relationship
    /// without changing the library-wide default.
    /// Declare the tabs inside `strip` with [`Frame::tab`]; render the selected
    /// panel *after* this call, switching on `*active`.
    ///
    /// ```no_run
    /// # let mut ui = lens::Ui::headless().unwrap();
    /// # let input = lens::Input::default();
    /// # let mut active = 0i32;
    /// # ui.frame(&input, |f| {
    /// f.tabs("settings", &mut active, |f| {
    ///     f.tab("General");
    ///     f.tab("Advanced");
    /// });
    /// match active {
    ///     0 => f.label("General panel"),
    ///     _ => f.label("Advanced panel"),
    /// }
    /// # });
    /// ```
    pub fn tabs(&mut self, id: &str, active: &mut i32, strip: impl FnOnce(&mut Frame)) {
        let c = cstr(id);
        // SAFETY: ui is live; c and active outlive the call.
        let build = unsafe { sys::lens_tabs_begin(self.ui, c.as_ptr(), active as *mut i32) };
        if build {
            strip(self);
        }
        // lens_tabs_end is called unconditionally (matches the C contract).
        // SAFETY: matched tabs begin/end.
        unsafe { sys::lens_tabs_end(self.ui) };
    }

    /// A tab strip with structural options (ADR-0061: the presentation
    /// knobs retired — the default skin draws the neutral static indicator;
    /// visual tuning goes through the style cascade, and a different
    /// presentation is a caller-owned skin). `equal_width` gives every tab
    /// the same share of the strip.
    pub fn tabs_ex(
        &mut self,
        id: &str,
        active: &mut i32,
        opts: &TabsOpts,
        strip: impl FnOnce(&mut Frame),
    ) {
        let c = cstr(id);
        // SAFETY: ui is live; c and active outlive the call; opts is copied.
        let build = unsafe {
            sys::lens_tabs_begin_ex(self.ui, c.as_ptr(), active as *mut i32, opts.to_raw())
        };
        if build {
            strip(self);
        }
        // SAFETY: matched tabs begin/end.
        unsafe { sys::lens_tabs_end(self.ui) };
    }

    /// Declare one tab inside a [`Frame::tabs`] strip. Returns `true` when this
    /// tab is the active one.
    pub fn tab(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_tab(self.ui, c.as_ptr()) }
    }

    /// A dropdown selecting an index into `items`, stored in `selected`.
    /// Its vector chevron stays on the trailing edge and the floating option
    /// list owns an opaque surface. The list opens below the trigger when it
    /// fits there, flips above otherwise, and is height-capped (at most ~7
    /// rows) with its own scrolling: a wheel over the list scrolls it, a
    /// wheel anywhere else closes the popup. In a scroll area the list
    /// inherits the viewport as its placement boundary. Returns `true` when
    /// selection changes.
    pub fn dropdown(&mut self, label: &str, selected: &mut i32, items: &[&str]) -> bool {
        let label_c = cstr(label);
        // Keep the CStrings alive for the whole call; collect their pointers.
        let owned: Vec<CString> = items.iter().map(|s| cstr(s)).collect();
        let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
        // SAFETY: ui is live; label_c, owned, and ptrs all outlive the call;
        // ptrs has `items.len()` valid entries.
        unsafe {
            sys::lens_dropdown(
                self.ui,
                label_c.as_ptr(),
                selected as *mut i32,
                ptrs.as_ptr() as *mut *const c_char,
                ptrs.len() as i32,
            )
        }
    }

    // ---- queries & placement ------------------------------------------------

    /// Bounds of a widget or placed subtree from a previous frame, resolved
    /// from its id within the current id-stack context (typically the root at
    /// the top of the build callback). Returns `None` when no such node exists
    /// yet (e.g. on the first frame). Useful for hosts that layer custom
    /// pointer handling under the chrome: read the chrome's bounds here and
    /// exclude hits inside them.
    pub fn node_bounds(&self, id: &str) -> Option<Rect> {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the calls.
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

    /// Move keyboard focus to no widget. Hosts with their own canvas
    /// interaction call this when a pointer press starts a canvas gesture so
    /// stray key presses (Space, Enter) do not re-trigger a focused button.
    pub fn clear_focus(&mut self) {
        // SAFETY: ui is live; id 0 is the documented "none".
        unsafe { sys::lens_set_focus(self.ui, 0) };
    }

    /// The interaction result of the most recently built widget — useful for a
    /// popup anchor (`f.response().rect`) or reading hover/click after the
    /// fact.
    pub fn response(&self) -> Response {
        // SAFETY: ui is live; the call only reads state.
        Response::from_raw(unsafe { sys::lens_get_response(self.ui as *const sys::lens) })
    }

    /// Whether any widget currently holds the active (pressed) role. Hosts
    /// mixing custom canvas pointer handling with lens chrome should skip
    /// canvas gestures while this is true.
    pub fn active_widget(&self) -> bool {
        // SAFETY: ui is live; the call only reads state.
        unsafe { sys::lens_active(self.ui as *const sys::lens) != 0 }
    }

    /// Whether the retained store overflowed (see [`Ui::overflowed`]).
    /// Convenience mirror for hosts driving the frame closure.
    pub fn overflowed(&self) -> bool {
        // SAFETY: ui is live for the duration of the build closure.
        unsafe { sys::lens_overflowed(self.ui as *const sys::lens) }
    }

    /// Cursor intent accumulated from hovered widgets built so far. Read this
    /// once after building the frame and pass it to the windowing host.
    pub fn cursor_hint(&self) -> CursorHint {
        // SAFETY: ui is live; the call only reads state.
        CursorHint::from_raw(unsafe { sys::lens_get_cursor_hint(self.ui as *const sys::lens) })
    }

    /// Open the transient place node keyed by `id` (retained until closed).
    pub fn place_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_place_open(self.ui, c.as_ptr()) };
    }

    /// Close the transient place node keyed by `id`.
    pub fn place_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_place_close(self.ui, c.as_ptr()) };
    }

    /// Whether the transient place node keyed by `id` is currently open.
    pub fn place_is_open(&self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call; read-only.
        unsafe { sys::lens_place_is_open(self.ui as *const sys::lens, c.as_ptr()) }
    }

    /// Whether the cursor is inside the open transient node's last-frame
    /// bounds. Combine this with the owner widget's hover response to
    /// implement a hover-to-open popup that remains usable while the cursor
    /// crosses over.
    pub fn place_hovered(&self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call; read-only.
        unsafe { sys::lens_place_hovered(self.ui as *const sys::lens, c.as_ptr()) }
    }

    /// An absolutely-placed container sub-root (ADR-0060): it keeps its
    /// parent chain but escapes the parent's layout flow and clip, and is
    /// emitted in `opts.band` z order. For a transient node the `body` runs
    /// only while the id is open; non-transient bodies always run (the
    /// persistent-chrome case). Pair the placement with
    /// [`Frame::place_open`] / [`Frame::place_close`] for transients.
    pub fn place(&mut self, id: &str, opts: &PlaceOpts, body: impl FnOnce(&mut Frame)) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        let open = unsafe { sys::lens_place_begin(self.ui, c.as_ptr(), opts.to_raw()) };
        if open {
            body(self);
            // SAFETY: only paired with a true return from lens_place_begin.
            unsafe { sys::lens_place_end(self.ui) };
        }
    }

    // ---- modal dialogs (ADR-0039) ------------------------------------------

    /// Open the modal dialog keyed by `id`. The body builds on subsequent
    /// frames while the id stays open; pair with [`Frame::modal`] to declare
    /// the body.
    pub fn modal_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live for the frame; c outlives the call.
        unsafe { sys::lens_modal_open(self.ui, c.as_ptr()) };
    }

    /// Close the modal dialog keyed by `id`.
    pub fn modal_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live for the frame; c outlives the call.
        unsafe { sys::lens_modal_close(self.ui, c.as_ptr()) };
    }

    /// Whether the modal dialog keyed by `id` is currently open.
    pub fn modal_is_open(&self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: ui is live for the frame; c outlives the call; read-only.
        unsafe { sys::lens_modal_is_open(self.ui as *const sys::lens, c.as_ptr()) }
    }

    /// A centered modal dialog with a dim backdrop and a Tab focus trap.
    /// `body` runs only while the dialog is open (see [`Frame::modal_open`]);
    /// the focusable widgets built inside define the Tab cycle.
    pub fn modal(&mut self, id: &str, opts: &ModalOpts, body: impl FnOnce(&mut Frame)) {
        let c = cstr(id);
        let title = opts.title.map(cstr);
        let raw = sys::lens_modal_opts {
            title: title.as_ref().map_or(ptr::null(), |t| t.as_ptr()),
            backdrop: opts.backdrop.raw(),
            min_width: opts.min_width.max(0.0),
            pinned: opts.pinned,
        };
        // SAFETY: ui is live for the frame; c and title outlive the call; lens
        // copies the title into its frame arena.
        let open = unsafe { sys::lens_modal_begin(self.ui, c.as_ptr(), raw) };
        if open {
            body(self);
            // SAFETY: only paired with a true return from lens_modal_begin.
            unsafe { sys::lens_modal_end(self.ui) };
        }
    }

    // ---- widgets ----------------------------------------------------------

    /// A button. Returns `true` on the frame it is clicked.
    pub fn button(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_button(self.ui, c.as_ptr()) }
    }

    /// A lightweight inline text action for breadcrumbs and secondary
    /// navigation. It remains plain text at rest and gains an accent
    /// underline on hover/focus without changing size or weight.
    pub fn link(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_link(self.ui, c.as_ptr()) }
    }

    /// A plain, borderless, full-width list or navigation row. It is
    /// transparent at rest, uses a subtle fill on hover, and uses the theme's
    /// active-surface colour when `selected`. Returns `true` on the frame it
    /// is clicked.
    ///
    /// Unlike [`Frame::button`], it has no persistent filled surface or border
    /// at rest, so a `column` of selectables reads as one flat navigation list.
    /// In a stretched column the row spans the full width, so the whole row is
    /// clickable.
    pub fn selectable(&mut self, label: &str, selected: bool) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_selectable(self.ui, c.as_ptr(), selected) }
    }

    /// A selectable row with a leading icon. It has one full-row hit target
    /// and draws icon, text, hover, and selected background as one widget.
    pub fn selectable_icon(&mut self, icon: Icon, label: &str, selected: bool) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_selectable_icon(self.ui, icon.raw(), c.as_ptr(), selected) }
    }

    /// Push a partial style onto the scope stack (ADR-0061): every widget
    /// declared until the matching [`Frame::pop_style`] — terse forms
    /// included — resolves its unset atoms against the merged scope
    /// (per-call box styles still win; the theme fills the rest). This is
    /// the primitive for caller-built design-system scopes ("danger",
    /// "sidebar"). The stack resets every frame, so a forgotten pop cannot
    /// leak.
    pub fn push_style(&mut self, style: Style) {
        // SAFETY: ui is live for the frame; style is copied by value.
        unsafe { sys::lens_push_style(self.ui, style.0) };
    }

    /// Pop the innermost style scope pushed with [`Frame::push_style`].
    pub fn pop_style(&mut self) {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_pop_style(self.ui) };
    }

    /// Replace the skin for a widget kind context-wide (ADR-0059). Every
    /// migrated widget of that kind — however it is called — now draws
    /// through `skin`. `None` restores the built-in default. The context is
    /// the single override granularity (ADR-0061 retired the per-call forms):
    /// for a one-off override, set the skin, build the widget, restore `None`.
    pub fn set_skin(&mut self, kind: WidgetKind, skin: SkinFn) {
        // SAFETY: ui is live for the frame; the fn pointer (or None) is
        // stored on the context and called during later widget builds.
        unsafe { sys::lens_set_skin(self.ui, kind, skin) };
    }

    /// A text label.
    pub fn label(&mut self, text: &str) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_label(self.ui, c.as_ptr()) };
    }

    /// A text label at a specific point size.
    pub fn label_sized(&mut self, text: &str, size: f32) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_label_ex(self.ui, c.as_ptr(), size) };
    }

    /// A text label that wraps within `max_width` logical pixels.
    pub fn label_wrapped(&mut self, text: &str, max_width: f32) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_label_wrapped(self.ui, c.as_ptr(), max_width.max(0.0)) };
    }

    /// A wrapped text label at a specific point size.
    pub fn label_wrapped_sized(&mut self, text: &str, size: f32, max_width: f32) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_label_wrapped_ex(self.ui, c.as_ptr(), size, max_width.max(0.0)) };
    }

    /// A text label without the theme padding. Use inside fixed-height chrome
    /// such as status bars where the regular label would overflow.
    pub fn label_compact_sized(&mut self, text: &str, size: f32) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_label_compact_ex(self.ui, c.as_ptr(), size) };
    }

    /// A compact label that measures and draws at an explicit weight
    /// (0 = the theme's regular weight). The weight rides the widget
    /// record, so an overriding label skin sees the same value.
    pub fn label_compact_weighted(&mut self, text: &str, size: f32, weight: f32) {
        let c = cstr(text);
        // SAFETY: ui is live for the frame; c outlives the call.
        unsafe { sys::lens_label_compact_ex2(self.ui, c.as_ptr(), size, weight) };
    }

    /// A title (larger, emphasized label).
    pub fn title(&mut self, text: &str) {
        let c = cstr(text);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_title(self.ui, c.as_ptr()) };
    }

    /// A semantic heading with an explicit level from 1 (largest) to 6.
    pub fn heading(&mut self, text: &str, level: i32) {
        let c = cstr(text);
        // SAFETY: ui is live for the frame; c outlives the call.
        unsafe { sys::lens_heading(self.ui, c.as_ptr(), level.clamp(1, 6)) };
    }

    /// A checkbox bound to `value`. Returns `true` on the frame it toggles.
    pub fn checkbox(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_checkbox(self.ui, c.as_ptr(), value as *mut bool) }
    }

    /// A compact boolean switch bound to `value`.
    pub fn switch(&mut self, label: &str, value: &mut bool) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_switch(self.ui, c.as_ptr(), value as *mut bool) }
    }

    /// A full-width settings row with title, supporting description, and a
    /// trailing switch. `id` keeps identity stable when text is translated.
    pub fn setting_switch(
        &mut self,
        id: &str,
        label: &str,
        description: &str,
        value: &mut bool,
        disabled: bool,
    ) -> Response {
        let id = cstr(id);
        let label = cstr(label);
        let description = cstr(description);
        let opts = sys::lens_switch_opts {
            box_: sys::lens_box {
                id: id.as_ptr(),
                flex: 0.0,
                width: 0.0,
                height: 0.0,
                disabled,
                error: false,
                tooltip: ptr::null(),
                style: sys::lens_style::default(),
            },
            label: label.as_ptr(),
            description: description.as_ptr(),
            value: value as *mut bool,
        };
        // SAFETY: ui is live and every pointer in opts remains valid for the
        // synchronous call; Lens copies visible text into its frame arena.
        Response::from_raw(unsafe { sys::lens_switch_ex(self.ui, opts) })
    }

    /// A slider bound to `value`, clamped to `[min, max]`. Its knob stays
    /// hidden at rest and animates in for hover, keyboard focus, or dragging.
    /// Returns `true` on the frame the value changes.
    pub fn slider(&mut self, label: &str, value: &mut f32, min: f32, max: f32) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_slider(self.ui, c.as_ptr(), value as *mut f32, min, max) }
    }

    /// A vertical slider with `min` at the bottom and `max` at the top.
    /// Hovered wheel input and Up/Down keys adjust by `step`; pass `0.0` to
    /// use one twentieth of the range.
    pub fn slider_vertical(
        &mut self,
        label: &str,
        value: &mut f32,
        min: f32,
        max: f32,
        step: f32,
    ) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_slider_vertical(self.ui, c.as_ptr(), value as *mut f32, min, max, step) }
    }

    /// Adjust `value` from the current wheel delta when the most recently
    /// built widget is hovered. The wheel event is consumed on use.
    pub fn adjust_float_on_scroll(
        &mut self,
        value: &mut f32,
        min: f32,
        max: f32,
        step: f32,
    ) -> bool {
        // SAFETY: ui is live and value outlives the call.
        unsafe { sys::lens_adjust_float_on_scroll(self.ui, value, min, max, step) }
    }

    /// A radio button. Sets `*value = option` and returns `true` when picked.
    pub fn radio(&mut self, label: &str, value: &mut i32, option: i32) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_radio(self.ui, c.as_ptr(), value as *mut i32, option) }
    }

    /// A progress bar in `[0, 1]`.
    pub fn progress(&mut self, label: &str, value: f32) {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_progress(self.ui, c.as_ptr(), value) };
    }

    /// A single-line text field editing `buf` in place. `buf` must stay valid
    /// and NUL-terminated; editing happens within its capacity. Returns `true`
    /// on the frame the text changes.
    pub fn textfield(&mut self, label: &str, buf: &mut TextBuf) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and buf outlive the call; buf is NUL-terminated
        // with capacity buf.cap().
        unsafe { sys::lens_textfield(self.ui, c.as_ptr(), buf.as_mut_ptr(), buf.cap()) }
    }

    /// As [`Frame::textfield`], but shows `placeholder` as a hint while the
    /// buffer is empty and the field is unfocused.
    pub fn textfield_placeholder(
        &mut self,
        label: &str,
        buf: &mut TextBuf,
        placeholder: &str,
    ) -> bool {
        let c = cstr(label);
        let hint = cstr(placeholder);
        // SAFETY: ui is live; c, hint and buf outlive the call; buf is
        // NUL-terminated with capacity buf.cap().
        let r = unsafe {
            sys::lens_textfield_ex(
                self.ui,
                sys::lens_textfield_opts {
                    label: c.as_ptr(),
                    buf: buf.as_mut_ptr(),
                    buf_cap: buf.cap(),
                    placeholder: hint.as_ptr(),
                    ..Default::default()
                },
            )
        };
        r.changed
    }

    /// Move the caret of the text field identified by `label` (a byte offset
    /// into the edit buffer, not a character index), collapsing any
    /// selection. Call **before** [`Frame::textfield`] with the same label in
    /// the same frame (or on an earlier frame) — typically right after
    /// programmatically rewriting the buffer for Tab completion or a
    /// pre-filled value. The write is unconditional: it wins over the field's
    /// remembered position for that frame, then the field's own editing takes
    /// over again.
    ///
    /// Out-of-range offsets clamp to the buffer length and offsets that land
    /// mid-character snap back to a UTF-8 boundary, both at the next build.
    /// Calling before the field's first-ever frame works (the state waits in
    /// the retained store until the field appears). While an IME preedit is
    /// active the field manages its own caret, so host writes then have no
    /// visible effect.
    pub fn textfield_set_caret(&mut self, label: &str, caret: u32) {
        let c = cstr(label);
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_textfield_set_caret(self.ui, c.as_ptr(), caret) };
    }

    /// Like [`Self::textfield_set_caret`], but sets a selection: the anchor at
    /// `anchor` and the caret at `caret` (both byte offsets; selecting
    /// backwards is fine). Select-all is `anchor = 0, caret = u32::MAX` — the
    /// caret clamps to the buffer length at the next build.
    pub fn textfield_set_selection(&mut self, label: &str, anchor: u32, caret: u32) {
        let c = cstr(label);
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_textfield_set_selection(self.ui, c.as_ptr(), anchor, caret) };
    }

    /// Like [`Self::textfield_set_selection`] but for a scoped field — the
    /// `id` must match what was passed to [`Self::push_id`] around the
    /// field's build.
    pub fn textfield_scoped_set_selection(
        &mut self,
        id: &str,
        label: &str,
        anchor: u32,
        caret: u32,
    ) {
        self.push_id(id);
        self.textfield_set_selection(label, anchor, caret);
        self.pop_id();
    }

    /// A multi-line text editor with a minimum visible height.
    pub fn textarea(&mut self, label: &str, buf: &mut TextBuf, min_height: f32) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and buf outlive the call; buf is NUL-terminated
        // with capacity buf.cap().
        unsafe {
            sys::lens_textarea(
                self.ui,
                c.as_ptr(),
                buf.as_mut_ptr(),
                buf.cap(),
                min_height.max(0.0),
            )
        }
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

/// Build a `CString`, panicking on an interior NUL (a programming error in the
/// caller's literal, not a runtime condition).
fn cstr(s: &str) -> CString {
    CString::new(s).expect("string passed to a widget contains an interior NUL")
}
