//! Optics — high-performance modern graphics and UI engine.
//!
//! # Canonical Semantic Modules (ADR-0170)
//! - `gfx`: 2D vector canvas, Vulkan 1.3 RHI, 3D draw core, compute, CPU fallback.
//! - `text`: HarfBuzz text shaping, layout metrics, dynamic font atlas.
//! - `svg`: SVG vector document static-art compiler.
//! - `material`: Dielectric Liquid Glass, Frosted Glass, Acrylic, Mica BRDFs.
//! - `scene`: 3D scene graph, camera hierarchy, and glTF loading.
//! - `motion`: Closed-form analytic springs and parametric easing.
//! - `window`: Native OS windowing (Wayland, Win32, Cocoa), display pacing, event pump.
//! - `ui`: Headless immediate-mode UI engine, Flex/Grid layout, retained store.
//! - `app`: Application shell binding `ui` to `window`.

#[cfg(feature = "gfx")]
pub use optics_gfx as gfx;

#[cfg(feature = "text")]
pub use optics_text as text;

#[cfg(feature = "svg")]
pub use optics_svg as svg;

#[cfg(feature = "material")]
pub use optics_material as material;

#[cfg(feature = "scene")]
pub use optics_scene as scene;

#[cfg(feature = "motion")]
pub use optics_motion as motion;

#[cfg(feature = "window")]
pub use optics_window as window;

#[cfg(feature = "ui")]
pub use optics_ui as ui;

#[cfg(feature = "app")]
pub use optics_app as app;
