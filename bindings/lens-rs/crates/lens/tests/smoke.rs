//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liblens + libflux, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use lens::{Input, TextBuf, Ui};

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

#[test]
fn headless_frame_drives_grid_and_nested_flex() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    ui.frame(&input, |f| {
        // Outer 1D Column
        f.column().gap(16.0).show_flat(|f| {
            f.label("Dashboard");

            // 2D Grid with 3 columns, Bento layout
            f.grid(3).col_gap(10.0).row_gap(10.0).pad(12.0).show(|f| {
                // Card 1: 2 cols x 1 row with nested 1D Row
                f.column().col_span(2).row_span(1).show_flat(|f| {
                    f.label("Hero Card (2x1)");
                    f.row().show_flat(|f| {
                        f.button("Action A");
                        f.button("Action B");
                    });
                });

                // Card 2: 1 col x 2 rows
                f.column().col_span(1).row_span(2).show_flat(|f| {
                    f.label("Tall Card (1x2)");
                });

                // Card 3: 1 col x 1 row
                f.column().col_span(1).row_span(1).show_flat(|f| {
                    f.label("Tile 1");
                });

                // Card 4: 1 col x 1 row
                f.column().col_span(1).row_span(1).show_flat(|f| {
                    f.label("Tile 2");
                });
            });
        });
    });
}

#[test]
fn theme_minimalist_defaults_and_classic() {
    let def = lens::Theme::default();
    let dark = lens::Theme::dark();
    let classic_light = lens::Theme::classic(false);
    let classic_dark = lens::Theme::classic(true);

    assert_eq!(def.border_width(), 0.0);
    assert_eq!(dark.border_width(), 0.0);
    assert_eq!(classic_light.border_width(), 1.0);
    assert_eq!(classic_dark.border_width(), 1.0);

    // Semantic surface tokens
    assert_ne!(def.surface().raw(), 0);
    assert_ne!(def.surface_sunken().raw(), 0);
    assert_ne!(def.surface_elevated().raw(), 0);
    assert_ne!(dark.surface().raw(), 0);
    assert_ne!(dark.surface_sunken().raw(), 0);
    assert_ne!(dark.surface_elevated().raw(), 0);
}

#[test]
fn headless_frame_drives_wrap_and_stack_containers() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    ui.frame(&input, |f| {
        // Flow wrap layout
        f.wrap().gap(8.0).row_gap(10.0).show_flat(|f| {
            f.button("Tag 1");
            f.button("Tag 2");
            f.button("Tag 3");
        });

        // Layering stack layout
        f.stack().pad(4.0).show_flat(|f| {
            f.label("Base Background");
            f.button("Overlay Action");
        });
    });
}
