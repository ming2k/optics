//! Raw FFI bindings to `libflux`.
//!
//! Generated at build time by bindgen from the flux public headers
//! (`core.h`, `math.h`, `canvas.h`, `vulkan.h`, `dmabuf.h`, `scene.h`,
//! `compute.h`, `effect.h`), so the bindings always match the C headers
//! pkg-config resolves. Set `FLUX_SOURCE_DIR` to pin a specific checkout.
//! This crate is `unsafe` by nature; prefer the safe `flux` wrapper crate
//! for application code.
#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]
#![allow(dead_code)]
#![allow(clippy::all)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
