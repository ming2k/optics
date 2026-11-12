//! Raw FFI bindings to `liblens`.
//!
//! Generated at build time by bindgen from `<lens/lens.h>`, so the bindings
//! always match the C header pkg-config resolves. Set `LENS_SOURCE_DIR` to
//! pin a specific checkout. The flux types lens's API speaks (`flux_canvas`,
//! `flux_color`, `flux_rect`, ...) are NOT duplicated here: they are
//! re-exported from `flux_sys` (via a build-time `raw_line`), so there is
//! one definition of each across the stack. This crate is `unsafe` by
//! nature; prefer the safe `lens` wrapper crate for application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
