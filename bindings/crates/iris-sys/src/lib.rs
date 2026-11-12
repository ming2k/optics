//! Raw FFI bindings to `libiris`.
//!
//! Generated at build time by bindgen from `<iris/iris.h>`. Set
//! `IRIS_SOURCE_DIR` to pin a specific source checkout. The flux/lens
//! types iris's API speaks (`flux_canvas`, `flux_device`, `lens`,
//! `lens_input`) are NOT duplicated here: they are re-exported from
//! `flux_sys`/`lens_sys`, so there is one definition of each across the
//! stack. This crate is `unsafe` by nature; prefer the safe `iris`
//! wrapper crate for application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
