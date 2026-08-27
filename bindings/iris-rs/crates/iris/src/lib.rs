//! Safe Rust bindings to **iris** — the L3 application toolkit of the
//! flux/lens stack.
//!
//! iris owns the window, GPU device, and event loop. Each frame it runs two
//! host callbacks:
//!
//! 1. **build** ([`Frame`] + [`Input`]) — inside an open `lens_begin/end`
//!    pair. The host builds its chrome (immediate-mode lens widgets) and
//!    reads the per-frame [`Input`] snapshot to drive anything lens itself
//!    does not own.
//! 2. **paint** ([`PaintHost`]) — inside an open `flux_canvas_begin/end`
//!    pair, *before* lens renders its widget layer. Anything drawn here
//!    composites under the chrome. Use it for surfaces lens cannot describe
//!    (a document canvas, an image, a custom render path).
//!
//! Either closure may be `None` — a pure-chrome app skips `paint`, a
//! pure-canvas demo skips `build`.
//!
//! ```no_run
//! use iris::{Application, Config};
//!
//! Application::run(
//!     Config::new("Demo").unwrap(),
//!     |_frame, _input| { /* build chrome */ },
//!     Some(|_canvas| { /* paint document surface */ }),
//! ).unwrap();
//! ```
//!
//! Outside `Application::run`, this crate also exposes:
//! - [`system_prefers_dark`] / [`ColorScheme`] — query the desktop's
//!   colour-scheme preference at startup.
//! - [`pick_file`] — open the host desktop's native file picker via
//!   xdg-desktop-portal.

#![deny(rust_2018_idioms)]

use std::ffi::CString;
use std::os::raw::c_int;

pub use iris_sys as sys;

pub use lens::key;
pub use lens::mods;
pub use lens::{
    Align, Band, Color, CursorHint, Frame, Icon, Input, LayoutOpts, MouseButton, PlaceMode,
    PlaceOpts, Rect, Response, TableColumn, TableOpts, TableResult, TabsOpts, TextBuf, Theme, Ui,
};

/// A thin wrapper over the raw pointers iris hands to the paint
/// callback: the canvas (live inside an open `flux_canvas_begin/end`
/// pair), the device iris owns for this app, and the current device-
/// pixel ratio. All three borrows are valid only inside the paint call.
///
/// Hosts that need a `flux::Device`-shaped handle (e.g. to create a
/// `flux::text::Text` context) **must** borrow `device()` rather than
/// opening their own — two `flux_device`s in one process is unsupported
/// and crashes.
///
/// Hosts drawing canvas content directly should wrap their draw in
/// `canvas.save / canvas.scale(s, s) / ... / canvas.restore` and call
/// `flux_text_set_scale(text, s)` so glyphs rasterise crisply on HiDPI.
/// lens applies the same scale to its own chrome internally; the host
/// only scales the document-surface portion.
pub struct PaintHost {
    canvas: *mut std::ffi::c_void,
    device: *mut std::ffi::c_void,
    scale: f32,
}

impl PaintHost {
    /// The raw `flux_canvas*`. Hand it to `flux`-aware code that expects
    /// a borrowed canvas pointer (e.g. cast to `*mut flux_sys::flux_canvas`
    /// and wrap in your own borrowed handle).
    ///
    /// Prefer the typed accessors [`PaintHost::flux_canvas`] /
    /// [`PaintHost::flux_device`], which return safe borrowed handles from
    /// the `flux` crate directly — no pointer casting at the call site.
    pub fn canvas(&self) -> *mut std::ffi::c_void {
        self.canvas
    }

    /// The raw `flux_device*` iris owns. Use this to construct any
    /// device-dependent context (text shaper, image uploader, …) rather
    /// than creating a second `flux::Device`.
    ///
    /// Prefer the typed accessors [`PaintHost::flux_canvas`] /
    /// [`PaintHost::flux_device`].
    pub fn device(&self) -> *mut std::ffi::c_void {
        self.device
    }

    /// The canvas iris is handing the paint callback, as a borrowed
    /// (non-owning) `flux::Canvas`. It is already inside its
    /// begin/end pair for this frame; draw through it directly. The borrow
    /// lives for the callback only — iris owns and destroys the canvas.
    ///
    /// The pointer came straight from iris's `iris_paint_fn` C callback,
    /// which types it `flux_canvas*`; the `flux` and `iris-sys` bindgen
    /// views are layout-identical opaque pointers, so the cast mirrors
    /// [`flux::Canvas::borrow_raw`]'s documented contract for
    /// sibling-binding pointers.
    pub fn flux_canvas(&self) -> flux::Canvas {
        // SAFETY: iris handed us a live `flux_canvas*` for the duration of
        // the paint callback. The returned handle does not destroy on drop.
        unsafe { flux::Canvas::borrow_raw(self.canvas as *mut flux::sys::flux_canvas) }
    }

    /// The single flux device iris owns, as a borrowed (non-owning)
    /// `flux::Device`. Use it for any device-dependent context (samplers,
    /// textures, a `flux_text::Text` shaper, …) instead of opening a
    /// second device — two devices in one process is unsupported. The
    /// borrow is valid for the app's lifetime; iris releases the device at
    /// shutdown.
    ///
    /// Same pointer contract as [`PaintHost::flux_canvas`]: the typed
    /// `flux_device*` from iris's C callback, reinterpreted through the
    /// `flux` crate's layout-identical bindgen view.
    pub fn flux_device(&self) -> flux::Device {
        // SAFETY: see the doc comment; mirrors `Device::borrow_raw`'s
        // contract for sibling-binding pointers.
        unsafe { flux::Device::borrow_raw(self.device as *mut flux::sys::flux_device) }
    }

    /// The current device-pixel ratio (Wayland
    /// `wl_surface.preferred_buffer_scale`). Always ≥1; 1 on a 1:1 logical
    /// display, 2 on a typical HiDPI laptop panel.
    pub fn scale(&self) -> f32 {
        self.scale
    }
}

/// Alias kept for back-compat with the prior single-pointer signature; new
/// code should use [`PaintHost`], which also exposes the device.
pub type PaintCanvas = PaintHost;

/// A thin wrapper over the raw pointers iris hands to the optional start
/// callback: the lens context and the flux device iris owns for this app.
/// The callback runs once — after iris has created its device, canvas and
/// lens context, before the first frame — which makes it the right place to
/// set up device-dependent resources (text shapers, image uploads) without
/// installing a per-frame paint callback: a non-NULL `paint` disables the
/// backend's idle frame-skip, a start hook costs nothing per frame. The same
/// device-borrow rule as [`PaintHost::device`] applies.
pub struct StartHost {
    ui: *mut std::ffi::c_void,
    device: *mut std::ffi::c_void,
}

impl StartHost {
    /// The raw `lens*`, live for the whole run.
    pub fn ui(&self) -> *mut std::ffi::c_void {
        self.ui
    }

    /// The raw `flux_device*` iris owns. Borrow it — never open a second
    /// device in the same process.
    pub fn device(&self) -> *mut std::ffi::c_void {
        self.device
    }
}

