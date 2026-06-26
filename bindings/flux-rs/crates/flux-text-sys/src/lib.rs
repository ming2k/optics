//! Raw FFI bindings to `libflux-text`.
//!
//! Generated at build time by bindgen from `<flux-text/text.h>`, so the
//! bindings always match the C header pkg-config resolves. Set
//! `FLUX_SOURCE_DIR` to pin a specific checkout. This crate is `unsafe`
//! by nature; prefer the safe `flux-text` wrapper crate.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
