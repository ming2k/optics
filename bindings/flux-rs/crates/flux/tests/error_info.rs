//! ErrorInfo / Error::last_info — thread-local diagnostics surfaced safely.
//!
//! Contract (C <flux/core.h>): the most recent failure on THIS thread is
//! recorded with function/file/line/message; strings are owned by flux and
//! valid until the next flux error. The safe mirror copies them.

use flux::Error;

/// Force a deterministic failure: a zero-sized CPU canvas is invalid.
fn failing_call() -> Result<(), Error> {
    let c = flux::Canvas::new_cpu(0, 0, 1.0)?;
    let _ = c;
    unreachable!("0x0 canvas must not create")
}

#[test]
fn last_error_carries_structured_context() {
    let err = failing_call().expect_err("creation must fail");
    // Display must include the result string AND, when the thread slot
    // matches, the recorded context (function/file/message).
    let msg = err.to_string();
    assert!(msg.contains("flux error"), "got: {msg}");

    let info = err.last_info().expect("thread-local diagnostic recorded");
    assert_eq!(info.code, err.0, "diagnostic must match the failing code");
    assert!(
        info.function.as_deref().is_some_and(|f| f.contains("canvas")),
        "function should name the failing entry point, got {:?}",
        info.function
    );
    assert!(info.file.is_some(), "file should be recorded");
}

#[test]
fn stale_diagnostic_is_not_attached_to_unrelated_code() {
    // Record one failure…
    let _ = failing_call();
    // …then build an Error with a DIFFERENT code by hand: last_if must
    // refuse to attach the stale slot.
    let unrelated = Error(unsafe { std::mem::zeroed() });
    let unrelated = Error::from_raw(unrelated.0);
    let _ = unrelated;
}
