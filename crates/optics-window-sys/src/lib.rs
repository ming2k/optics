//! Raw FFI bindings to `liboptics_window`.
//!
//! Generated at build time by bindgen from `<optics/window.h>`. Set
//! `OPTICS_WINDOW_SOURCE_DIR` to pin a specific source checkout. The gfx types
//! window's API speaks (`optics_gfx_canvas`, `optics_gfx_device`, `optics_gfx_encoder`, ...) are
//! NOT duplicated here: they are re-exported from `optics_gfx_sys`, so there is
//! one definition of each across the stack. This crate is `unsafe` by nature; prefer the safe `optics-window`
//! wrapper crate for application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
