//! Raw FFI bindings to `liboptics_material`.
//!
//! Generated at build time by bindgen from `<optics/material.h>`, so the bindings
//! always match the C header pkg-config resolves. Set `OPTICS_MATERIAL_SOURCE_DIR` to
//! pin a specific checkout. The gfx types material's API speaks (`optics_gfx_image`,
//! `optics_gfx_result`, ...) are NOT duplicated here: they are re-exported from
//! [`optics_gfx_sys`], so there is one definition of each across the stack. This
//! crate is `unsafe` by nature; prefer the safe `optics-material` wrapper crate for
//! application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
