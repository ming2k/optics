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

pub use input::{key, mods, Input, MouseButton};
pub use types::{
    Align, Color, Icon, LayoutOpts, OverlayOpts, Rect, Response, TableColumn, TableOpts,
    TableResult, Theme,
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
        let rc = sys::lens_render(self.raw, canvas);
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

    // ---- overlays (host-side open/close, ADR-0014) -----------------------
    //
    // `Frame::overlay_open` / `Frame::overlay_close` / `Frame::overlay` cover
    // the common case of driving an overlay from inside a frame. These mirror
    // the same calls on `Ui` so a host can open or dismiss an overlay outside
    // a frame — e.g. from a keyboard handler that runs between frames. The
    // state is retained per id on the context, so a `close` here is visible
    // to the next frame's `Frame::overlay` / `Frame::overlay_is_open`.

    /// Open the overlay keyed by `id` (retained until closed). Safe to call
    /// outside a frame; the body renders on the next [`Ui::frame`].
    pub fn overlay_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::lens_overlay_open(self.raw, c.as_ptr()) };
    }

    /// Close the overlay keyed by `id`. Safe to call outside a frame.
    pub fn overlay_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call.
        unsafe { sys::lens_overlay_close(self.raw, c.as_ptr()) };
    }

    /// Whether the overlay keyed by `id` is currently open. Safe to call
    /// outside a frame.
    pub fn overlay_is_open(&mut self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: raw is live; c outlives the call; read-only.
        unsafe { sys::lens_overlay_is_open(self.raw as *const sys::lens, c.as_ptr()) }
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

    /// Draw an icon glyph at `size` logical pixels.
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

    /// A flat icon-only button for navigation strips and toolbars: transparent
    /// at rest, with a subtle fill on hover. Returns `true` on the frame it is
    /// clicked.
    pub fn icon_button(&mut self, id: Icon) -> bool {
        // SAFETY: ui is live for the frame.
        unsafe { sys::lens_icon_button(self.ui, id.raw()) }
    }

    /// As [`Frame::icon_button`], but `active` marks the currently-selected
    /// view in a navigation strip. The visual treatment is theme-driven:
    /// always a background tint, plus an opt-in left accent rail and
    /// accent-tinted glyph when
    /// [`Theme::with_active_indicator_width`](crate::Theme::with_active_indicator_width)
    /// is > 0. The default is the tint-only active state. Returns `true` on
    /// the frame it is clicked.
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
            let value = (state.cell)(row as usize, column as usize).replace('\0', "�");
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
                },
            )
        };
        TableResult {
            selected: usize::try_from(raw.selected).ok(),
            selection_changed: raw.selection_changed,
            clicked: raw.clicked,
        }
    }

    /// A tab strip keyed by `id`, tracking the selected index in `active`.
    /// The strip enforces the themed label height and uses its underline for
    /// both selection and keyboard focus instead of drawing a detached frame.
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

    /// Declare one tab inside a [`Frame::tabs`] strip. Returns `true` when this
    /// tab is the active one.
    pub fn tab(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_tab(self.ui, c.as_ptr()) }
    }

    /// A dropdown selecting an index into `items`, stored in `selected`.
    /// Its vector chevron stays on the trailing edge and the floating option
    /// list owns an opaque surface. In a scroll area the list inherits the
    /// viewport and closes when scrolling starts. Returns `true` when
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

    // ---- queries & overlays -----------------------------------------------

    /// The interaction result of the most recently built widget — useful for an
    /// overlay anchor (`f.response().rect`) or reading hover/click after the
    /// fact.
    pub fn response(&self) -> Response {
        // SAFETY: ui is live; the call only reads state.
        Response::from_raw(unsafe { sys::lens_get_response(self.ui as *const sys::lens) })
    }

    /// Open the overlay keyed by `id` (retained until closed).
    pub fn overlay_open(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_overlay_open(self.ui, c.as_ptr()) };
    }

    /// Close the overlay keyed by `id`.
    pub fn overlay_close(&mut self, id: &str) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_overlay_close(self.ui, c.as_ptr()) };
    }

    /// Whether the overlay keyed by `id` is currently open.
    pub fn overlay_is_open(&self, id: &str) -> bool {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call; read-only.
        unsafe { sys::lens_overlay_is_open(self.ui as *const sys::lens, c.as_ptr()) }
    }

    /// A floating overlay layer anchored to `anchor` (usually the owning
    /// widget's `response().rect`). `body` runs only while the overlay is open;
    /// the closure is skipped otherwise.
    pub fn overlay(
        &mut self,
        id: &str,
        anchor: Rect,
        opts: &OverlayOpts,
        body: impl FnOnce(&mut Frame),
    ) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call.
        let open =
            unsafe { sys::lens_overlay_begin(self.ui, c.as_ptr(), anchor.to_raw(), opts.to_raw()) };
        if open {
            body(self);
            // SAFETY: only paired with a true return from lens_overlay_begin.
            unsafe { sys::lens_overlay_end(self.ui) };
        }
    }

    /// A persistent floating layer placed exactly at `rect`. Unlike
    /// [`Frame::overlay`], a layer is always entered (no open/close state) and
    /// is never auto-dismissed: use it for chrome that stays on screen every
    /// frame — a dock, a status bar, a notification stack, per-window title
    /// bars. `rect.w`/`rect.h` are a minimum; the layer grows to fit its body.
    pub fn layer(
        &mut self,
        id: &str,
        rect: Rect,
        opts: &OverlayOpts,
        body: impl FnOnce(&mut Frame),
    ) {
        let c = cstr(id);
        // SAFETY: ui is live; c outlives the call. lens_layer_begin always
        // returns true, so the body always builds and lens_layer_end is always
        // paired.
        unsafe { sys::lens_layer_begin(self.ui, c.as_ptr(), rect.to_raw(), opts.to_raw()) };
        body(self);
        unsafe { sys::lens_layer_end(self.ui) };
    }

    // ---- widgets ----------------------------------------------------------

    /// A button. Returns `true` on the frame it is clicked.
    pub fn button(&mut self, label: &str) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c outlives the call.
        unsafe { sys::lens_button(self.ui, c.as_ptr()) }
    }

    /// A plain, borderless, full-width list or navigation row. It is
    /// transparent at rest, uses a subtle fill on hover, and uses the theme's
    /// active-surface colour when `selected`. Independently, themes can opt
    /// into an accent rail and accent text through
    /// [`Theme::with_active_indicator_width`](crate::Theme::with_active_indicator_width).
    /// Returns `true` on the frame it is clicked.
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

    /// A slider bound to `value`, clamped to `[min, max]`. Its knob stays
    /// hidden at rest and animates in for hover, keyboard focus, or dragging.
    /// Returns `true` on the frame the value changes.
    pub fn slider(&mut self, label: &str, value: &mut f32, min: f32, max: f32) -> bool {
        let c = cstr(label);
        // SAFETY: ui is live; c and value outlive the call.
        unsafe { sys::lens_slider(self.ui, c.as_ptr(), value as *mut f32, min, max) }
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
    /// keeps its cursor clamped to the new length on the next frame.
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