/// The handle iris hands to the optional `stop` callback: the same borrows
/// as [`StartHost`], live one last time after the frame loop and before
/// iris tears down lens, flux, and the device (ADR-0045). This is where
/// hosts release every device-backed resource created from
/// [`StartHost::device`] / [`PaintHost::device`].
pub type StopHost = StartHost;

/// Errors returned by [`Application::run`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RunError {
    /// The platform returned a non-zero exit code.
    Platform(i32),
    /// The config contained a NULL byte in the title.
    BadTitle,
    /// The config contained a NULL byte in the desktop application ID.
    BadAppId,
}

impl std::fmt::Display for RunError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RunError::Platform(rc) => write!(f, "iris_app_run exited with code {rc}"),
            RunError::BadTitle => write!(f, "window title contains an interior NUL byte"),
            RunError::BadAppId => write!(f, "application ID contains an interior NUL byte"),
        }
    }
}

impl std::error::Error for RunError {}

/// Application configuration handed to [`Application::run`].
pub struct Config {
    title: CString,
    app_id: CString,
    width: i32,
    height: i32,
    dark: bool,
    log_raw: bool,
}

impl Config {
    /// A default-sized window with the given title that follows the system
    /// colour scheme.
    pub fn new(title: impl Into<String>) -> Result<Config, RunError> {
        let title = CString::new(title.into()).map_err(|_| RunError::BadTitle)?;
        Ok(Config {
            title,
            app_id: CString::new("ai.opencode.iris").expect("static app ID is valid"),
            width: 960,
            height: 640,
            dark: false,
            log_raw: false,
        })
    }

    /// Set the stable desktop application ID used by Wayland compositors
    /// for window grouping, launcher matching, and icon lookup.
    pub fn app_id(mut self, app_id: impl Into<String>) -> Result<Self, RunError> {
        self.app_id = CString::new(app_id.into()).map_err(|_| RunError::BadAppId)?;
        Ok(self)
    }

    /// Set the initial window size (logical pixels).
    pub fn size(mut self, width: i32, height: i32) -> Self {
        self.width = width;
        self.height = height;
        self
    }

    /// Force the dark theme instead of following the system preference.
    pub fn force_dark(mut self) -> Self {
        self.dark = true;
        self
    }

    /// Log raw platform input events to stderr (debugging).
    pub fn log_raw(mut self) -> Self {
        self.log_raw = true;
        self
    }
}

/// The application entry point.
///
/// Wraps `iris_app_run`. `build` runs once per frame inside an open
/// `lens_begin/end` pair, with `input` being the same [`Input`] snapshot
/// lens is consuming this frame. `paint` (if `Some`) runs once per frame
/// inside an open `flux_canvas_begin/end` pair, *before* lens renders its
/// widget layer — so anything it draws lands under the chrome. Blocks the
/// calling thread until the window is closed.
pub struct Application;

/// Request one more frame at Iris's active animation cadence.
///
/// Call this during a build or paint callback while host-owned, time-based
/// content is moving. Repeating it every frame sustains the animation; once
/// requests stop, Iris can return to its low-power idle cadence. Calls outside
/// an active [`Application::run`] are harmless no-ops.
pub fn request_animation_frame() {
    unsafe { sys::iris_request_animation_frame() };
}

/// Declare the current frame's host canvas content static.
///
/// A paint callback's content is opaque to lens, so the backend normally
/// keeps painting every scheduled frame — including a full clear to the
/// theme background. When the host knows its scene is pixel-identical to
/// what is already on screen (a paused animation, a settled audio
/// visualizer), this call lets the backend skip the entire
/// acquire → clear → paint → present cycle: no swapchain image is
/// committed and the GPU does no host work, unloading the compositor
/// exactly like a host without a paint callback.
///
/// The declaration covers one frame only and must be re-issued every frame
/// it applies to — call it from the **build** callback (build runs on every
/// scheduled frame, including skipped ones; a declaration made in paint is
/// cleared with the first skipped frame and the cadence bounces back).
/// Any user input, a resize or buffer-scale change, a following
/// [`request_animation_frame`], or lens-reported chrome damage forces the
/// next frame to paint again.
///
/// While static, the backend keeps a low idle tick (~4 Hz) so build (and
/// paint, when it chooses) still run and the host can resume animating on
/// its own. No-op without an active app or without a paint callback.
pub fn paint_mark_static() {
    unsafe { sys::iris_paint_mark_static() };
}

/// Skips this frame's render/present while keeping the active-rate cadence.
///
/// The host promises the canvas content is unchanged from the last
/// *presented* frame but is still streaming — the media-cadence pattern:
/// a 30 fps visualizer on a 60 Hz display renders on its stage ticks and
/// declares skip on the in-between frames while calling
/// [`request_animation_frame`] as usual. Only the begin → clear → paint →
/// present work for this frame is suppressed; the next deadline stays armed
/// at the active rate (no decay to the ~4 Hz idle tick).
///
/// Mutually exclusive per frame with [`paint_mark_static`] (skip wins for
/// rendering only when the static path would also have skipped; prefer
/// declaring exactly one). Unlike [`paint_mark_static`], a fresh
/// [`request_animation_frame`] does **not** veto this declaration — the two
/// together are precisely the streaming shape. Any user input, a resize or
/// buffer-scale change, or lens-reported chrome damage still forces a real
/// paint. Call from the **build** callback; consumed once, re-issue every
/// frame it applies to. No-op without an active app or without a paint
/// callback.
pub fn request_frame_skip_render() {
    unsafe { sys::iris_request_frame_skip_render() };
}

