//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liblens + libflux, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use lens::{
    Input, TextBuf, Ui,
};

#[test]
fn register_svg_icon_accepts_valid_and_rejects_garbage() {
    let svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" \
               stroke=\"currentColor\" stroke-width=\"2\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>\
               <line x1=\"12\" y1=\"7\" x2=\"12\" y2=\"13\"/></svg>";
    let id = lens::register_svg_icon(svg).expect("valid svg registers");
    assert!(id.0 >= lens::sys::lens_icon_id::LENS_ICON_COUNT.0);
    assert!(lens::register_svg_icon("this is not svg <<<").is_none());
    assert!(lens::register_svg_icon("<svg viewBox=\"0 0 24 24\"></svg>").is_none());
}

#[test]
fn version_links_and_runs() {
    let v = lens::version();
    assert!(!v.is_empty(), "version string should be non-empty");
    assert!(v.starts_with("0."), "unexpected version: {v}");
}

#[test]
fn headless_frame_drives_widgets() {
    let mut ui = Ui::headless().expect("create headless ui");

    let mut wrap = true;
    let mut zoom = 1.5f32;
    let mut choice = 0i32;
    let mut name = TextBuf::new(64, "flux");

    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    let clicked = ui.frame(&input, |f| {
        f.column().show_flat(|f| {
            f.label("Settings");
            f.label("A label");
            f.icon(lens::Icon::Globe, 16.0);
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.switch("Compact mode", &mut wrap);
            f.row().items_center().show_flat(|f| {
                f.col().show_flat(|f| {
                    f.label("Tap to click");
                    f.label_sized("Tap with one finger", 11.0);
                });
                f.flex(1.0);
                f.spacer(0.0);
                f.switch("tap-switch", &mut wrap);
            });
            f.slider("Zoom", &mut zoom, 0.5, 4.0);
            f.radio("Option A", &mut choice, 0);
            f.radio("Option B", &mut choice, 1);
            f.textfield("Name", &mut name);
            f.button("Save")
        })
    });
    assert!(!clicked, "no input was supplied, nothing should click");
    assert_eq!(name.as_str(), "flux");

    for _ in 0..3 {
        ui.frame(&input, |f| {
            f.button("Save");
        });
    }
}

#[test]
fn headless_frame_drives_containers() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    ui.frame(&input, |f| {
        f.column().show_flat(|f| {
            f.row().show_flat(|f| {
                f.selectable("General", true);
                f.selectable("Advanced", false);
                f.selectable("About", false);
            });

            f.scroll("list", |f| {
                f.column().show_flat(|f| {
                    for i in 0..10 {
                        f.label(&format!("item-{i}"));
                    }
                });
            });
        });
    });
}
