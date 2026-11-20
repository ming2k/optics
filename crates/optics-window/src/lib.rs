//! Safe Rust bindings to **iris** — the platform shell of the optics stack.
//!
//! iris owns the native window, the event pump and the frame cadence, and
//! depends on flux only. This crate wraps the parts of it an application
//! calls directly:
//!
//! - window control ([`window_close`], [`window_maximize`], ...) and
//!   [`set_cursor`];
//! - frame pacing requests ([`request_animation_frame`], ...);
//! - [`system_prefers_dark`] / [`ColorScheme`] and the scheme watcher;
//! - native file dialogs ([`pick_file`], [`FileDialog`]), capability queries
//!   and drag-and-drop.
//!
//! Running an application with a lens UI is the `aperture` crate's job
//! (`optics_app::Application`); it builds on this one. The raw UI-host seam a
//! different UI layer would implement is available through [`sys`].

#![deny(rust_2018_idioms)]

use std::ffi::CString;
use std::os::raw::c_int;

/// Raw-bindings escape hatch (see the `flux` crate's note). Kept `pub`
/// deliberately as the one documented unsafe escape hatch.
pub use optics_window_sys as sys;

/// Request one more frame at Iris's active animation cadence.
///
/// Call this during a build or record callback while host-owned, time-based
/// content is moving. Repeating it every frame sustains the animation; once
/// requests stop, Iris can return to its low-power idle cadence. Calls outside
/// an active [`Application::run`] are harmless no-ops.
pub fn request_animation_frame() {
    unsafe { sys::optics_window_request_animation_frame() };
}

/// Declare the current frame's host canvas content static.
///
/// Recorded host content is opaque to lens, so the backend normally
/// keeps painting every scheduled frame — including a full clear to the
/// theme background. When the host knows its scene is pixel-identical to
/// what is already on screen (a paused animation, a settled audio
/// visualizer), this call lets the backend skip the entire
/// record → clear → submit → present cycle: no swapchain image is
/// committed and the GPU does no host work, unloading the compositor
/// exactly like a host without a record callback.
///
/// The declaration covers one frame only and must be re-issued every frame
/// it applies to — call it from the **build** callback (build runs on every
/// scheduled frame, including skipped ones; a declaration made in record is
/// cleared with the first skipped frame and the cadence bounces back).
/// Any user input, a resize or buffer-scale change, a following
/// [`request_animation_frame`], or lens-reported chrome damage forces the
/// next frame to present again.
///
/// While static, the backend keeps a low idle tick (~4 Hz) so build (and
/// record, when it chooses) still run and the host can resume animating on
/// its own. No-op without an active app or without a record callback.
pub fn record_mark_static() {
    unsafe { sys::optics_window_record_mark_static() };
}

/// Skips this frame's render/present while keeping the active-rate cadence.
///
/// The host promises the canvas content is unchanged from the last
/// *presented* frame but is still streaming — the media-cadence pattern:
/// a 30 fps visualizer on a 60 Hz display renders on its stage ticks and
/// declares skip on the in-between frames while calling
/// [`request_animation_frame`] as usual. Only the begin → clear → record →
/// present work for this frame is suppressed; the next deadline stays armed
/// at the active rate (no decay to the ~4 Hz idle tick).
///
/// Mutually exclusive per frame with [`record_mark_static`] (skip wins for
/// rendering only when the static path would also have skipped; prefer
/// declaring exactly one). Unlike [`record_mark_static`], a fresh
/// [`request_animation_frame`] does **not** veto this declaration — the two
/// together are precisely the streaming shape. Any user input, a resize or
/// buffer-scale change, or lens-reported chrome damage still forces a real
/// present. Call from the **build** callback; consumed once, re-issue every
/// frame it applies to. No-op without an active app or without a record
/// callback.
pub fn request_frame_skip_render() {
    unsafe { sys::optics_window_request_frame_skip_render() };
}

/// System colour-scheme preference (queried at startup).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ColorScheme {
    NoPreference,
    PreferDark,
    PreferLight,
}

