//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liblens + libflux, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use lens::{Align, Color, Input, LayoutOpts, MouseButton, TextBuf, Theme, Ui};

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
        f.column(|f| {
            f.title("Settings");
            f.label("A label");
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.slider("Zoom", &mut zoom, 0.5, 4.0);
            f.radio("Option A", &mut choice, 0);
            f.radio("Option B", &mut choice, 1);
            f.progress("Loading", 0.42);
            f.textfield("Name", &mut name);
            let mut clicked = false;
            f.collapsing("More", |f| {
                clicked = f.row(|f| f.button("Apply") || f.button("Reset"));
            });
            clicked
        })
    });
    assert!(!clicked, "no input was supplied, nothing should click");
    assert_eq!(name.as_str(), "flux");

    // Reconcile across several frames so the retained tree exercises its
    // entering/stable/leaving phases.
    for _ in 0..3 {
        ui.frame(&input, |f| {
            f.button("Save");
        });
    }
}

#[test]
fn headless_frame_drives_containers() {
    let mut ui = Ui::headless().expect("create headless ui");
    let mut active = 0i32;
    let mut theme = 1i32;
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    ui.frame(&input, |f| {
        f.column(|f| {
            f.tabs("settings", &mut active, |f| {
                f.tab("General");
                f.tab("Advanced");
                f.tab("About");
            });
            match active {
                0 => f.label("General"),
                1 => f.label("Advanced"),
                _ => f.label("About"),
            }

            f.dropdown("Theme", &mut theme, &["Dark", "Light", "System"]);

            f.size_next(0.0, 120.0);
            f.scroll("list", |f| {
                f.column(|f| {
                    for i in 0..10 {
                        f.button(&format!("Row {i}"));
                    }
                });
            });

            // Anchor an overlay to the last widget; opens only after a click,
            // so here it simply must not crash.
            let anchor = f.response().rect;
            if f.overlay_is_open("menu") {
                f.overlay("menu", anchor, &lens::OverlayOpts::default(), |f| {
                    f.button("Item");
                });
            }
        });
    });

    // caret rect query (no focused text widget -> zero-sized, must not crash).
    let _caret = ui.caret_rect();
    ui.paste("clip");
}

#[test]
fn headless_frame_drives_descriptor_containers() {
    // The *_ex containers carry gap / pad / cross / bg. Drive them headless so
    // the retained tree reconciles layout with the full option set.
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    let clicked = ui.frame(&input, |f| {
        let mut clicked = false;
        f.column_ex(
            &LayoutOpts {
                gap: 10.0,
                pad: 20.0,
                cross: Align::Center,
                ..LayoutOpts::default()
            },
            |f| {
                f.title("Card");
                f.label("Centred, with breathing room");
                f.row_ex(
                    &LayoutOpts {
                        gap: 8.0,
                        cross: Align::Center,
                        ..LayoutOpts::default()
                    },
                    |f| clicked = f.button("Yes") || f.button("No"),
                );
                // Fixed-width, surfaced, rounded nested container.
                f.column_ex(
                    &LayoutOpts {
                        gap: 6.0,
                        pad: 12.0,
                        width: 200.0,
                        bg: Color::rgba(255, 255, 255, 10),
                        radius: 8.0,
                        ..LayoutOpts::default()
                    },
                    |f| {
                        f.label("inner");
                    },
                );
            },
        );
        clicked
    });
    assert!(!clicked, "no input supplied, nothing should click");

    // Reconcile a couple more frames so the tree exercises stable/leaving.
    for _ in 0..2 {
        ui.frame(&input, |f| {
            f.row_ex(&LayoutOpts::default(), |f| {
                f.button("a");
            });
        });
    }
}

#[test]
fn theme_colours_adapt_to_light_and_dark() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((400.0, 300.0), 1.0 / 60.0);

    ui.set_theme(Theme::dark());
    ui.frame(&input, |f| {
        let t = f.theme();
        assert!(t.is_dark(), "dark theme should report is_dark");
        let (_r, _g, _b, a) = t.bg().components();
        assert!(a > 0, "bg alpha should be opaque");
        // Use the foreground colour as a theme-aware lift.
        let lift = t.fg().with_alpha(18);
        let (lr, lg, lb, lift_a) = lift.components();
        assert_eq!(lift_a, 18);
        // flux stores colours premultiplied, so the RGB components must be
        // scaled down by alpha/255 (allowing for integer rounding).
        assert!(lr <= 18, "premultiplied R should be <= alpha, got {lr}");
        assert!(lg <= 18, "premultiplied G should be <= alpha, got {lg}");
        assert!(lb <= 19, "premultiplied B should be ~= alpha, got {lb}");
    });

    ui.set_theme(Theme::light());
    ui.frame(&input, |f| {
        let t = f.theme();
        assert!(!t.is_dark(), "light theme should not report is_dark");
        let (fr, fg, fb, _) = t.fg().components();
        let (br, bg, bb, _) = t.bg().components();
        // Light theme: foreground is darker than background.
        let fluma = 0.299 * fr as f32 + 0.587 * fg as f32 + 0.114 * fb as f32;
        let bluma = 0.299 * br as f32 + 0.587 * bg as f32 + 0.114 * bb as f32;
        assert!(
            fluma < bluma,
            "light theme foreground should be darker than background"
        );
    });
}

#[test]
fn textbuf_set_replaces_and_truncates() {
    let mut buf = TextBuf::new(8, "hello");
    assert_eq!(buf.as_str(), "hello");
    // Shorter replacement leaves no stale tail.
    buf.set("hi");
    assert_eq!(buf.as_str(), "hi");
    // Truncation respects capacity (8 bytes incl. NUL => 7 chars max).
    buf.set("abcdefgh");
    assert_eq!(buf.as_str(), "abcdefg");
    // Clearing to empty.
    buf.set("");
    assert_eq!(buf.as_str(), "");
}

#[test]
fn input_builder_sets_fields() {
    let mut input = Input::new((1024.0, 768.0), 0.016);
    input
        .set_cursor(10.0, 20.0)
        .set_mouse_pressed(MouseButton::Left, true)
        .set_scroll(0.0, -3.0)
        .set_text("hi")
        .push_key(lens::key::RETURN, true, false);

    let raw = input.as_raw();
    assert_eq!(raw.cursor.x, 10.0);
    assert_eq!(raw.key_count, 1);

    // Feed it through a real frame; must not crash.
    let mut ui = Ui::headless().unwrap();
    ui.frame(&input, |f| {
        f.button("Click");
    });
}
