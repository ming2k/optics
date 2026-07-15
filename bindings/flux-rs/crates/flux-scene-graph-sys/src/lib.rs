//! Raw FFI bindings to `libflux-scene-graph`.
//!
//! Generated at build time from `<flux-scene-graph/scene-graph.h>`, so the
//! bindings match the C library selected by pkg-config. Prefer the safe
//! `flux-scene-graph` wrapper for application code.

#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
