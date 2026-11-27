//! Safe Rust bindings to **app** — the application shell that runs a
//! UI in a native window.
//!
//! window owns the window, GPU device and event loop; ui is the UI engine;
//! app binds the two. Each frame it runs two host callbacks:
//!
//! 1. **build** ([`Frame`] + [`Input`]) — inside an open `optics_ui_begin/end`
//!    pair. The host builds its chrome (immediate-mode ui widgets) and
//!    reads the per-frame [`Input`] snapshot to drive anything ui itself
//!    does not own.
//! 2. **record** ([`RecordHost`]) — on the UI thread, before the frame is
//!    submitted. The host records canvas content into an immutable display
//!    list; it is composited under the chrome. Use it for surfaces ui
//!    cannot describe (a document canvas, an image, a custom render path).
//!
//! ```no_run
//! use optics_app::{Application, Config};
//!
//! Application::run_with_record(
//!     Config::new("Demo").unwrap(),
//!     |_frame, _input| { /* build chrome */ },
//!     Some(|_host| { /* record document surface */ }),
//! ).unwrap();
//! ```
//!
//! Window control, cursors, dialogs and the colour-scheme query live in the
//! re-exported [`window`] crate; the UI toolkit lives in [`ui`] (with the
//! signature types [`Frame`], [`Input`], [`Ui`], and [`CursorHint`] also
//! re-exported at the crate root); and canvas recording types live in [`gfx`].

#![deny(rust_2018_idioms)]

use std::ffi::CString;

/// Raw-bindings escape hatch (see the `gfx` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
pub use optics_app_sys as sys;

/// The 2D vector canvas and Vulkan rendering foundation (`RecordHost::encoder`).
pub use optics_gfx as gfx;

/// The immediate-mode UI toolkit driven inside each frame.
pub use optics_ui as ui;

/// The platform shell this crate runs on: window, cursor, dialogs, pacing.
pub use optics_window as window;

/// UI types that appear directly in `optics-app` function signatures.
pub use optics_ui::{CursorHint, Frame, Input, Ui};

/// Borrowed handles passed to [`Application::run_with_record`] for decoupled host canvas recording (ADR-0127).
pub struct RecordHost {
    encoder: *mut sys::optics_gfx_encoder,
    scale: f32,
}

impl RecordHost {
    /// Borrowed encoder command recorder for this frame.
    pub fn encoder(&mut self) -> optics_gfx::Encoder {
        // SAFETY: window handed us a live `optics_gfx_encoder*` for the duration of the record callback.
        unsafe { optics_gfx::Encoder::borrow_raw(self.encoder) }
    }

    /// Current display scale factor.
    pub fn scale(&self) -> f32 {
        self.scale
    }
}

/// A thin wrapper over the raw pointers window hands to the optional start
/// callback: the ui context and the gfx device window owns for this app.
/// The callback runs once — after window has created its device, canvas and
/// ui context, before the first frame — which makes it the right place to
/// set up device-dependent resources (text shapers, image uploads) without
/// installing a per-frame record callback: a start hook costs nothing per
/// frame and does not defeat the backend's idle frame-skip. The same
/// device-borrow rule as [`StartHost::optics_gfx_device`] applies.
pub struct StartHost {
    ui: *mut sys::optics_ui,
    device: *mut sys::optics_gfx_device,
}

impl StartHost {
    /// The live `ui*`, as the safe crate's borrowed [`Ui`] handle. Valid
    /// for the app's lifetime; window destroys the context at shutdown.
    pub fn ui(&self) -> Ui {
        // SAFETY: window keeps `ui*` alive for the whole run; `Ui::borrow`
        // is the safe non-owning view over it (no destroy on drop).
        // SAFETY: window handed us live typed pointers for the callback's duration.
        unsafe { Ui::borrow_raw(self.ui) }
    }

    /// The `optics_gfx_device*` window owns, as a borrowed (non-owning)
    /// `optics_gfx::Device`. Borrow it — never open a second device in the same
    /// process.
    pub fn optics_gfx_device(&self) -> optics_gfx::Device {
        // SAFETY: see [`StartHost::optics_gfx_device`]; same contract, the start
        // callback's device pointer lives until shutdown.
        unsafe { optics_gfx::Device::borrow_raw(self.device) }
    }
}

