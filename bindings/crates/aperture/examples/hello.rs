//! A real window driven entirely from Rust via aperture.
//!
//! Requires a Wayland compositor, a Vulkan-capable GPU, and a built or
//! installed optics tree (libflux, liblens, libiris, libaperture). State lives in the closure's captured variables across frames.

use aperture::{Application, Config, iris};

fn main() -> Result<(), aperture::RunError> {
    let mut wrap = true;
    let mut zoom = 1.5f32;

    println!(
        "iris {} — Wayland shell. System colour scheme: {:?}",
        iris::version(),
        iris::query_system_color_scheme()
    );

    let cfg = Config::new("iris — hello")?.size(720, 480);
    Application::run(cfg, |f, _| {
        f.column().show(|f| {
            f.label("Hello, iris");
            f.label("Cross-platform L3 toolkit for the flux/lens stack");
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.slider("Zoom", &mut zoom, 0.5, 4.0);

            if f.button("Pick a file") {
                match iris::pick_file(Some("Pick")) {
                    Some(uri) => println!("picked: {uri}"),
                    None => println!("cancelled"),
                }
            }
        });
    })
}