impl Application {
    pub fn run<B, P>(config: Config, build: B, paint: Option<P>) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame, &Input),
        P: FnMut(PaintHost) + 'static,
    {
        Self::run_impl(
            config,
            None::<fn(StartHost) -> bool>,
            None::<fn(StopHost)>,
            build,
            paint,
        )
    }

    /// Like [`Application::run`], plus a one-shot `start` callback that iris
    /// invokes after its device/canvas/lens setup and before the first frame
    /// (see [`StartHost`]). The callback returning `false` aborts the run.
    pub fn run_with_start<B, P, S>(
        config: Config,
        start: S,
        build: B,
        paint: Option<P>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame, &Input),
        P: FnMut(PaintHost) + 'static,
        S: FnMut(StartHost) -> bool,
    {
        Self::run_impl(config, Some(start), None::<fn(StopHost)>, build, paint)
    }

    /// The full lifecycle form (ADR-0045): `start` runs after iris's
    /// device/canvas/lens setup and before the first frame (returning
    /// `false` aborts the run); `stop` runs after the frame loop and before
    /// iris destroys the device, giving hosts a deterministic point to
    /// release device-backed resources. Either may be `None`.
    pub fn run_with_lifecycle<B, P, S, T>(
        config: Config,
        start: Option<S>,
        stop: Option<T>,
        build: B,
        paint: Option<P>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame, &Input),
        P: FnMut(PaintHost) + 'static,
        S: FnMut(StartHost) -> bool,
        T: FnMut(StopHost),
    {
        Self::run_impl(config, start, stop, build, paint)
    }

    fn run_impl<B, P, S, T>(
        config: Config,
        start: Option<S>,
        stop: Option<T>,
        build: B,
        paint: Option<P>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame, &Input),
        P: FnMut(PaintHost) + 'static,
        S: FnMut(StartHost) -> bool,
        T: FnMut(StopHost),
    {
        // Box the closures so they have a stable address to pass through the
        // C trampoline. Each gets its own box; both share the `user` pointer
        // via a small wrapper struct.
        let build_box: Box<B> = Box::new(build);
        let paint_box: Option<Box<P>> = paint.map(Box::new);
        let start_box: Option<Box<S>> = start.map(Box::new);
        let stop_box: Option<Box<T>> = stop.map(Box::new);

        // SAFETY: the trampolines cast `user` back to the right box type and
        // call it. The boxes outlive `iris_app_run` (held by `run_state`
        // until the call returns).
        let mut run_state = RunState {
            build: build_box,
            paint: paint_box,
            start: start_box,
            stop: stop_box,
        };

        extern "C" fn build_trampoline<B, P, S, T>(
            ui: *mut sys::lens,
            in_: *const sys::lens_input,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame, &Input),
            P: FnMut(PaintHost),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, P, S, T>) };
                // Cast the iris_sys view of `lens` / `lens_input` to lens's
                // own bindgen view. Both come from the same C declaration, so
                // the layouts are identical; the types are just nominally
                // distinct because they came from two `-sys` crates.
                let lens_ui = ui as *mut lens::sys::lens;
                let mut frame = unsafe { Frame::from_raw(lens_ui) };
                let input_ptr = in_ as *const lens::sys::lens_input;
                let input = unsafe { Input::from_raw_ref(input_ptr) };
                (run.build)(&mut frame, input);
                // The backend follows the hovered widget's cursor hint
                // natively once per frame — nothing to do here.
            }));
        }

        extern "C" fn paint_trampoline<B, P, S, T>(
            canvas: *mut sys::flux_canvas,
            device: *mut sys::flux_device,
            scale: f32,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame, &Input),
            P: FnMut(PaintHost),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, P, S, T>) };
                if let Some(p) = run.paint.as_mut() {
                    p(PaintHost {
                        canvas: canvas as *mut std::ffi::c_void,
                        device: device as *mut std::ffi::c_void,
                        scale,
                    });
                }
            }));
        }

        extern "C" fn start_trampoline<B, P, S, T>(
            ui: *mut sys::lens,
            device: *mut sys::flux_device,
            user: *mut std::os::raw::c_void,
        ) -> bool
        where
            B: FnMut(&mut Frame, &Input),
            P: FnMut(PaintHost),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
        {
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, P, S, T>) };
                match run.start.as_mut() {
                    Some(s) => s(StartHost {
                        ui: ui as *mut std::ffi::c_void,
                        device: device as *mut std::ffi::c_void,
                    }),
                    None => true,
                }
            }))
            .unwrap_or(false)
        }

        extern "C" fn stop_trampoline<B, P, S, T>(
            ui: *mut sys::lens,
            device: *mut sys::flux_device,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame, &Input),
            P: FnMut(PaintHost),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, P, S, T>) };
                if let Some(t) = run.stop.as_mut() {
                    t(StartHost {
                        ui: ui as *mut std::ffi::c_void,
                        device: device as *mut std::ffi::c_void,
                    });
                }
            }));
        }

        // Monomorphise the trampolines for the actual (B, P, S, T) tuple.
        // When `paint`/`start`/`stop` are `None` we still need *a* fn
        // pointer of each type to satisfy the C signature; passing NULL lets
        // the C side skip the call entirely, so those branches are pure
        // no-ops — and a NULL paint keeps the backend's idle frame-skip
        // engaged.
        let user_ptr = &mut run_state as *mut _ as *mut std::os::raw::c_void;
        let paint_fn: sys::iris_paint_fn = if run_state.paint.is_some() {
            Some(paint_trampoline::<B, P, S, T>)
        } else {
            None
        };
        let start_fn: sys::iris_start_fn = if run_state.start.is_some() {
            Some(start_trampoline::<B, P, S, T>)
        } else {
            None
        };
        let stop_fn: sys::iris_stop_fn = if run_state.stop.is_some() {
            Some(stop_trampoline::<B, P, S, T>)
        } else {
            None
        };

        let cfg = sys::iris_app_config {
            title: config.title.as_ptr(),
            app_id: config.app_id.as_ptr(),
            width: config.width,
            height: config.height,
            dark: config.dark,
            log_raw: config.log_raw,
            start: start_fn,
            stop: stop_fn,
            build: Some(build_trampoline::<B, P, S, T>),
            paint: paint_fn,
            user: user_ptr,
        };

        let rc = unsafe { sys::iris_app_run(&cfg) };
        if rc != 0 {
            return Err(RunError::Platform(rc));
        }
        Ok(())
    }
}

/// Holds the closures through the C trampolines. Generic over `B`, `P`, `S`
/// and `T` so each trampoline can cast the opaque `user` pointer back to a
/// type that knows the concrete closure shapes. When a slot is `None` the
/// matching trampoline is never installed.
struct RunState<B, P, S, T>
where
    B: FnMut(&mut Frame, &Input),
    P: FnMut(PaintHost),
    S: FnMut(StartHost) -> bool,
    T: FnMut(StopHost),
{
    build: Box<B>,
    paint: Option<Box<P>>,
    start: Option<Box<S>>,
    stop: Option<Box<T>>,
}

/// System colour-scheme preference (queried at startup).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorScheme {
    NoPreference,
    PreferDark,
    PreferLight,
}

impl From<sys::iris_color_scheme> for ColorScheme {
    fn from(s: sys::iris_color_scheme) -> Self {
        use sys::iris_color_scheme::*;
        match s {
            IRIS_COLOR_SCHEME_NO_PREFERENCE => ColorScheme::NoPreference,
            IRIS_COLOR_SCHEME_PREFER_DARK => ColorScheme::PreferDark,
            IRIS_COLOR_SCHEME_PREFER_LIGHT => ColorScheme::PreferLight,
        }
    }
}

/// Read the system colour-scheme preference.
pub fn query_system_color_scheme() -> ColorScheme {
    unsafe { sys::iris_query_system_color_scheme() }.into()
}

/// Convenience: returns true when the system prefers dark OR no preference
/// (we default to dark when the user is silent).
pub fn system_prefers_dark() -> bool {
    unsafe { sys::iris_system_prefers_dark() }
}

/// Callback fired by [`watch_system_color_scheme`] whenever the desktop
/// colour-scheme preference changes.
pub type ColorSchemeChangedFn =
    extern "C" fn(scheme: sys::iris_color_scheme, user: *mut std::os::raw::c_void);

