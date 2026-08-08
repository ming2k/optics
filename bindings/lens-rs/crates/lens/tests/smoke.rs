//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liblens + libflux, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use lens::{
    Align, Color, ForegroundOutline, Input, LayoutOpts, MouseButton, TabStyle, TableColumn,
    TableOpts, TabsOpts, TextBuf, Theme, Ui,
};

#[test]
fn register_svg_icon_accepts_valid_and_rejects_garbage() {
    let svg = "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 24 24\" fill=\"none\" \
               stroke=\"currentColor\" stroke-width=\"2\"><circle cx=\"12\" cy=\"12\" r=\"9\"/>\
               <line x1=\"12\" y1=\"7\" x2=\"12\" y2=\"13\"/></svg>";
    let id = lens::register_svg_icon(svg).expect("valid svg registers");
    // Runtime ids continue where the built-in enum ends.
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
        f.column(|f| {
            f.title("Settings");
            f.label("A label");
            let outline = ForegroundOutline::new(Color::rgba(0, 0, 0, 160), 0.75);
            f.label_compact_outlined_sized("12:34", 14.0, outline);
            f.icon_outlined(lens::Icon::Globe, 16.0, outline);
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.switch("Compact mode", &mut wrap);
            let response = f.setting_switch(
                "tap",
                "Tap to click",
                "Tap with one finger",
                &mut wrap,
                false,
            );
            assert!(!response.changed);
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
            f.tabs_ex(
                "connected-settings",
                &mut active,
                &TabsOpts {
                    style: TabStyle::Connected,
                    ..TabsOpts::default()
                },
                |f| {
                    f.tab("Primary");
                    f.tab("Secondary");
                    f.tab("Tertiary");
                },
            );
            f.tabs_ex(
                "indicator-settings",
                &mut active,
                &TabsOpts {
                    style: TabStyle::Indicator,
                    equal_width: true,
                    ..TabsOpts::default()
                },
                |f| {
                    f.tab("Recent");
                    f.tab("Pending");
                    f.tab("Completed");
                },
            );
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
fn safe_table_virtualizes_rust_cells() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((640.0, 480.0), 1.0 / 60.0);
    let columns = [
        TableColumn {
            title: "Title",
            width: 0.0,
            align: Align::Start,
        },
        TableColumn {
            title: "Time",
            width: 64.0,
            align: Align::End,
        },
    ];
    let mut requested = 0_usize;
    ui.frame(&input, |frame| {
        frame.size_next(600.0, 300.0);
        let result = frame.table(
            "library",
            &columns,
            10_000,
            TableOpts::default(),
            |row, column| {
                requested += 1;
                format!("{row}:{column}")
            },
        );
        assert_eq!(result.selected, None);
    });
    assert!(requested < 100, "table requested {requested} cells");
}

#[test]
fn safe_table_wheel_down_advances_rows() {
    let mut ui = Ui::headless().expect("create headless ui");
    let columns = [TableColumn {
        title: "Title",
        width: 0.0,
        align: Align::Start,
    }];
    let mut visible_rows = Vec::new();
    for _ in 0..2 {
        visible_rows.clear();
        let input = Input::new((480.0, 320.0), 1.0 / 60.0);
        ui.frame(&input, |frame| {
            frame.size_next(440.0, 240.0);
            frame.table(
                "scroll-direction",
                &columns,
                1_000,
                TableOpts {
                    row_height: 28.0,
                    ..TableOpts::default()
                },
                |row, _| {
                    visible_rows.push(row);
                    row.to_string()
                },
            );
        });
    }
    visible_rows.clear();
    let mut wheel_down = Input::new((480.0, 320.0), 1.0 / 60.0);
    wheel_down.set_cursor(100.0, 100.0).set_scroll(0.0, -8.0);
    ui.frame(&wheel_down, |frame| {
        frame.size_next(440.0, 240.0);
        frame.table(
            "scroll-direction",
            &columns,
            1_000,
            TableOpts {
                row_height: 28.0,
                ..TableOpts::default()
            },
            |row, _| {
                visible_rows.push(row);
                row.to_string()
            },
        );
    });
    assert!(
        visible_rows
            .iter()
            .copied()
            .min()
            .is_some_and(|row| row > 0),
        "wheel-down must reveal later rows, got {visible_rows:?}"
    );
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
fn layout_constraints_and_text_metrics_are_available_safely() {
    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((400.0, 200.0), 1.0 / 60.0);

    ui.frame(&input, |f| {
        let metrics = f.measure_text("adaptive rail", f.theme().font_size());
        assert!(metrics.width > 0.0);
        assert!(metrics.height > 0.0);
        assert!(f.theme().padding() >= 0.0);

        f.column_ex(
            &LayoutOpts {
                min_width: 120.0,
                max_width: 180.0,
                min_height: 40.0,
                max_height: 160.0,
                ..LayoutOpts::default()
            },
            |f| f.label("content"),
        );
    });
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
fn rgba_and_with_alpha_preserve_premultiplied_colour_contract() {
    let translucent = Color::rgba(240, 120, 60, 32);
    let (r, g, b, a) = translucent.components();
    assert_eq!(a, 32);
    assert!(r <= a && g <= a && b <= a);

    let retinted = translucent.with_alpha(128);
    let (r, g, b, a) = retinted.components();
    assert_eq!(a, 128);
    assert!(
        (118..=122).contains(&r),
        "red should preserve its straight colour"
    );
    assert!(
        (58..=62).contains(&g),
        "green should preserve its straight colour"
    );
    assert!(
        (28..=32).contains(&b),
        "blue should preserve its straight colour"
    );
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
        .set_scroll_pixels(1.5, -2.5)
        .set_text("hi")
        .push_key(lens::key::RETURN, true, false);

    let raw = input.as_raw();
    assert_eq!(raw.cursor.x, 10.0);
    assert_eq!(raw.key_count, 1);
    assert_eq!(raw.scroll_pixels_x, 1.5);
    assert_eq!(raw.scroll_pixels_y, -2.5);

    // Feed it through a real frame; must not crash.
    let mut ui = Ui::headless().unwrap();
    ui.frame(&input, |f| {
        f.button("Click");
    });
}
