//! Error codes and the thread-local diagnostic.

use super::*;

/// A gfx result code surfaced as a Rust error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Error(pub sys::optics_gfx_result);

impl Error {
    /// Map a raw result to `Ok(())` on `OPTICS_GFX_OK`, else `Err`.
    pub(crate) fn check(rc: sys::optics_gfx_result) -> Result<(), Error> {
        if rc == sys::optics_gfx_result::OPTICS_GFX_OK {
            Ok(())
        } else {
            Err(Error(rc))
        }
    }

    /// Wrap a raw `optics_gfx_result` as this crate's error. Public so sibling
    /// crates in the stack (gfx-text, gfx-scene-graph, …) reuse the one
    /// error type instead of declaring structurally identical copies.
    pub fn from_raw(rc: sys::optics_gfx_result) -> Error {
        Error(rc)
    }

    /// `Ok(())` when `rc` is `OPTICS_GFX_OK`, else `Err(Self::from_raw(rc))`.
    /// The public form of the internal check used at every FFI edge.
    pub fn check_raw(rc: sys::optics_gfx_result) -> Result<(), Error> {
        Error::check(rc)
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        // optics_gfx_result_string returns a static C string.
        let s = unsafe { std::ffi::CStr::from_ptr(sys::optics_gfx_result_string(self.0)) };
        write!(f, "gfx error: {}", s.to_string_lossy())?;
        // Attach the thread-local diagnostic when it matches this code, so
        // a logged error carries the same context the C examples print
        // (function/file/line/message). The strings are owned by gfx and
        // valid until the next error on this thread — copying them here
        // keeps Display free of lifetime surprises.
        if let Some(info) = ErrorInfo::last_if(Some(self.0)) {
            write!(f, " ({info})")?;
        }
        Ok(())
    }
}

impl std::error::Error for Error {}

/// Structured diagnostic for the most recent gfx error on this thread —
/// the owned mirror of C's `optics_gfx_error_info` (string fields are copied, so
/// the value outlives the next gfx call). Thread-local by contract.
///
/// Prefer [`Error::last_info`] / the automatic context [`Error`]'s `Display`
/// appends; call this directly only when you need the structured fields
/// (e.g. routing `backend_code` to a VkResult table).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ErrorInfo {
    pub code: sys::optics_gfx_result,
    /// gfx entry point that failed, e.g. `"optics_gfx_device_create"`.
    pub function: Option<String>,
    /// Source file inside gfx.
    pub file: Option<String>,
    pub line: i32,
    /// Human-readable detail.
    pub message: Option<String>,
    /// Backend result (e.g. `VkResult` as i32) when relevant.
    pub backend_code: i32,
}

impl ErrorInfo {
    /// Snapshot the thread-local diagnostic. `None` when no error has been
    /// recorded on this thread (or since the last successful call, per
    /// gfx's lifetime rules — treat `None` as "no context available").
    pub fn last() -> Option<ErrorInfo> {
        Self::last_if(None)
    }

    /// The diagnostic recorded for `code`, if the thread slot still describes a
    /// call that failed with it. Declining a mismatched code is what keeps a
    /// stale entry from a previous, unrelated failure out of a new error.
    ///
    /// Public because a binding that converts a raw `optics_gfx_result` into its own
    /// error type has to read the detail at the failure site — the slot is
    /// overwritten by the next failing call, so it cannot be recovered later.
    pub fn last_if_code(code: sys::optics_gfx_result) -> Option<ErrorInfo> {
        Self::last_if(Some(code))
    }

    fn last_if(code: Option<sys::optics_gfx_result>) -> Option<ErrorInfo> {
        let mut raw = sys::optics_gfx_error_info {
            code: sys::optics_gfx_result::OPTICS_GFX_OK,
            function: std::ptr::null(),
            file: std::ptr::null(),
            line: 0,
            message: std::ptr::null(),
            backend_code: 0,
        };
        // SAFETY: out is a valid, fully initialized optics_gfx_error_info slot.
        unsafe { sys::optics_gfx_get_last_error(&mut raw) };
        if raw.code == sys::optics_gfx_result::OPTICS_GFX_OK {
            return None;
        }
        if let Some(want) = code {
            if raw.code != want {
                return None; // stale diagnostic from a different call
            }
        }
        let cstr = |p: *const std::os::raw::c_char| unsafe {
            if p.is_null() {
                None
            } else {
                Some(std::ffi::CStr::from_ptr(p).to_string_lossy().into_owned())
            }
        };
        Some(ErrorInfo {
            code: raw.code,
            function: cstr(raw.function),
            file: cstr(raw.file),
            line: raw.line,
            message: cstr(raw.message),
            backend_code: raw.backend_code,
        })
    }
}

impl fmt::Display for ErrorInfo {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{:?}", self.code)?;
        if let Some(func) = &self.function {
            write!(f, " at {func}")?;
            if let Some(file) = &self.file {
                write!(f, " ({file}:{})", self.line)?;
            }
        }
        if let Some(msg) = &self.message {
            write!(f, ": {msg}")?;
        }
        Ok(())
    }
}

impl Error {
    /// The structured thread-local diagnostic for this error, if one was
    /// recorded by the failing call. `None` when the thread slot is empty
    /// or holds a newer diagnostic for a different call.
    pub fn last_info(&self) -> Option<ErrorInfo> {
        ErrorInfo::last_if(Some(self.0))
    }
}

/* ================================================================== */
/*  Color management (ADR-0069/0070)                                  */
/* ================================================================== */