/// The handle window hands to the optional `stop` callback: the same borrows
/// as [`StartHost`], live one last time after the frame loop and before
/// window tears down ui, gfx, and the device (ADR-0045). This is where
/// hosts release every device-backed resource created from
/// [`StartHost::optics_gfx_device`].
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
            RunError::Platform(rc) => write!(f, "optics_app_run exited with code {rc}"),
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
            app_id: CString::new("org.optics.app").expect("static app ID is valid"),
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
/// Wraps `optics_app_run`. `build` runs once per frame inside an open
/// `optics_ui_begin/end` pair, with `input` being the same [`Input`] snapshot
/// ui is consuming this frame. `record` (if `Some`) records host canvas
/// content into an immutable display list before the frame is submitted, so
/// it lands under the chrome. Blocks the calling thread until the window is
/// closed.
pub struct Application;

impl Application {
    /// Run the application until the user closes the window. Blocks the
    /// calling thread; callbacks run on it. See [`Config`] for the setup
    /// surface and the crate docs for the callback model.
    pub fn run<B>(config: Config, build: B) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame<'_>, &Input),
    {
        Self::run_impl(
            config,
            None::<fn(StartHost) -> bool>,
            None::<fn(StopHost)>,
            build,
            None::<fn(RecordHost)>,
        )
    }

    /// Like [`Application::run`], plus a one-shot `start` callback that window
    /// invokes after its device/canvas/ui setup and before the first frame
    /// (see [`StartHost`]). The callback returning `false` aborts the run.
    pub fn run_with_start<B, S>(config: Config, start: S, build: B) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame<'_>, &Input),
        S: FnMut(StartHost) -> bool,
    {
        Self::run_impl(
            config,
            Some(start),
            None::<fn(StopHost)>,
            build,
            None::<fn(RecordHost)>,
        )
    }

    /// The full lifecycle form (ADR-0045): `start` runs after window's
    /// device/canvas/ui setup and before the first frame (returning
    /// `false` aborts the run); `stop` runs after the frame loop and before
    /// window destroys the device, giving hosts a deterministic point to
    /// release device-backed resources. `record` is the optional host canvas
    /// recorder. Either may be `None`.
    pub fn run_with_lifecycle<B, S, T, R>(
        config: Config,
        start: Option<S>,
        stop: Option<T>,
        build: B,
        record: Option<R>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame<'_>, &Input),
        S: FnMut(StartHost) -> bool,
        T: FnMut(StopHost),
        R: FnMut(RecordHost) + 'static,
    {
        Self::run_impl(config, start, stop, build, record)
    }

    /// Run an application with a host canvas recorder (ADR-0127).
    ///
    /// The `record` closure records vector drawing into a CPU-bound
    /// [`optics_gfx::Encoder`]. The resulting immutable display list is submitted
    /// by the same presentation pipeline as ui chrome, on every backend.
    pub fn run_with_record<B, R>(
        config: Config,
        build: B,
        record: Option<R>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame<'_>, &Input),
        R: FnMut(RecordHost) + 'static,
    {
        Self::run_impl(
            config,
            None::<fn(StartHost) -> bool>,
            None::<fn(StopHost)>,
            build,
            record,
        )
    }

    fn run_impl<B, S, T, R>(
        config: Config,
        start: Option<S>,
        stop: Option<T>,
        build: B,
        record: Option<R>,
    ) -> Result<(), RunError>
    where
        B: FnMut(&mut Frame<'_>, &Input),
        S: FnMut(StartHost) -> bool,
        T: FnMut(StopHost),
        R: FnMut(RecordHost) + 'static,
    {
        let build_box: Box<B> = Box::new(build);
        let start_box: Option<Box<S>> = start.map(Box::new);
        let stop_box: Option<Box<T>> = stop.map(Box::new);
        let record_box: Option<Box<R>> = record.map(Box::new);

        let mut run_state = RunState {
            build: build_box,
            start: start_box,
            stop: stop_box,
            record: record_box,
        };

        extern "C" fn build_trampoline<B, S, T, R>(
            ui: *mut sys::optics_ui,
            in_: *const sys::optics_ui_input,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame<'_>, &Input),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
            R: FnMut(RecordHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, S, T, R>) };
                let mut frame = unsafe { Frame::from_raw(ui) };
                let input = unsafe { Input::from_raw_ref(in_) };
                (run.build)(&mut frame, input);
            }));
        }