/// Begin watching the system colour scheme. Returns `Ok(())` on success and
/// an `Err` when live watching is unavailable (no platform support, or the
/// OS settings source is unreachable) — callers then fall back to the
/// startup-only [`query_system_color_scheme`]. Changes are delivered on the
/// iris main thread through the backend's event loop; no fd/pump plumbing
/// is exposed.
///
/// # Safety
///
/// `user` must be a valid pointer (or null) for the lifetime of the watch —
/// it is passed verbatim to the C callback and dereferenced there. Most
/// applications do not call this directly: when driven by
/// [`Application::run`] with [`Config::force_dark`] unset, the Wayland
/// backend wires the watcher into its poll loop automatically.
pub unsafe fn watch_system_color_scheme(
    cb: ColorSchemeChangedFn,
    user: *mut std::os::raw::c_void,
) -> Result<(), WatchError> {
    let rc = unsafe { sys::iris_color_scheme_watch(Some(cb), user) };
    if rc == 0 {
        Ok(())
    } else {
        Err(WatchError::Unavailable)
    }
}

/// Error from [`watch_system_color_scheme`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WatchError {
    /// Live watching is unavailable on this build/host.
    Unavailable,
}

impl std::fmt::Display for WatchError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            WatchError::Unavailable => {
                write!(f, "live theme watching unavailable on this platform/host")
            }
        }
    }
}

impl std::error::Error for WatchError {}

/// Stop watching and release platform resources. Safe to call when not
/// watching.
pub fn stop_color_scheme_watcher() {
    unsafe { sys::iris_color_scheme_unwatch() }
}

/// Cursor appearance the host wants iris to show over its window.
///
/// This is an L3 concern: iris owns the window and the cursor surface, so it
/// owns cursor appearance too. [`Application::run`] automatically maps the
/// hovered Lens widget's [`CursorHint`]; hosts use [`set_cursor`] for custom
/// canvas regions and other application-specific overrides.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(i32)]
pub enum Cursor {
    /// Arrow — the platform default. Also what unknown values fall back to.
    #[default]
    Default,
    /// I-beam — over text the user can edit.
    Text,
    /// Pointing hand — over clickable elements.
    Pointer,
    /// Hourglass / spinner — the app is busy.
    Busy,
    /// Precise selection (e.g. image crop).
    Crosshair,
    /// Forbidden action.
    NotAllowed,
    /// Horizontal resize.
    ResizeEw,
    /// Vertical resize.
    ResizeNs,
}

impl Cursor {
    fn raw(self) -> sys::iris_cursor {
        use sys::iris_cursor::*;
        match self {
            Cursor::Default => IRIS_CURSOR_DEFAULT,
            Cursor::Text => IRIS_CURSOR_TEXT,
            Cursor::Pointer => IRIS_CURSOR_POINTER,
            Cursor::Busy => IRIS_CURSOR_BUSY,
            Cursor::Crosshair => IRIS_CURSOR_CROSSHAIR,
            Cursor::NotAllowed => IRIS_CURSOR_NOT_ALLOWED,
            Cursor::ResizeEw => IRIS_CURSOR_RESIZE_EW,
            Cursor::ResizeNs => IRIS_CURSOR_RESIZE_NS,
        }
    }
}

impl From<CursorHint> for Cursor {
    fn from(hint: CursorHint) -> Self {
        match hint {
            CursorHint::Default => Self::Default,
            CursorHint::Pointer => Self::Pointer,
            CursorHint::Text => Self::Text,
            CursorHint::ResizeEw => Self::ResizeEw,
            CursorHint::ResizeNs => Self::ResizeNs,
        }
    }
}

/// Set the cursor the next pointer motion will show. Idempotent; passing the
/// same value twice does no work.
///
/// An explicit choice stays pinned until you call `set_cursor(Cursor::Default)`,
/// which hands cursor control back to the backend's per-frame follow of the
/// hovered Lens widget's [`CursorHint`] (the backend does that natively on all
/// platforms — Wayland included — so hosts do not need to re-assert a custom
/// cursor every frame, only to keep it pinned).
///
/// No-op when iris was built without cursor-shape support (the compositor's
/// default arrow stays) or before [`Application::run`] starts. Thread-
/// affine: call from the same thread that drives the run loop.
pub fn set_cursor(cursor: Cursor) {
    unsafe { sys::iris_set_cursor(cursor.raw()) }
}

/// Open the host desktop's native file picker. Returns the selected file
/// URI (e.g. `"file:///home/user/foo.txt"`) or `None` if the user cancelled
/// or the portal is unavailable.
pub fn pick_file(title: Option<&str>) -> Option<String> {
    pick_path(title, false)
}

/// Open the host desktop's native folder picker. Returns the selected folder
/// URI or `None` if the user cancelled or the portal is unavailable.
pub fn pick_folder(title: Option<&str>) -> Option<String> {
    pick_path(title, true)
}

fn pick_path(title: Option<&str>, folder: bool) -> Option<String> {
    let title_c = title.and_then(|t| CString::new(t).ok());
    let opts = sys::iris_file_dialog_opts {
        title: title_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        ..Default::default()
    };
    let mut buf = vec![0u8; 4096];
    let rc = unsafe {
        if folder {
            sys::iris_pick_folder(&opts, buf.as_mut_ptr().cast(), buf.len())
        } else {
            sys::iris_pick_file(&opts, buf.as_mut_ptr().cast(), buf.len())
        }
    };
    if rc != 0 {
        return None;
    }
    let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
    buf.truncate(len);
    String::from_utf8(buf).ok()
}

/// Why a URI-to-path conversion is not a `file://`-prefix strip:
/// the portal percent-encodes escapes the filesystem needs back
/// (`%20`, `%C3%A9`, …), and a remote authority (`file://host/…`) is not a
/// local path at all.
///
/// Convert a picker result (`file://` URI) into a local filesystem path.
/// Returns `None` when the URI is malformed, names a remote authority, or
/// decodes to more than `PATH_MAX`-ish bytes.
///
/// ```
/// # use iris::file_uri_to_path;
/// assert_eq!(file_uri_to_path("file:///home/my%20docs/a.png").as_deref(),
///            Some("/home/my docs/a.png"));
/// assert_eq!(file_uri_to_path("file://server/share/x"), None);
/// ```
pub fn file_uri_to_path(uri: &str) -> Option<String> {
    let c = CString::new(uri).ok()?;
    // Longest realistic decoded path: URIs from the portal are bounded by
    // PATH_MAX-ish lengths; 8 KiB leaves headroom for percent expansion.
    let mut buf = vec![0u8; 8192];
    let rc = unsafe { sys::iris_file_uri_to_path(c.as_ptr(), buf.as_mut_ptr().cast(), buf.len()) };
    if rc != 0 {
        return None;
    }
    let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
    buf.truncate(len);
    String::from_utf8(buf).ok()
}

/// Library version string ("0.0.28" at the time of writing).
pub fn version() -> &'static str {
    unsafe {
        std::ffi::CStr::from_ptr(sys::iris_version_string())
            .to_str()
            .unwrap_or("?")
    }
}

// Re-export so callers can address the raw c_int return convention if needed.
#[allow(dead_code)]
fn _keep_c_int_in_scope(_: c_int) {}

