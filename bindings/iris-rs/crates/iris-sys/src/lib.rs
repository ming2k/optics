//! Raw FFI bindings to `libiris`.
//!
//! Generated at build time by bindgen from `<iris/iris.h>`. Set
//! `IRIS_SOURCE_DIR` to pin a specific source checkout. This crate is
//! `unsafe` by nature; prefer the safe `iris` wrapper crate for
//! application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
