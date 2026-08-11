//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liblens + libflux, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use lens::{
    Align, Color, Input, LayoutOpts, MouseButton, TableColumn, TableOpts, TabsOpts, TextBuf, Theme,
    Ui,
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
            // The contour is a style atom now (ADR-0061): scope + plain calls.
            let outline = lens::Style::default()
                .with_outline_color(Color::rgba(0, 0, 0, 160))
                .with_outline_width(0.75);
            f.push_style(outline);
            f.label_compact_sized("12:34", 14.0);
            f.icon(lens::Icon::Globe, 16.0);
            f.pop_style();
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
                "wide-settings",
                &mut active,
                &TabsOpts { equal_width: true },
                |f| {
                    f.tab("Primary");
                    f.tab("Secondary");
                    f.tab("Tertiary");
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

            // Anchor a transient popup to the last widget; opens only after a
            // click, so here it simply must not crash.
            let anchor = f.response().rect;
            if f.place_is_open("menu") {
                let opts = lens::PlaceOpts {
                    rect: anchor,
                    ..lens::PlaceOpts::default()
                };
                f.place("menu", &opts, |f| {
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
fn table_ex_keyboard_drives_cursor_and_activation() {
    let mut ui = Ui::headless().expect("create headless ui");
    let columns = [TableColumn {
        title: "Name",
        width: 0.0,
        align: Align::Start,
    }];
    let opts = TableOpts {
        row_height: 28.0,
        keyboard: true,
        ..TableOpts::default()
    };
    let idle = Input::new((480.0, 320.0), 1.0 / 60.0);
    let mut cursor = -1i32;

    let build = |f: &mut lens::Frame, cursor: &mut i32| {
        f.size_next(440.0, 240.0);
        f.table_ex(
            "files",
            &columns,
            50,
            opts,
            |row, _| format!("row {row}"),
            |_, _| None,
            |row| row == 2,
            cursor,
        )
    };

    // Settle two frames so the table has hit-test geometry. The host-owned
    // selection callback keeps the retained store out of the result.
    let mut header_h = 0.0f32;
    for _ in 0..2 {
        let result = ui.frame(&idle, |f| {
            let theme = f.theme();
            header_h = theme.font_size() + 2.0 * theme.padding();
            build(f, &mut cursor)
        });
        assert_eq!(result.selected, None);
        assert!(!result.selection_changed);
        assert_eq!(result.cursor, None);
    }

    // Click row 1 to focus the table: reported via clicked_row only.
    let mut click = Input::new((480.0, 320.0), 1.0 / 60.0);
    click
        .set_cursor(20.0, header_h + 28.0 + 14.0)
        .set_mouse_down(MouseButton::Left, true)
        .set_mouse_pressed(MouseButton::Left, true);
    let result = ui.frame(&click, |f| build(f, &mut cursor));
    assert!(result.clicked);
    assert_eq!(result.clicked_row, Some(1));
    assert_eq!(result.selected, None);

    // Down from -1 lands on the first row and writes back through `cursor`.
    let mut down = idle.clone();
    down.push_key(lens::key::DOWN, true, false);
    let result = ui.frame(&down, |f| build(f, &mut cursor));
    assert_eq!(result.cursor, Some(0));
    assert!(result.cursor_changed);
    assert_eq!(cursor, 0);

    let result = ui.frame(&down, |f| build(f, &mut cursor));
    assert_eq!(result.cursor, Some(1));

    // Return activates the cursor row.
    let mut enter = idle.clone();
    enter.push_key(lens::key::RETURN, true, false);
    let result = ui.frame(&enter, |f| build(f, &mut cursor));
    assert!(result.activated);
    assert_eq!(result.cursor, Some(1));

    // End jumps to the last row, Up steps back.
    let mut end = idle.clone();
    end.push_key(lens::key::END, true, false);
    let result = ui.frame(&end, |f| build(f, &mut cursor));
    assert_eq!(result.cursor, Some(49));
    let mut up = idle.clone();
    up.push_key(lens::key::UP, true, false);
    let result = ui.frame(&up, |f| build(f, &mut cursor));
    assert_eq!(result.cursor, Some(48));
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

#[test]
fn textfield_host_caret_and_selection() {
    let mut ui = Ui::headless().expect("create headless ui");
    let mut buf = TextBuf::new(64, "ac");
    let idle = Input::new((400.0, 200.0), 1.0 / 60.0);

    // Frame 1: caret set before the field's first-ever build — the
    // find-or-create touch must remember it until the field appears.
    ui.frame(&idle, |f| {
        f.textfield_set_caret("tf", 1);
        f.textfield("tf", &mut buf);
    });

    // Frame 2: click inside the field to focus it (click grants focus; the
    // press also places the caret at the click point — near the end here).
    let mut click = Input::new((400.0, 200.0), 1.0 / 60.0);
    click
        .set_cursor(60.0, 20.0)
        .set_mouse_down(MouseButton::Left, true)
        .set_mouse_pressed(MouseButton::Left, true);
    ui.frame(&click, |f| {
        f.textfield("tf", &mut buf);
    });

    // Frame 3: release the button.
    let mut release = Input::new((400.0, 200.0), 1.0 / 60.0);
    release
        .set_cursor(60.0, 20.0)
        .set_mouse_released(MouseButton::Left, true);
    ui.frame(&release, |f| {
        f.textfield("tf", &mut buf);
    });

    // Frame 4: move the caret back to byte 1 and type in the same frame —
    // the host write wins over the click-placed caret, so 'b' lands at 1.
    let mut typing = Input::new((400.0, 200.0), 1.0 / 60.0);
    typing.set_text("b");
    let changed = ui.frame(&typing, |f| {
        f.textfield_set_caret("tf", 1);
        f.textfield("tf", &mut buf)
    });
    assert!(changed, "typing 'b' should edit the buffer");
    assert_eq!(buf.as_str(), "abc");

    // Frame 5: select-all (anchor 0, caret u32::MAX clamps to the length),
    // then a typed char replaces the whole buffer.
    let mut typing = Input::new((400.0, 200.0), 1.0 / 60.0);
    typing.set_text("x");
    let changed = ui.frame(&typing, |f| {
        f.textfield_set_selection("tf", 0, u32::MAX);
        f.textfield("tf", &mut buf)
    });
    assert!(changed, "typing 'x' should replace the selection");
    assert_eq!(buf.as_str(), "x");

    // The scoped variant addresses a field built under push_id; drive it
    // once to prove the push/call/pop wiring.
    ui.frame(&idle, |f| {
        f.push_id("scope");
        f.textfield("inner", &mut buf);
        f.pop_id();
    });
    ui.frame(&idle, |f| {
        f.textfield_scoped_set_selection("scope", "inner", 0, 1);
        f.push_id("scope");
        f.textfield("inner", &mut buf);
        f.pop_id();
    });
}
