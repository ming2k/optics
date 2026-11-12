//! Raw FFI bindings to `libprism`.
//!
//! Generated at build time by bindgen from `<prism/prism.h>`, so the bindings
//! always match the C header pkg-config resolves. Set `PRISM_SOURCE_DIR` to
//! pin a specific checkout. The flux types prism's API speaks (`flux_image`,
//! `flux_result`, ...) are NOT duplicated here: they are re-exported from
//! [`flux_sys`], so there is one definition of each across the stack. This
//! crate is `unsafe` by nature; prefer the safe `prism` wrapper crate for
//! application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
