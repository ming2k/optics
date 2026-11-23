//! A real window driven entirely from Rust via app.
//!
//! Requires a Wayland compositor, a Vulkan-capable GPU, and a built or
//! installed optics tree (liboptics_gfx, liboptics_ui, liboptics_window, liboptics_app). State lives in the closure's captured variables across frames.

use optics_app::{Application, Config, window};

fn main() -> Result<(), optics_app::RunError> {
    let mut wrap = true;
    let mut zoom = 1.5f32;

    println!(
        "window {} — Wayland shell. System colour scheme: {:?}",
        window::version(),
        window::query_system_color_scheme()
    );

    let cfg = Config::new("window — hello")?.size(720, 480);
    Application::run(cfg, |f, _| {
        f.column().show(|f| {
            f.label("Hello, window");
            f.label("Cross-platform L3 toolkit for the gfx/ui stack");
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.slider("Zoom", &mut zoom, 0.5, 4.0);

            if f.button("Pick a file") {
                match window::pick_file(Some("Pick")) {
                    Some(uri) => println!("picked: {uri}"),
                    None => println!("cancelled"),
                }
            }
        });
    })
}