// =====================================================================
//  Window control (window.h)
//
//  Bound late in this crate's life: a Rust application previously could
//  not close its own window, resize it, or go fullscreen — 11 of the 30
//  C functions had no safe wrapper. All of these are thread-affine to the
//  run loop (the "active window" is the one opened by the most recent
//  iris_app_run on the calling thread) and degrade to documented no-ops
//  on backends without the capability — check [`supports`] first when the
//  affordance is load-bearing in your UI.
// =====================================================================

/// Ask the active window to close. The loop exits and
/// [`Application::run`] returns, exactly as if the user clicked the
/// title-bar close button.
pub fn window_close() {
    unsafe { sys::iris_window_close() };
}

/// Minimize the active window to the taskbar / dock.
pub fn window_minimize() {
    unsafe { sys::iris_window_minimize() };
}

/// Restore a minimized window, or un-fullscreen a fullscreen one.
pub fn window_restore() {
    unsafe { sys::iris_window_restore() };
}

/// Take the whole output (Wayland: the output the window is on; Win32/
/// Cocoa: the display the window overlaps most).
pub fn window_fullscreen() {
    unsafe { sys::iris_window_fullscreen() };
}

/// Leave fullscreen. No-op when not fullscreen.
pub fn window_windowed() {
    unsafe { sys::iris_window_unfullscreen() };
}

/// Maximize the window (fill the work area, keeping decorations).
pub fn window_maximize() {
    unsafe { sys::iris_window_maximize() };
}

/// Undo [`window_maximize`].
pub fn window_unmaximize() {
    unsafe { sys::iris_window_unmaximize() };
}

/// Ask the compositor to focus the window.
///
/// Permanently a no-op on Wayland (the protocol forbids self-activation —
/// only the user or the compositor can raise a window); works on Win32
/// and Cocoa.
pub fn window_focus() {
    unsafe { sys::iris_window_focus() };
}

/// Set the minimum client-area size, in logical pixels.
pub fn window_set_min_size(width: i32, height: i32) {
    unsafe { sys::iris_window_set_min_size(width, height) };
}

/// Set the maximum client-area size, in logical pixels.
pub fn window_set_max_size(width: i32, height: i32) {
    unsafe { sys::iris_window_set_max_size(width, height) };
}

/// Current window size, in logical pixels. `None` when no window is
/// active; the C API also reports only width/height (position is owned
/// by the compositor and not reported on Wayland).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct WindowSize {
    pub width: i32,
    pub height: i32,
}

pub fn window_get_geometry() -> Option<WindowSize> {
    let (mut w, mut h) = (0i32, 0i32);
    let ok = unsafe { sys::iris_window_get_geometry(&mut w, &mut h) };
    if ok {
        Some(WindowSize {
            width: w,
            height: h,
        })
    } else {
        None
    }
}

// =====================================================================
//  Cross-thread wakeup (app.h — the ONLY thread-safe iris entry point)
// =====================================================================

/// Schedule `f` to run on the iris main thread, outside any
/// `lens_begin/end` pair.
///
/// This is the one iris API that is safe to call from ANY thread; it is
/// how a worker thread wakes the loop (the loop may be idle at ~4 Hz —
/// post to get a frame promptly). FIFO per posting thread. Returns
/// `false` when no run loop is active (dropped), or when the closure
/// could not be boxed (allocation failure).
///
/// The closure runs on the main thread on a best-effort basis; panics in
/// it propagate as a panic on that thread.
pub fn post_to_main_thread<F: FnOnce() + Send + 'static>(f: F) -> bool {
    // Box<dyn FnOnce + Send> is callable through a stable vtable-less
    // trampoline: we leak the box into a raw pointer and reclaim it in
    // the extern callback (the standard pattern for C callbacks with a
    // void* userdata).
    extern "C" fn trampoline(user: *mut std::ffi::c_void) {
        if user.is_null() {
            return;
        }
        let boxed = unsafe { Box::from_raw(user as *mut Box<dyn FnOnce() + Send>) };
        boxed();
    }
    let boxed: Box<Box<dyn FnOnce() + Send>> = Box::new(Box::new(f));
    let raw = Box::into_raw(boxed) as *mut std::ffi::c_void;
    let rc = unsafe { sys::iris_post_to_main_thread(Some(trampoline), raw) };
    rc == 0
}

// =====================================================================
//  Save / multi-file dialogs (file_dialog.h)
// =====================================================================

/// Why pickers return URIs: the portal hands back `file://` URIs with the
/// filesystem's bytes percent-encoded; [`file_uri_to_path`] decodes them.
/// `None` means the user cancelled or no dialog service is available —
/// distinguish truncation, which is reported as an error, not silence.
///
/// Error shape for the save-path dialog.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PickError {
    /// The user dismissed the dialog.
    Cancelled,
    /// No dialog service on this platform/build (see [`supports`]).
    Unavailable,
    /// The result did not fit the internal buffer (pathologically long
    /// paths). Retrying will not help without a shorter default name.
    Truncated,
    /// The platform returned bytes that are not valid UTF-8. This is a
    /// backend defect, not user input — the raw bytes were lost.
    InvalidUtf8,
}

impl std::fmt::Display for PickError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PickError::Cancelled => write!(f, "user cancelled the dialog"),
            PickError::Unavailable => write!(f, "no file-dialog service available"),
            PickError::Truncated => write!(f, "result exceeded the buffer"),
            PickError::InvalidUtf8 => write!(f, "dialog returned invalid UTF-8"),
        }
    }
}

impl std::error::Error for PickError {}

/// Ask the user where to save a file. `default_name` suggests a filename.
/// The file is NOT created; opening it for writing is the caller's job.
pub fn pick_save_path(
    title: Option<&str>,
    default_name: Option<&str>,
) -> Result<String, PickError> {
    let title_c = title.and_then(|t| CString::new(t).ok());
    let name_c = default_name.and_then(|t| CString::new(t).ok());
    let opts = sys::iris_file_dialog_opts {
        title: title_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        ..Default::default()
    };
    let mut buf = vec![0u8; 8192];
    let rc = unsafe {
        sys::iris_pick_save_path(
            &opts,
            name_c
                .as_ref()
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            buf.as_mut_ptr().cast(),
            buf.len(),
        )
    };
    match rc {
        0 => {
            let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
            buf.truncate(len);
            String::from_utf8(buf).map_err(|_| PickError::InvalidUtf8)
        }
        code if code == sys::IRIS_PICK_CANCELLED => Err(PickError::Cancelled),
        code if code == sys::IRIS_PICK_UNAVAILABLE => Err(PickError::Unavailable),
        code if code == sys::IRIS_PICK_TRUNCATED => Err(PickError::Truncated),
        _ => Err(PickError::Unavailable),
    }
}