impl From<sys::optics_window_color_scheme> for ColorScheme {
    fn from(s: sys::optics_window_color_scheme) -> Self {
        use sys::optics_window_color_scheme::*;
        match s {
            OPTICS_WINDOW_COLOR_SCHEME_NO_PREFERENCE => ColorScheme::NoPreference,
            OPTICS_WINDOW_COLOR_SCHEME_PREFER_DARK => ColorScheme::PreferDark,
            OPTICS_WINDOW_COLOR_SCHEME_PREFER_LIGHT => ColorScheme::PreferLight,
        }
    }
}

/// Read the system colour-scheme preference.
pub fn query_system_color_scheme() -> ColorScheme {
    unsafe { sys::optics_window_query_system_color_scheme() }.into()
}

/// Convenience: returns true when the system prefers dark OR no preference
/// (we default to dark when the user is silent).
pub fn system_prefers_dark() -> bool {
    unsafe { sys::optics_window_system_prefers_dark() }
}

/// Callback fired by [`watch_system_color_scheme`] whenever the desktop
/// colour-scheme preference changes.
pub type ColorSchemeChangedFn =
    extern "C" fn(scheme: sys::optics_window_color_scheme, user: *mut std::os::raw::c_void);

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
    let rc = unsafe { sys::optics_window_color_scheme_watch(Some(cb), user) };
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
    unsafe { sys::optics_window_color_scheme_unwatch() }
}

/// Cursor appearance the host wants iris to show over its window.
///
/// This is an L3 concern: iris owns the window and the cursor surface, so it
/// owns cursor appearance too. While the host cursor is `Default`, iris shows
/// the semantic cursor the UI layer reports for the widget under the pointer;
/// hosts use [`set_cursor`] for custom canvas regions and other
/// application-specific overrides.
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
    fn raw(self) -> sys::optics_window_cursor {
        use sys::optics_window_cursor::*;
        match self {
            Cursor::Default => OPTICS_WINDOW_CURSOR_DEFAULT,
            Cursor::Text => OPTICS_WINDOW_CURSOR_TEXT,
            Cursor::Pointer => OPTICS_WINDOW_CURSOR_POINTER,
            Cursor::Busy => OPTICS_WINDOW_CURSOR_BUSY,
            Cursor::Crosshair => OPTICS_WINDOW_CURSOR_CROSSHAIR,
            Cursor::NotAllowed => OPTICS_WINDOW_CURSOR_NOT_ALLOWED,
            Cursor::ResizeEw => OPTICS_WINDOW_CURSOR_RESIZE_EW,
            Cursor::ResizeNs => OPTICS_WINDOW_CURSOR_RESIZE_NS,
        }
    }
}

