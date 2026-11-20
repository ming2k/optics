//! Raw FFI bindings to `libaperture`.
//!
//! Generated at build time by bindgen from `<aperture/aperture.h>`. The flux,
//! iris and lens types its API speaks are re-exported from their own -sys
//! crates, not duplicated. This crate is `unsafe` by nature; prefer the safe
//! `optics-aperture` wrapper crate for application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