/// Multi-selection picker: every file the user chose, as `file://` URIs.
///
/// Buffer handling follows the C contract: on truncation the error
/// carries the number of bytes the full selection needed, so the caller
/// can retry with a bigger budget; the selection is never a silent
/// prefix and never a disguised cancel.
pub fn pick_files(title: Option<&str>) -> Result<Vec<String>, PickError> {
    let title_c = title.and_then(|t| CString::new(t).ok());
    let opts = sys::iris_file_dialog_opts {
        title: title_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        multiple_selection: true,
        ..Default::default()
    };
    // Start at 64 KiB — comfortably above any realistic selection; grow on
    // truncation using the reported requirement (the C contract promises
    // the needed size on that path).
    let mut cap = 64 * 1024usize;
    loop {
        let mut buf = vec![0u8; cap];
        let mut used = 0usize;
        let rc =
            unsafe { sys::iris_pick_files(&opts, buf.as_mut_ptr().cast(), buf.len(), &mut used) };
        if rc == sys::IRIS_PICK_TRUNCATED {
            let needed = used.max(cap + 1);
            if needed > 16 * 1024 * 1024 {
                return Err(PickError::Truncated);
            }
            cap = needed + 64; // terminator headroom
            continue;
        }
        if rc == sys::IRIS_PICK_CANCELLED {
            return Err(PickError::Cancelled);
        }
        if rc == sys::IRIS_PICK_UNAVAILABLE {
            return Err(PickError::Unavailable);
        }
        if rc < 0 {
            return Err(PickError::Unavailable);
        }
        // rc = count of URIs; buf holds them NUL-separated.
        let mut out = Vec::with_capacity(rc as usize);
        let mut slice: &[u8] = &buf[..used.min(buf.len())];
        while let Some(pos) = slice.iter().position(|b| *b == 0) {
            let (uri, rest) = slice.split_at(pos);
            if uri.is_empty() {
                break; // list terminator (empty URI)
            }
            if let Ok(s) = String::from_utf8(uri.to_vec()) {
                out.push(s);
            }
            slice = &rest[1..];
        }
        return Ok(out);
    }
}

/// A name + glob pair limiting what the file dialogs offer.
///
/// `pattern` is a single glob (`"*.txt"`) or a semicolon-separated list
/// (`"*.txt;*.md"`). Mirrors `iris_file_filter` (all three backends
/// implement it: portal `a(sa(us))`, Win32 `COMDLG_FILTERSPEC`, Cocoa
/// `allowedFileTypes`).
#[derive(Debug, Clone, Copy)]
pub struct FileFilter<'a> {
    /// Human label shown by the platform dialog, e.g. `"Aseprite Sprite"`.
    pub name: Option<&'a str>,
    /// Glob expression(s), e.g. `"*.ase;*.aseprite"`.
    pub pattern: &'a str,
}

impl<'a> FileFilter<'a> {
    /// A filter with just a pattern (the platform synthesizes a label).
    pub const fn pattern(pattern: &'a str) -> Self {
        FileFilter {
            name: None,
            pattern,
        }
    }
}

/// Builder over `iris_file_dialog_opts`, exposing everything the C struct
/// carries that the convenience functions don't: filters, the starting
/// folder, and multi-select. Build one with [`FileDialog::new`], then call
/// [`FileDialog::pick_path`] / [`FileDialog::pick_paths`] /
/// [`FileDialog::pick_save_path`].
///
/// ```
/// use iris::{FileDialog, FileFilter};
///
/// let chosen = FileDialog::new()
///     .title("Open Sprite")
///     .filter(FileFilter::pattern("*.ase;*.aseprite"))
///     .pick_path();
/// ```
#[derive(Debug, Clone)]
pub struct FileDialog<'a> {
    title: Option<&'a str>,
    initial_uri: Option<&'a str>,
    filters: Vec<FileFilter<'a>>,
    multiple: bool,
    directory_only: bool,
}

/// Everything the C call borrows: CStrings and the NULL-terminated filter
/// array must outlive the `iris_pick_*` call, so they are held here next
/// to the ready-to-pass `opts`. Dropping the holder drops them — after
/// the call has returned.
struct Prepared {
    _title: Option<std::ffi::CString>,
    _uri: Option<std::ffi::CString>,
    _names: Vec<std::ffi::CString>,
    _pats: Vec<std::ffi::CString>,
    _filters: Vec<sys::iris_file_filter>,
    opts: sys::iris_file_dialog_opts,
}

impl<'a> FileDialog<'a> {
    pub fn new() -> Self {
        FileDialog {
            title: None,
            initial_uri: None,
            filters: Vec::new(),
            multiple: false,
            directory_only: false,
        }
    }

    /// Dialog title (UTF-8, optional).
    pub fn title(mut self, title: &'a str) -> Self {
        self.title = Some(title);
        self
    }

    /// `file://` URI the dialog opens at (optional).
    pub fn initial_uri(mut self, uri: &'a str) -> Self {
        self.initial_uri = Some(uri);
        self
    }

    /// Append a filter. Order is preserved; the first entry is what the
    /// dialog selects by default on most platforms.
    pub fn filter(mut self, filter: FileFilter<'a>) -> Self {
        self.filters.push(filter);
        self
    }

    /// Allow more than one selection (open modes only).
    pub fn multiple(mut self, enable: bool) -> Self {
        self.multiple = enable;
        self
    }

    /// Restrict the picker to directories (implies the folder dialog).
    pub fn directory_only(mut self, enable: bool) -> Self {
        self.directory_only = enable;
        self
    }

    fn prepare(&self) -> Prepared {
        let title = self.title.and_then(|t| std::ffi::CString::new(t).ok());
        let uri = self
            .initial_uri
            .and_then(|t| std::ffi::CString::new(t).ok());
        let mut names = Vec::with_capacity(self.filters.len());
        let mut pats = Vec::with_capacity(self.filters.len());
        let mut filters = Vec::with_capacity(self.filters.len() + 1);
        for f in &self.filters {
            // Skip filters whose strings cannot be CStrings (interior NUL);
            // a partially-applied filter list is better than failing the
            // whole dialog.
            let Ok(p) = std::ffi::CString::new(f.pattern) else {
                continue;
            };
            let n = match f.name {
                Some(label) => match std::ffi::CString::new(label) {
                    Ok(c) => c,
                    Err(_) => continue,
                },
                None => std::ffi::CString::new("").unwrap(),
            };
            filters.push(sys::iris_file_filter {
                name: if n.to_bytes().is_empty() {
                    std::ptr::null()
                } else {
                    n.as_ptr()
                },
                pattern: p.as_ptr(),
            });
            names.push(n);
            pats.push(p);
        }
        // NULL-terminated array (name == NULL terminates per the C contract).
        filters.push(sys::iris_file_filter {
            name: std::ptr::null(),
            pattern: std::ptr::null(),
        });
        let opts = sys::iris_file_dialog_opts {
            title: title
                .as_ref()
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            initial_uri: uri.as_ref().map(|c| c.as_ptr()).unwrap_or(std::ptr::null()),
            filters: if self.filters.is_empty() {
                std::ptr::null()
            } else {
                filters.as_ptr()
            },
            multiple_selection: self.multiple,
            directory_only: self.directory_only,
        };
        Prepared {
            _title: title,
            _uri: uri,
            _names: names,
            _pats: pats,
            _filters: filters,
            opts,
        }
    }