/// Set the cursor the next pointer motion will show. Idempotent; passing the
/// same value twice does no work.
///
/// An explicit choice stays pinned until you call `set_cursor(Cursor::Default)`,
/// which hands cursor control back to the backend's per-frame follow of the
/// hovered widget's cursor hint (the backend does that natively on all
/// platforms — Wayland included — so hosts do not need to re-assert a custom
/// cursor every frame, only to keep it pinned).
///
/// No-op when iris was built without cursor-shape support (the compositor's
/// default arrow stays) or before the application loop starts. Thread-
/// affine: call from the same thread that drives the run loop.
pub fn set_cursor(cursor: Cursor) {
    unsafe { sys::optics_window_set_cursor(cursor.raw()) }
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
    let mut buf = vec![0u8; 4096];
    let opts = sys::optics_window_file_dialog_opts {
        title: title_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        out_path: buf.as_mut_ptr().cast(),
        out_cap: buf.len(),
        ..Default::default()
    };
    let rc = unsafe {
        if folder {
            sys::optics_window_pick_folder(&opts)
        } else {
            sys::optics_window_pick_file(&opts)
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
/// # use optics_window::file_uri_to_path;
/// assert_eq!(file_uri_to_path("file:///home/my%20docs/a.png").as_deref(),
///            Some("/home/my docs/a.png"));
/// assert_eq!(file_uri_to_path("file://server/share/x"), None);
/// ```
pub fn file_uri_to_path(uri: &str) -> Option<String> {
    let c = CString::new(uri).ok()?;
    // Longest realistic decoded path: URIs from the portal are bounded by
    // PATH_MAX-ish lengths; 8 KiB leaves headroom for percent expansion.
    let mut buf = vec![0u8; 8192];
    let opts = sys::optics_window_file_uri_to_path_opts {
        uri: c.as_ptr(),
        out_path: buf.as_mut_ptr().cast(),
        out_cap: buf.len(),
    };
    let rc = unsafe { sys::optics_window_file_uri_to_path(&opts) };
    if rc != 0 {
        return None;
    }
    let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
    buf.truncate(len);
    String::from_utf8(buf).ok()
}

/// Library version string ("0.0.30" at the time of writing).
pub fn version() -> &'static str {
    unsafe {
        std::ffi::CStr::from_ptr(sys::optics_window_version_string())
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
//  optics_window_run on the calling thread) and degrade to documented no-ops
//  on backends without the capability — check [`supports`] first when the
//  affordance is load-bearing in your UI.
// =====================================================================

/// Ask the active window to close. The loop exits and
/// [`Application::run`] returns, exactly as if the user clicked the
/// title-bar close button.
pub fn window_close() {
    unsafe { sys::optics_window_close() };
}

/// Request the compositor / window manager to start an interactive move of
/// the active window (initiated e.g. by pointer drag on a custom title bar
/// in client-side decoration mode).
///
/// On Wayland, this issues an `xdg_toplevel.move` request using the seat and
/// serial of the current pointer event. Safe no-op on backends without
/// interactive move capability.
pub fn window_start_move() {
    unsafe { sys::optics_window_start_move() };
}

/// Minimize the active window to the taskbar / dock.
pub fn window_minimize() {
    unsafe { sys::optics_window_minimize() };
}

/// Restore a minimized window, or un-fullscreen a fullscreen one.
pub fn window_restore() {
    unsafe { sys::optics_window_restore() };
}

/// Take the whole output (Wayland: the output the window is on; Win32/
/// Cocoa: the display the window overlaps most).
pub fn window_fullscreen() {
    unsafe { sys::optics_window_fullscreen() };
}

/// Leave fullscreen. No-op when not fullscreen.
pub fn window_windowed() {
    unsafe { sys::optics_window_unfullscreen() };
}

/// Maximize the window (fill the work area, keeping decorations).
pub fn window_maximize() {
    unsafe { sys::optics_window_maximize() };
}

/// Undo [`window_maximize`].
pub fn window_unmaximize() {
    unsafe { sys::optics_window_unmaximize() };
}

/// Ask the compositor to focus the window.
///
/// Permanently a no-op on Wayland (the protocol forbids self-activation —
/// only the user or the compositor can raise a window); works on Win32
/// and Cocoa.
pub fn window_focus() {
    unsafe { sys::optics_window_focus() };
}

/// Set the minimum client-area size, in logical pixels.
pub fn window_set_min_size(width: i32, height: i32) {
    let opts = sys::optics_window_set_min_size_opts { width, height };
    unsafe { sys::optics_window_set_min_size(&opts) };
}

/// Set the maximum client-area size, in logical pixels.
pub fn window_set_max_size(width: i32, height: i32) {
    let opts = sys::optics_window_set_max_size_opts { width, height };
    unsafe { sys::optics_window_set_max_size(&opts) };
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
    let ok = unsafe { sys::optics_window_get_geometry(&mut w, &mut h) };
    if ok {
        Some(WindowSize {
            width: w,
            height: h,
        })
    } else {
        None
    }
}

/// Request a window activation token (xdg-activation-v1 on Wayland) to pass
/// to an external or child process for smooth focus handoff. Returns None if
/// unsupported, inactive, or token creation fails.
pub fn window_create_activation_token(app_id: Option<&str>) -> Option<String> {
    let mut buf = [0u8; 512];
    let app_id_c = app_id.and_then(|s| std::ffi::CString::new(s).ok());
    let ptr = app_id_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr());
    let opts = sys::optics_window_create_activation_token_opts {
        app_id: ptr,
        out_buf: buf.as_mut_ptr() as *mut std::os::raw::c_char,
        out_cap: buf.len(),
    };
    let rc = unsafe { sys::optics_window_create_activation_token(&opts) };
    if rc == 0 {
        let cstr = unsafe { std::ffi::CStr::from_ptr(buf.as_ptr() as *const std::os::raw::c_char) };
        cstr.to_str().ok().map(|s| s.to_string())
    } else {
        None
    }
}

// =====================================================================
//  Cross-thread wakeup (app.h — the ONLY thread-safe iris entry point)
// =====================================================================

/// Schedule `f` to run on the iris main thread, outside any
/// `optics_ui_begin/end` pair.
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
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            boxed();
        }));
    }
    let boxed: Box<Box<dyn FnOnce() + Send>> = Box::new(Box::new(f));
    let raw = Box::into_raw(boxed) as *mut std::ffi::c_void;
    let rc = unsafe { sys::optics_window_post_to_main_thread(Some(trampoline), raw) };
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
    let mut buf = vec![0u8; 8192];
    let opts = sys::optics_window_file_dialog_opts {
        title: title_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        default_name: name_c
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        out_path: buf.as_mut_ptr().cast(),
        out_cap: buf.len(),
        ..Default::default()
    };
    let rc = unsafe { sys::optics_window_pick_save_path(&opts) };
    match rc {
        0 => {
            let len = buf.iter().position(|b| *b == 0).unwrap_or(buf.len());
            buf.truncate(len);
            String::from_utf8(buf).map_err(|_| PickError::InvalidUtf8)
        }
        code if code == sys::OPTICS_WINDOW_PICK_CANCELLED => Err(PickError::Cancelled),
        code if code == sys::OPTICS_WINDOW_PICK_UNAVAILABLE => Err(PickError::Unavailable),
        code if code == sys::OPTICS_WINDOW_PICK_TRUNCATED => Err(PickError::Truncated),
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
    let opts = sys::optics_window_file_dialog_opts {
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
        let call_opts = sys::optics_window_file_dialog_opts {
            out_path: buf.as_mut_ptr().cast(),
            out_cap: buf.len(),
            out_bytes_used: &mut used,
            ..opts
        };
        let rc = unsafe { sys::optics_window_pick_files(&call_opts) };
        if rc == sys::OPTICS_WINDOW_PICK_TRUNCATED {
            let needed = used.max(cap + 1);
            if needed > 16 * 1024 * 1024 {
                return Err(PickError::Truncated);
            }
            cap = needed + 64; // terminator headroom
            continue;
        }
        if rc == sys::OPTICS_WINDOW_PICK_CANCELLED {
            return Err(PickError::Cancelled);
        }
        if rc == sys::OPTICS_WINDOW_PICK_UNAVAILABLE {
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
/// (`"*.txt;*.md"`). Mirrors `optics_window_file_filter` (all three backends
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

/// Builder over `optics_window_file_dialog_opts`, exposing everything the C struct
/// carries that the convenience functions don't: filters, the starting
/// folder, and multi-select. Build one with [`FileDialog::new`], then call
/// [`FileDialog::pick_path`] / [`FileDialog::pick_paths`] /
/// [`FileDialog::pick_save_path`].
///
/// ```no_run
/// use optics_window::{FileDialog, FileFilter};
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
/// array must outlive the `optics_window_pick_*` call, so they are held here next
/// to the ready-to-pass `opts`. Dropping the holder drops them — after
/// the call has returned.
struct Prepared {
    _title: Option<std::ffi::CString>,
    _uri: Option<std::ffi::CString>,
    _names: Vec<std::ffi::CString>,
    _pats: Vec<std::ffi::CString>,
    _filters: Vec<sys::optics_window_file_filter>,
    opts: sys::optics_window_file_dialog_opts,
}

impl<'a> FileDialog<'a> {
    /// Start building a file dialog with default options; finish with
    /// [`FileDialog::pick_path`](Self::pick_path). Runs the platform portal
    /// (Wayland), IFileDialog (Win32), or NSSavePanel (Cocoa).
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
            filters.push(sys::optics_window_file_filter {
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
        filters.push(sys::optics_window_file_filter {
            name: std::ptr::null(),
            pattern: std::ptr::null(),
        });
        let opts = sys::optics_window_file_dialog_opts {
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
            ..Default::default()
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
            code if code == sys::OPTICS_WINDOW_PICK_CANCELLED => Err(PickError::Cancelled),
            code if code == sys::OPTICS_WINDOW_PICK_UNAVAILABLE => Err(PickError::Unavailable),
            code if code == sys::OPTICS_WINDOW_PICK_TRUNCATED => Err(PickError::Truncated),
            _ => Err(PickError::Unavailable),
        }
    }

    /// Single-selection open. Returns the chosen `file://` URI; convert
    /// with [`file_uri_to_path`] for a filesystem path.
    pub fn pick_path(&self) -> Result<String, PickError> {
        let p = self.prepare();
        let mut buf = vec![0u8; 8192];
        let opts = sys::optics_window_file_dialog_opts {
            out_path: buf.as_mut_ptr().cast(),
            out_cap: buf.len(),
            ..p.opts
        };
        let rc = unsafe {
            if self.directory_only {
                sys::optics_window_pick_folder(&opts)
            } else {
                sys::optics_window_pick_file(&opts)
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
            let opts = sys::optics_window_file_dialog_opts {
                out_path: buf.as_mut_ptr().cast(),
                out_cap: buf.len(),
                out_bytes_used: &mut used,
                ..p.opts
            };
            let rc = unsafe { sys::optics_window_pick_files(&opts) };
            if rc == sys::OPTICS_WINDOW_PICK_TRUNCATED {
                let needed = used.max(cap + 1);
                if needed > 16 * 1024 * 1024 {
                    return Err(PickError::Truncated);
                }
                cap = needed + 64;
                continue;
            }
            if rc == sys::OPTICS_WINDOW_PICK_CANCELLED {
                return Err(PickError::Cancelled);
            }
            if rc == sys::OPTICS_WINDOW_PICK_UNAVAILABLE || rc < 0 {
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
        let opts = sys::optics_window_file_dialog_opts {
            default_name: name_c
                .as_ref()
                .map(|c| c.as_ptr())
                .unwrap_or(std::ptr::null()),
            out_path: buf.as_mut_ptr().cast(),
            out_cap: buf.len(),
            ..p.opts
        };
        let rc = unsafe { sys::optics_window_pick_save_path(&opts) };
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
    DragSource,
    ActivationToken,
}

impl Capability {
    fn raw(self) -> sys::optics_window_capability {
        use sys::optics_window_capability as C;
        match self {
            Self::WindowControl => C::OPTICS_WINDOW_CAP_WINDOW_CONTROL,
            Self::ThemeWatch => C::OPTICS_WINDOW_CAP_THEME_WATCH,
            Self::A11y => C::OPTICS_WINDOW_CAP_A11Y,
            Self::FileDialog => C::OPTICS_WINDOW_CAP_FILE_DIALOG,
            Self::Clipboard => C::OPTICS_WINDOW_CAP_CLIPBOARD,
            Self::PrimarySelection => C::OPTICS_WINDOW_CAP_PRIMARY_SELECTION,
            Self::Tablet => C::OPTICS_WINDOW_CAP_TABLET,
            Self::DropTarget => C::OPTICS_WINDOW_CAP_DROP_TARGET,
            Self::Decorations => C::OPTICS_WINDOW_CAP_DECORATIONS,
            Self::FractionalScale => C::OPTICS_WINDOW_CAP_FRACTIONAL_SCALE,
            Self::DragSource => C::OPTICS_WINDOW_CAP_DRAG_SOURCE,
            Self::ActivationToken => C::OPTICS_WINDOW_CAP_ACTIVATION_TOKEN,
        }
    }
}

/// Does this build implement `capability`? Static per-build answer,
/// callable before [`Application::run`], from any thread. Branch your UI
/// on this — not on platform names.
pub fn supports(capability: Capability) -> bool {
    unsafe { sys::optics_window_supports(capability.raw()) != 0 }
}

/// The backend this libiris was built with: `"wayland"`, `"win32"`,
/// `"cocoa"`, or `"none"`. For diagnostics; prefer [`supports`] for
/// behaviour.
pub fn backend_name() -> &'static str {
    unsafe {
        std::ffi::CStr::from_ptr(sys::optics_window_backend_name())
            .to_str()
            .unwrap_or("unknown")
    }
}

// =====================================================================
//  Drag-and-Drop (dnd.h, ADR-0086)
// =====================================================================

pub mod dnd {
    use super::*;

    /// Action bitflags for Drag-and-Drop.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct DndAction(pub u32);

    impl DndAction {
        pub const NONE: Self =
            Self(sys::optics_window_dnd_action::OPTICS_WINDOW_DND_ACTION_NONE as u32);
        pub const COPY: Self =
            Self(sys::optics_window_dnd_action::OPTICS_WINDOW_DND_ACTION_COPY as u32);
        pub const MOVE: Self =
            Self(sys::optics_window_dnd_action::OPTICS_WINDOW_DND_ACTION_MOVE as u32);
        pub const LINK: Self =
            Self(sys::optics_window_dnd_action::OPTICS_WINDOW_DND_ACTION_LINK as u32);
        pub const ASK: Self =
            Self(sys::optics_window_dnd_action::OPTICS_WINDOW_DND_ACTION_ASK as u32);
    }

    /// Why a drag session could not be started.
    ///
    /// `optics_window_dnd_start` collapses every failure to `-1`; the distinction that
    /// matters to a caller is whether the platform can drag at all
    /// ([`DndError::Unsupported`], the answer on a backend without a drag
    /// implementation) or whether an otherwise capable backend refused this
    /// particular session (no active window, a drag already in flight).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum DndError {
        /// No drag-and-drop implementation on this platform or build.
        Unsupported,
        /// The platform implements drag-and-drop but refused this session.
        Rejected,
    }

    impl std::fmt::Display for DndError {
        fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
            match self {
                DndError::Unsupported => {
                    write!(f, "drag-and-drop is not supported on this platform")
                }
                DndError::Rejected => write!(f, "the platform refused to start the drag session"),
            }
        }
    }

    impl std::error::Error for DndError {}

    /// Start an outgoing drag session.
    ///
    /// The payload is copied by the backend before this returns, so `text` is
    /// only borrowed for the duration of the call.
    pub fn start_drag(text: &str, actions: DndAction) -> Result<(), DndError> {
        let src = sys::optics_window_dnd_source {
            actions: actions.0,
            mime_types: std::ptr::null(),
            mime_count: 0,
            static_text: text.as_ptr() as *const std::ffi::c_char,
            static_text_len: text.len(),
            callbacks: unsafe { std::mem::zeroed() },
        };
        let rc = unsafe { sys::optics_window_dnd_start(&src) };
        if rc == 0 {
            Ok(())
        } else if supports(Capability::DragSource) {
            Err(DndError::Rejected)
        } else {
            Err(DndError::Unsupported)
        }
    }

    /// Query whether a drag session is currently active.
    pub fn is_active() -> bool {
        unsafe { sys::optics_window_dnd_is_active() }
    }

    /// Cancel any active drag session.
    pub fn cancel() {
        unsafe { sys::optics_window_dnd_cancel() }
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
        // Cargo workspace is 0.1.0; libiris is 0.0.30) — so we cannot
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
                assert!(supports(Capability::DropTarget));
                assert!(supports(Capability::DragSource));
                assert!(supports(Capability::ActivationToken));
            }
            "win32" => {
                assert!(!supports(Capability::PrimarySelection));
                assert!(supports(Capability::FractionalScale));
                assert!(supports(Capability::Decorations));
                assert!(!supports(Capability::A11y)); // stub, ADR-0056 D5
                assert!(supports(Capability::DropTarget));
                assert!(supports(Capability::DragSource));
                assert!(!supports(Capability::ActivationToken));
            }
            "cocoa" => {
                assert!(!supports(Capability::PrimarySelection));
                assert!(!supports(Capability::A11y)); // stub, ADR-0056 D5
                assert!(supports(Capability::Decorations));
                assert!(supports(Capability::DropTarget));
                assert!(supports(Capability::DragSource));
                assert!(!supports(Capability::ActivationToken));
            }
            _ => {
                assert!(!supports(Capability::WindowControl));
                assert!(!supports(Capability::FileDialog));
                assert!(!supports(Capability::DropTarget));
                assert!(!supports(Capability::DragSource));
                assert!(!supports(Capability::ActivationToken));
            }
        }
    }

    #[test]
    fn pick_error_codes_are_the_documented_sentinels() {
        // The C contract fixes these ABI values; a renumbered enum here
        // would silently mis-classify every picker failure.
        assert_eq!(sys::OPTICS_WINDOW_PICK_CANCELLED, -1);
        assert_eq!(sys::OPTICS_WINDOW_PICK_UNAVAILABLE, -2);
        assert_eq!(sys::OPTICS_WINDOW_PICK_TRUNCATED, -3);
    }

    #[test]
    fn dnd_calls_without_an_active_app_are_safe() {
        assert!(!dnd::is_active());
        dnd::cancel();
        assert!(dnd::start_drag("hello", dnd::DndAction::COPY).is_err());
    }

    #[test]
    fn window_calls_without_an_active_app_are_safe_noops() {
        // Thread-affine but inert outside a run loop — the documented
        // degradation. Calling them must not crash.
        window_close();
        window_start_move();
        window_minimize();
        window_restore();
        window_fullscreen();
        window_windowed();
        window_focus();
        window_set_min_size(200, 100);
        window_set_max_size(4096, 4096);
        assert_eq!(window_get_geometry(), None);
        assert_eq!(window_create_activation_token(None), None);
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
    #[ignore = "spawns interactive host file dialog; run with --ignored in an interactive session"]
    fn pick_save_path_interactive_smoke() {
        let _ = pick_save_path(Some("t"), Some("out.txt"));
    }

    #[test]
    fn pick_save_path_null_buffer_rejects_without_portal() {
        // Pickers reject NULL buffers / zero capacity before contacting the
        // portal (matching the C contract in tests/iris/test_app_api.c).
        // A valid buffer in an active desktop session would pop up a live picker.
        let opts = sys::optics_window_file_dialog_opts {
            title: std::ptr::null(),
            ..Default::default()
        };
        let rc = unsafe { sys::optics_window_pick_save_path(&opts) };
        assert_eq!(rc, sys::OPTICS_WINDOW_PICK_UNAVAILABLE);
    }

    #[test]
    fn file_dialog_builders_set_fields() {
        let filter = FileFilter {
            name: Some("Text files"),
            pattern: "*.txt",
        };
        let dlg = FileDialog::new()
            .title("Save report")
            .initial_uri("file:///tmp")
            .filter(filter)
            .multiple(true)
            .directory_only(true);
        assert_eq!(dlg.title, Some("Save report"));
        assert_eq!(dlg.initial_uri, Some("file:///tmp"));
        assert_eq!(dlg.filters.len(), 1);
        assert_eq!(dlg.filters[0].name, Some("Text files"));
        assert_eq!(dlg.filters[0].pattern, "*.txt");
        assert!(dlg.multiple);
        assert!(dlg.directory_only);
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
        use sys::optics_window_color_scheme::*;
        assert_eq!(
            ColorScheme::from(OPTICS_WINDOW_COLOR_SCHEME_NO_PREFERENCE),
            ColorScheme::NoPreference
        );
        assert_eq!(
            ColorScheme::from(OPTICS_WINDOW_COLOR_SCHEME_PREFER_DARK),
            ColorScheme::PreferDark
        );
        assert_eq!(
            ColorScheme::from(OPTICS_WINDOW_COLOR_SCHEME_PREFER_LIGHT),
            ColorScheme::PreferLight
        );
    }

    #[test]
    fn color_scheme_derives_eq() {
        assert_eq!(ColorScheme::PreferDark, ColorScheme::PreferDark);
        assert_ne!(ColorScheme::PreferDark, ColorScheme::PreferLight);
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
}