        extern "C" fn record_trampoline<B, S, T, R>(
            enc: *mut sys::optics_gfx_encoder,
            scale: f32,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame<'_>, &Input),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
            R: FnMut(RecordHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, S, T, R>) };
                if let Some(r) = run.record.as_mut() {
                    r(RecordHost {
                        encoder: enc,
                        scale,
                    });
                }
            }));
        }

        extern "C" fn start_trampoline<B, S, T, R>(
            ui: *mut sys::optics_ui,
            device: *mut sys::optics_gfx_device,
            user: *mut std::os::raw::c_void,
        ) -> bool
        where
            B: FnMut(&mut Frame<'_>, &Input),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
            R: FnMut(RecordHost),
        {
            std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, S, T, R>) };
                match run.start.as_mut() {
                    Some(s) => s(StartHost { ui, device }),
                    None => true,
                }
            }))
            .unwrap_or(false)
        }

        extern "C" fn stop_trampoline<B, S, T, R>(
            ui: *mut sys::optics_ui,
            device: *mut sys::optics_gfx_device,
            user: *mut std::os::raw::c_void,
        ) where
            B: FnMut(&mut Frame<'_>, &Input),
            S: FnMut(StartHost) -> bool,
            T: FnMut(StopHost),
            R: FnMut(RecordHost),
        {
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let run = unsafe { &mut *(user as *mut RunState<B, S, T, R>) };
                if let Some(t) = run.stop.as_mut() {
                    t(StartHost { ui, device });
                }
            }));
        }

        let user_ptr = &mut run_state as *mut _ as *mut std::os::raw::c_void;
        let record_fn: sys::optics_window_record_fn = if run_state.record.is_some() {
            Some(record_trampoline::<B, S, T, R>)
        } else {
            None
        };
        let start_fn: sys::optics_app_start_fn = if run_state.start.is_some() {
            Some(start_trampoline::<B, S, T, R>)
        } else {
            None
        };
        let stop_fn: sys::optics_app_stop_fn = if run_state.stop.is_some() {
            Some(stop_trampoline::<B, S, T, R>)
        } else {
            None
        };

        let cfg = sys::optics_app_opts {
            title: config.title.as_ptr(),
            app_id: config.app_id.as_ptr(),
            width: config.width,
            height: config.height,
            dark: config.dark,
            log_raw: config.log_raw,
            no_clear: false,
            on_event: None,
            start: start_fn,
            stop: stop_fn,
            build: Some(build_trampoline::<B, S, T, R>),
            record: record_fn,
            prepare: None,
            user: user_ptr,
        };

        let rc = unsafe { sys::optics_app_run(&cfg) };
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
struct RunState<B, S, T, R>
where
    B: FnMut(&mut Frame<'_>, &Input),
    S: FnMut(StartHost) -> bool,
    T: FnMut(StopHost),
    R: FnMut(RecordHost),
{
    build: Box<B>,
    start: Option<Box<S>>,
    stop: Option<Box<T>>,
    record: Option<Box<R>>,
}

/// The semantic cursor ui reports for the hovered widget, as the platform
/// cursor window shows for it. The backends apply this mapping themselves every
/// frame; it is exposed for hosts that draw their own cursor.
pub fn cursor_for_hint(hint: CursorHint) -> window::Cursor {
    match hint {
        CursorHint::Default => window::Cursor::Default,
        CursorHint::Pointer => window::Cursor::Pointer,
        CursorHint::Text => window::Cursor::Text,
        CursorHint::ResizeEw => window::Cursor::ResizeEw,
        CursorHint::ResizeNs => window::Cursor::ResizeNs,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn optics_ui_cursor_hints_map_to_platform_cursor_shapes() {
        assert_eq!(
            cursor_for_hint(CursorHint::Default),
            window::Cursor::Default
        );
        assert_eq!(
            cursor_for_hint(CursorHint::Pointer),
            window::Cursor::Pointer
        );
        assert_eq!(cursor_for_hint(CursorHint::Text), window::Cursor::Text);
        assert_eq!(
            cursor_for_hint(CursorHint::ResizeEw),
            window::Cursor::ResizeEw
        );
        assert_eq!(
            cursor_for_hint(CursorHint::ResizeNs),
            window::Cursor::ResizeNs
        );
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
        assert_eq!(cfg.app_id.to_str().unwrap(), "org.optics.app");
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
            "optics_app_run exited with code 7"
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
    fn reexported_motion_module_is_reachable() {
        let mut s = crate::ui::motion::Spring::new(0.0);
        let v = s.advance(
            1.0,
            crate::ui::motion::SpringParams::snappy(),
            1.0 / 60.0,
            true,
        );
        assert_eq!(v, 1.0);
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
            |_f: &mut Frame<'_>, _i: &Input| {},
            None::<fn(RecordHost)>,
        );
    }
}