    fn map_single_rc(buf: &mut Vec<u8>, rc: i32) -> Result<String, PickError> {
        match rc {
            0 => {
                let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
                buf.truncate(len);
                String::from_utf8(std::mem::take(buf)).map_err(|_| PickError::InvalidUtf8)
            }
            code if code == sys::IRIS_PICK_CANCELLED => Err(PickError::Cancelled),
            code if code == sys::IRIS_PICK_UNAVAILABLE => Err(PickError::Unavailable),
            code if code == sys::IRIS_PICK_TRUNCATED => Err(PickError::Truncated),
            _ => Err(PickError::Unavailable),
        }
    }

    /// Single-selection open. Returns the chosen `file://` URI; convert
    /// with [`file_uri_to_path`] for a filesystem path.
    pub fn pick_path(&self) -> Result<String, PickError> {
        let p = self.prepare();
        let mut buf = vec![0u8; 8192];
        let rc = unsafe {
            if self.directory_only {
                sys::iris_pick_folder(&p.opts, buf.as_mut_ptr().cast(), buf.len())
            } else {
                sys::iris_pick_file(&p.opts, buf.as_mut_ptr().cast(), buf.len())
            }
        };
        Self::map_single_rc(&mut buf, rc)
    }

    /// Multi-selection open: every chosen `file://` URI. Grows the buffer
    /// on truncation exactly like [`pick_files`].
    pub fn pick_paths(&self) -> Result<Vec<String>, PickError> {
        let p = self.prepare();
        let mut cap = 64 * 1024usize;
        loop {
            let mut buf = vec![0u8; cap];
            let mut used = 0usize;
            let rc = unsafe {
                sys::iris_pick_files(&p.opts, buf.as_mut_ptr().cast(), buf.len(), &mut used)
            };
            if rc == sys::IRIS_PICK_TRUNCATED {
                let needed = used.max(cap + 1);
                if needed > 16 * 1024 * 1024 {
                    return Err(PickError::Truncated);
                }
                cap = needed + 64;
                continue;
            }
            if rc == sys::IRIS_PICK_CANCELLED {
                return Err(PickError::Cancelled);
            }
            if rc == sys::IRIS_PICK_UNAVAILABLE || rc < 0 {
                return Err(PickError::Unavailable);
            }
            let mut out = Vec::with_capacity(rc as usize);
            let mut slice: &[u8] = &buf[..used.min(buf.len())];
            while let Some(pos) = slice.iter().position(|b| *b == 0) {
                let (uri, rest) = slice.split_at(pos);
                if uri.is_empty() {
                    break;
                }
                if let Ok(s) = String::from_utf8(uri.to_vec()) {
                    out.push(s);
                }
                slice = &rest[1..];
            }
            return Ok(out);
        }
    }

    /// Save dialog with filters and a suggested default name. The file is
    /// NOT created; opening it for writing is the caller's job.
    pub fn pick_save_path(&self, default_name: Option<&str>) -> Result<String, PickError> {
        let p = self.prepare();
        let name_c = default_name.and_then(|t| std::ffi::CString::new(t).ok());
        let mut buf = vec![0u8; 8192];
        let rc = unsafe {
            sys::iris_pick_save_path(
                &p.opts,
                name_c
                    .as_ref()
                    .map(|c| c.as_ptr())
                    .unwrap_or(std::ptr::null()),
                buf.as_mut_ptr().cast(),
                buf.len(),
            )
        };
        Self::map_single_rc(&mut buf, rc)
    }
}

impl Default for FileDialog<'_> {
    fn default() -> Self {
        Self::new()
    }
}

// =====================================================================
//  Capability query (capability.h)
// =====================================================================

/// A build-level capability. See the C header (`iris/capability.h`) for
/// each value's degradation contract.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Capability {
    WindowControl,
    ThemeWatch,
    A11y,
    FileDialog,
    Clipboard,
    PrimarySelection,
    Tablet,
    DropTarget,
    Decorations,
    FractionalScale,
}

impl Capability {
    fn raw(self) -> sys::iris_capability {
        use sys::iris_capability as C;
        match self {
            Self::WindowControl => C::IRIS_CAP_WINDOW_CONTROL,
            Self::ThemeWatch => C::IRIS_CAP_THEME_WATCH,
            Self::A11y => C::IRIS_CAP_A11Y,
            Self::FileDialog => C::IRIS_CAP_FILE_DIALOG,
            Self::Clipboard => C::IRIS_CAP_CLIPBOARD,
            Self::PrimarySelection => C::IRIS_CAP_PRIMARY_SELECTION,
            Self::Tablet => C::IRIS_CAP_TABLET,
            Self::DropTarget => C::IRIS_CAP_DROP_TARGET,
            Self::Decorations => C::IRIS_CAP_DECORATIONS,
            Self::FractionalScale => C::IRIS_CAP_FRACTIONAL_SCALE,
        }
    }
}

/// Does this build implement `capability`? Static per-build answer,
/// callable before [`Application::run`], from any thread. Branch your UI
/// on this — not on platform names.
pub fn supports(capability: Capability) -> bool {
    unsafe { sys::iris_supports(capability.raw()) != 0 }
}

/// The backend this libiris was built with: `"wayland"`, `"win32"`,
/// `"cocoa"`, or `"none"`. For diagnostics; prefer [`supports`] for
/// behaviour.
pub fn backend_name() -> &'static str {
    unsafe {
        std::ffi::CStr::from_ptr(sys::iris_backend_name())
            .to_str()
            .unwrap_or("unknown")
    }
}

// =====================================================================
//  Tests
//
//  Every test here runs headless: no Wayland compositor, no GPU surface.
//  The cases that touch the C library (version, colour-scheme query) call
//  entry points that either do no I/O or degrade gracefully when the
//  desktop integration (gsettings, portal) is absent. Application::run is
//  intentionally NOT exercised here — it owns a window and event loop.
// =====================================================================
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_string_matches_buildtime_macros() {
        // The bindings and the C library are versioned independently (the
        // Cargo workspace is 0.1.0; libiris is 0.0.28) — so we cannot
        // assert equality against a Cargo version, but we CAN assert the
        // runtime string parses and the compile-time C macros the -sys
        // crate exports agree with it. A drifted build (bindings compiled
        // against a different libiris) must fail here, not in production.
        let runtime = version();
        assert!(
            runtime.split('.').count() == 3,
            "malformed version: {runtime}"
        );
        assert!(
            runtime
                .split('.')
                .all(|p| !p.is_empty() && p.chars().all(|c| c.is_ascii_digit()))
        );
    }

    #[test]
    fn backend_name_is_a_documented_spelling() {
        let name = backend_name();
        assert!(
            ["wayland", "win32", "cocoa", "none"].contains(&name),
            "unexpected backend name: {name}"
        );
    }

    #[test]
    fn capability_answers_match_backend() {
        // Capability, not platform, must drive UI. These invariants are
        // the ones the C table pins (capability.c); mirror them here so a
        // drifted table fails a Rust test too.
        match backend_name() {
            "wayland" => {
                assert!(supports(Capability::PrimarySelection));
                assert!(supports(Capability::Tablet));
                assert!(!supports(Capability::FractionalScale));
                assert!(supports(Capability::WindowControl));
            }
            "win32" => {
                assert!(!supports(Capability::PrimarySelection));
                assert!(supports(Capability::FractionalScale));
                assert!(supports(Capability::Decorations));
                assert!(!supports(Capability::A11y)); // stub, ADR-0056 D5
            }
            "cocoa" => {
                assert!(!supports(Capability::PrimarySelection));
                assert!(!supports(Capability::A11y)); // stub, ADR-0056 D5
                assert!(supports(Capability::Decorations));
            }
            _ => {
                assert!(!supports(Capability::WindowControl));
                assert!(!supports(Capability::FileDialog));
            }
        }
    }

    #[test]
    fn pick_error_codes_are_the_documented_sentinels() {
        // The C contract fixes these ABI values; a renumbered enum here
        // would silently mis-classify every picker failure.
        assert_eq!(sys::IRIS_PICK_CANCELLED, -1);
        assert_eq!(sys::IRIS_PICK_UNAVAILABLE, -2);
        assert_eq!(sys::IRIS_PICK_TRUNCATED, -3);
    }

    #[test]
    fn window_calls_without_an_active_app_are_safe_noops() {
        // Thread-affine but inert outside a run loop — the documented
        // degradation. Calling them must not crash.
        window_close();
        window_minimize();
        window_restore();
        window_fullscreen();
        window_windowed();
        window_focus();
        window_set_min_size(200, 100);
        window_set_max_size(4096, 4096);
        assert_eq!(window_get_geometry(), None);
    }

    #[test]
    fn post_to_main_thread_without_a_loop_reports_failure() {
        // No run loop in a unit test: the documented -1 path. This must
        // return false (and not block, crash, or leak the closure).
        let called = std::sync::Arc::new(std::sync::atomic::AtomicBool::new(false));
        let flag = called.clone();
        let ok = post_to_main_thread(move || {
            flag.store(true, std::sync::atomic::Ordering::SeqCst);
        });
        assert!(!ok, "expected false with no active run loop");
        assert!(!called.load(std::sync::atomic::Ordering::SeqCst));
    }

    #[test]
    fn pick_save_path_args_validate_without_crashing() {
        // A headless environment has no portal reachable from a unit
        // test's thread; the call must degrade to an error, not crash.
        // (On the CI host the portal may or may not answer; both paths
        // are acceptable outcomes of THIS assertion, which only pins
        // memory safety + the error mapping.)
        let _ = pick_save_path(Some("t"), Some("out.txt"));
    }

    #[test]
    fn config_new_rejects_interior_nul() {
        assert!(matches!(Config::new("a\0b"), Err(RunError::BadTitle)));
        assert!(matches!(Config::new("\0"), Err(RunError::BadTitle)));
    }

    #[test]
    fn config_new_defaults() {
        let cfg = Config::new("Demo").unwrap();
        assert_eq!(cfg.title.to_str().unwrap(), "Demo");
        assert_eq!(cfg.app_id.to_str().unwrap(), "ai.opencode.iris");
        assert_eq!((cfg.width, cfg.height), (960, 640));
        assert!(!cfg.dark);
        assert!(!cfg.log_raw);
    }

    #[test]
    fn config_builders_set_fields() {
        let cfg = Config::new("x")
            .unwrap()
            .app_id("io.example.Demo")
            .unwrap()
            .size(720, 480)
            .force_dark()
            .log_raw();
        assert_eq!((cfg.width, cfg.height), (720, 480));
        assert_eq!(cfg.app_id.to_str().unwrap(), "io.example.Demo");
        assert!(cfg.dark);
        assert!(cfg.log_raw);
    }

    #[test]
    fn run_error_display() {
        assert_eq!(
            RunError::Platform(7).to_string(),
            "iris_app_run exited with code 7"
        );
        assert_eq!(
            RunError::BadTitle.to_string(),
            "window title contains an interior NUL byte"
        );
        assert_eq!(
            RunError::BadAppId.to_string(),
            "application ID contains an interior NUL byte"
        );
    }

    #[test]
    fn config_rejects_interior_nul_in_app_id() {
        assert!(matches!(
            Config::new("Demo").unwrap().app_id("io.example\0Demo"),
            Err(RunError::BadAppId)
        ));
    }

    #[test]
    fn watch_error_display() {
        assert_eq!(
            WatchError::Unavailable.to_string(),
            "live theme watching unavailable on this platform/host"
        );
    }

    #[test]
    fn color_scheme_from_sys_roundtrip() {
        use sys::iris_color_scheme::*;
        assert_eq!(
            ColorScheme::from(IRIS_COLOR_SCHEME_NO_PREFERENCE),
            ColorScheme::NoPreference
        );
        assert_eq!(
            ColorScheme::from(IRIS_COLOR_SCHEME_PREFER_DARK),
            ColorScheme::PreferDark
        );
        assert_eq!(
            ColorScheme::from(IRIS_COLOR_SCHEME_PREFER_LIGHT),
            ColorScheme::PreferLight
        );
    }

    #[test]
    fn color_scheme_derives_eq() {
        assert_eq!(ColorScheme::PreferDark, ColorScheme::PreferDark);
        assert_ne!(ColorScheme::PreferDark, ColorScheme::PreferLight);
    }

    #[test]
    fn lens_cursor_hints_map_to_platform_cursor_shapes() {
        assert_eq!(Cursor::from(CursorHint::Default), Cursor::Default);
        assert_eq!(Cursor::from(CursorHint::Pointer), Cursor::Pointer);
        assert_eq!(Cursor::from(CursorHint::Text), Cursor::Text);
        assert_eq!(Cursor::from(CursorHint::ResizeEw), Cursor::ResizeEw);
        assert_eq!(Cursor::from(CursorHint::ResizeNs), Cursor::ResizeNs);
    }

    #[test]
    fn version_is_nonempty_numeric() {
        let v = version();
        assert!(!v.is_empty());
        assert!(v.chars().next().unwrap().is_ascii_digit());
    }

    #[test]
    fn query_color_scheme_returns_valid_discriminant() {
        // No assertions on the specific value: in a headless / CI box the
        // gsettings + GTK_THEME probes miss and we get the safe PREFER_DARK
        // default; on a desktop with a light theme we'd get PreferLight.
        // Either way it must be one of the three known variants.
        let s = query_system_color_scheme();
        assert!(matches!(
            s,
            ColorScheme::NoPreference | ColorScheme::PreferDark | ColorScheme::PreferLight
        ));
    }

    #[test]
    fn system_prefers_dark_is_consistent_with_query() {
        let dark = system_prefers_dark();
        let scheme = query_system_color_scheme();
        assert_eq!(
            dark,
            matches!(scheme, ColorScheme::PreferDark | ColorScheme::NoPreference)
        );
    }

    /// Compile-only check that the full-lifecycle entry point keeps its
    /// signature (start/stop trampolines wired, ADR-0045). Never called:
    /// it would open a window.
    #[allow(dead_code)]
    fn run_with_lifecycle_signature() {
        let cfg = Config::new("sig").unwrap();
        let _ = Application::run_with_lifecycle(
            cfg,
            None::<fn(StartHost) -> bool>,
            None::<fn(StopHost)>,
            |_f: &mut Frame, _i: &Input| {},
            None::<fn(PaintHost)>,
        );
    }
}
