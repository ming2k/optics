//! End-to-end smoke test: proves the whole chain works — bindgen generated the
//! bindings, the linker resolved liboptics_ui + liboptics_gfx, and a real headless
//! frame drives the widget set through the safe wrapper. No GPU required.

use optics_ui::{Input, TextBuf, Ui};

#[test]
fn register_vector_icon_accepts_valid_and_rejects_empty() {
    let mut ui = Ui::headless().expect("create headless ui");
    let cmds = [
        optics_ui::sys::optics_ui_icon_cmd {
            kind: optics_ui::sys::optics_ui_icon_cmd_kind::OPTICS_UI_ICON_CMD_CIRCLE,
            _pad: [0; 3],
            params: [12.0, 12.0, 9.0, 0.0, 0.0, 0.0],
        },
        optics_ui::sys::optics_ui_icon_cmd {
            kind: optics_ui::sys::optics_ui_icon_cmd_kind::OPTICS_UI_ICON_CMD_MOVE,
            _pad: [0; 3],
            params: [12.0, 7.0, 0.0, 0.0, 0.0, 0.0],
        },
        optics_ui::sys::optics_ui_icon_cmd {
            kind: optics_ui::sys::optics_ui_icon_cmd_kind::OPTICS_UI_ICON_CMD_LINE,
            _pad: [0; 3],
            params: [12.0, 13.0, 0.0, 0.0, 0.0, 0.0],
        },
    ];
    let id = ui
        .register_icon(
            &cmds,
            &[],
            optics_ui::sys::optics_ui_icon_paint::OPTICS_UI_ICON_PAINT_STROKE,
        )
        .expect("valid vector icon registers");
    assert_ne!(id, optics_ui::sys::optics_ui_icon_id::OPTICS_UI_ICON_NONE);
    assert!(
        ui.register_icon(
            &[],
            &[],
            optics_ui::sys::optics_ui_icon_paint::OPTICS_UI_ICON_PAINT_STROKE,
        )
        .is_none()
    );
    ui.unregister_icon(id);
}

#[test]
fn version_links_and_runs() {
    let v = optics_ui::version();
    assert!(!v.is_empty(), "version string should be non-empty");
    assert!(v.starts_with("0."), "unexpected version: {v}");
    assert!(!optics_ui::gfx::version().is_empty());
}

#[test]
fn headless_frame_drives_widgets() {
    let mut ui = Ui::headless().expect("create headless ui");

    let mut wrap = true;
    let mut zoom = 1.5f32;
    let mut choice = 0i32;
    let mut name = TextBuf::new(64, "gfx");

    let input = Input::new((800.0, 600.0), 1.0 / 60.0);

    let clicked = ui.frame(&input, |f| {
        f.column().show_flat(|f| {
            f.label("Settings");
            f.label("A label");
            f.icon(optics_ui::Icon::ChevronRight, 16.0);
            f.separator();
            f.checkbox("Wrap", &mut wrap);
            f.switch("Compact mode", &mut wrap);
            f.row().items_center().show_flat(|f| {
                f.column().show_flat(|f| {
                    f.label("Tap to click");
                    f.label_with(
                        "Tap with one finger",
                        &optics_ui::LabelOpts {
                            size: 11.0,
                            ..Default::default()
                        },
                    );
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
    assert_eq!(name.as_str(), "gfx");

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
    let def = optics_ui::Theme::default();
    let dark = optics_ui::Theme::dark();
    let classic_light = optics_ui::Theme::classic(false);
    let classic_dark = optics_ui::Theme::classic(true);

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
        f.stack().pad(4.0).show(|f| {
            f.label("Base Background");
            f.button("Overlay Action");
        });
    });
}

#[test]
fn patterns_datagrid_tree_and_menu() {
    let mut ui = optics_ui::Ui::headless().unwrap();
    let input = optics_ui::Input::default();

    let mut cols = vec![
        optics_ui::patterns::DataGridCol::new("ID").width(60.0),
        optics_ui::patterns::DataGridCol::new("Name").width(120.0),
    ];
    let cells = ["1", "Item A", "2", "Item B"];
    let mut selected_row = 0;

    let mut tree_items = vec![
        optics_ui::patterns::TreeItem::new("Root").expanded(true),
        optics_ui::patterns::TreeItem::new("Child 1")
            .depth(1)
            .leaf(true),
        optics_ui::patterns::TreeItem::new("Child 2")
            .depth(1)
            .leaf(true),
    ];

    let menubar_items = vec![
        optics_ui::patterns::MenubarItem::new("File", "file_menu"),
        optics_ui::patterns::MenubarItem::new("Help", "help_menu"),
    ];

    ui.frame(&input, |f| {
        // DataGrid
        optics_ui::patterns::DataGrid::new("test_grid").show(
            f,
            &mut cols,
            &cells,
            2,
            &mut selected_row,
        );

        // TreeView
        optics_ui::patterns::TreeView::new("test_tree").show(f, &mut tree_items);

        // Menubar
        let (_open_menu, anchor) = optics_ui::patterns::menubar(f, "test_mb", &menubar_items);

        if let Some(anchor_rect) = anchor {
            let menu_items = vec![
                optics_ui::patterns::MenuItem::new("New"),
                optics_ui::patterns::MenuItem::separator(),
                optics_ui::patterns::MenuItem::new("Exit"),
            ];
            optics_ui::patterns::popup_menu(f, "file_menu", anchor_rect, &menu_items);
        }
    });
}

/// A failed ui call must carry gfx's own diagnostic, not just the result code.
///
/// `OPTICS_GFX_ERROR_INVALID_ARGUMENT` alone cannot distinguish a degenerate rect from
/// an out-of-bounds one from a double-begun pass, and each needs a different fix.
/// gfx records *which entry point* rejected the call and *why* in a
/// thread-local slot that the next failing call overwrites — so the only moment
/// the detail can be captured is at the failure site, inside the wrapper. This
/// test exists because losing it is otherwise a silent regression: the error
/// still reports a plausible code, and nothing complains.
#[test]
fn a_failed_call_carries_the_gfx_diagnostic() {
    // A null ui is rejected by `optics_ui_snapshot_activate`, and the rejection goes
    // through `Error::from_result`, which is the seam under test.
    let rc = unsafe {
        optics_ui::sys::optics_ui_snapshot_activate(std::ptr::null_mut(), std::ptr::null_mut())
    };
    assert_ne!(
        rc,
        optics_ui::sys::optics_gfx_result::OPTICS_GFX_OK,
        "null arguments must be rejected"
    );

    let diagnostic = optics_ui::GfxDiagnostic::for_test(rc);
    let rendered = diagnostic.to_string();
    assert!(
        rendered.contains("OPTICS_GFX_ERROR"),
        "the diagnostic must name the gfx code, not print a bare integer: {rendered}"
    );
    assert_ne!(diagnostic.code, 0, "the discriminant must survive");
    assert_eq!(
        diagnostic.code_name,
        format!("{rc:?}"),
        "code_name must be the C enum's own name so logs stay greppable"
    );

    /* The cause, not just the class. `OPTICS_GFX_ERROR_INVALID_ARGUMENT` alone cannot
     * distinguish a null handle from a degenerate rect from an out-of-bounds
     * scope; the message is what names which. Requiring it here is what stops a
     * future refactor from quietly reverting to a code-only error — the shape
     * would still compile and every "did it fail" test would still pass. */
    assert!(
        rendered.contains("optics_ui_snapshot_activate"),
        "the diagnostic must name the entry point that rejected the call: {rendered}"
    );
    assert!(
        diagnostic.message.is_some(),
        "the diagnostic must carry the human-readable cause: {rendered}"
    );
    assert!(
        diagnostic.line > 0,
        "the diagnostic must point at the rejecting site: {rendered}"
    );
}

/// The caret blink is a *phase*, and the host's two questions about it are not
/// interchangeable (ADR-0151).
///
/// Pinned here because the binding is where a host reads them, and the failure
/// they exist to prevent is a silent one: reporting "a caret exists" as damage
/// makes `optics_ui_frame_needs_repaint` permanently true, so a host with a focused
/// text field can never reach quiescence and composites the whole output at the
/// panel rate for a caret that changes twice a second.
#[test]
fn caret_blink_is_a_phase_with_a_separate_clock() {
    let mut ui = Ui::headless().expect("create headless ui");

    // No text widget has claimed the caret, so neither question has anything to
    // report — a focused-field contract must not fabricate a clock.
    let mut name = TextBuf::new(64, "hello");
    let input = Input::new((800.0, 600.0), 0.0);
    ui.frame(&input, |f| {
        f.textfield("caret-field", &mut name);
    });
    assert!(
        !ui.caret_clock_live(),
        "an unfocused field runs no blink clock"
    );
    assert!(!ui.caret_flip_pending(), "an unfocused field cannot flip");
    assert!(ui.caret_visible(), "with no blink the caret is solid");

    // Focus it through the safe surface, exactly as a host would.
    assert!(ui.focus("caret-field"), "the field must be focusable");
    ui.frame(&input, |f| {
        f.textfield("caret-field", &mut name);
    });
    assert!(
        ui.caret_clock_live(),
        "a focused text field must run a blink clock"
    );

    /* The clock is a *level* and the flip is an *edge*, so advancing within one
     * half-cycle must keep the clock live while owing no paint. Both facts are
     * asserted together on purpose: a test that only checked "no flip" would
     * pass for a caret that never blinks at all. */
    let step = Input::new((800.0, 600.0), 0.05);
    let mut flips = 0;
    for _ in 0..40 {
        ui.frame(&step, |f| {
            f.textfield("caret-field", &mut name);
        });
        assert!(ui.caret_clock_live(), "the clock must stay live");
        if ui.caret_flip_pending() {
            flips += 1;
            /* Presenting is what consumes a flip: the presented phase is
             * recorded on activation, never on build, so a skipped frame keeps
             * its flip pending instead of dropping it. */
            let snapshot = ui.snapshot().expect("snapshot");
            ui.activate(&snapshot).expect("activate");
            assert!(
                !ui.caret_flip_pending(),
                "activating must consume the flip it presented"
            );
        }
    }
    /* 40 frames at 50 ms is 2 s, i.e. under two 1.06 s periods, so a correct
     * contract owes roughly two to four paints — never 40 and never zero. */
    assert!(
        (1..=8).contains(&flips),
        "a 1.06 s blink owes a few paints over 2 s, got {flips}"
    );
}

/// Disabling the blink makes the caret solid *and* silent, and re-enabling names
/// a period rather than assuming the library default (ADR-0151).
#[test]
fn a_disabled_blink_is_solid_and_reports_nothing() {
    let mut ui = Ui::headless().expect("create headless ui");
    ui.set_caret_blink(0.0);

    let mut name = TextBuf::new(64, "hello");
    let input = Input::new((800.0, 600.0), 1.0 / 60.0);
    ui.frame(&input, |f| {
        f.textfield("solid-caret", &mut name);
    });
    assert!(ui.focus("solid-caret"), "the field must be focusable");
    ui.frame(&input, |f| {
        f.textfield("solid-caret", &mut name);
    });

    assert!(
        !ui.caret_clock_live(),
        "a disabled blink runs no clock, so nothing keeps a host awake"
    );
    assert!(!ui.caret_flip_pending(), "a solid caret never flips");
    assert!(
        ui.caret_visible(),
        "a disabled blink leaves the caret painted"
    );

    // Leaving reduced motion has to restore the blink. There is no "reset to
    // default" entry point — `period <= 0` *is* off — so the constant exists,
    // and this asserts it is a usable period rather than a placeholder.
    const { assert!(optics_ui::DEFAULT_CARET_BLINK_PERIOD_S > 0.0) };
    ui.set_caret_blink(optics_ui::DEFAULT_CARET_BLINK_PERIOD_S);
    ui.frame(&input, |f| {
        f.textfield("solid-caret", &mut name);
    });
    assert!(
        ui.caret_clock_live(),
        "restoring the default period must bring the clock back"
    );
}

#[test]
fn unified_corner_radii_across_gfx_and_ui_builders() {
    // optics_ui::CornerRadii is re-exported directly from optics_gfx (one type).
    let tab: optics_gfx::CornerRadii = optics_ui::CornerRadii::bottom(8.0);
    assert_eq!(tab, optics_ui::CornerRadii::new(0.0, 0.0, 8.0, 8.0));
    assert_eq!(tab.max_radius(), 8.0);

    let mut ui = Ui::headless().expect("create headless ui");
    let input = Input::new((320.0, 200.0), 1.0 / 60.0);
    let bg = optics_ui::Color::rgba(40, 80, 160, 255);
    ui.frame(&input, |f| {
        f.column()
            .bg(bg)
            .corner_radii(tab)
            .corner_curvature(0.6)
            .show_flat(|f| {
                f.row()
                    .bg(bg)
                    .corner_radii(12.0)
                    .corner_curvature(1.0)
                    .show_flat(|f| f.label("uniform via Into<CornerRadii>"));
                f.stack()
                    .bg(bg)
                    .corner_radii([0.0, 0.0, 16.0, 0.0])
                    .corner_curvature(0.5)
                    .show(|f| f.label("quarter-arc"));
                f.grid(2)
                    .bg(bg)
                    .corner_radii(optics_ui::CornerRadii::top(10.0))
                    .corner_curvature(0.7)
                    .show(|f| {
                        f.label("a");
                        f.label("b");
                    });
            });
    });
}

#[test]
fn motion_primitives_integrate_with_ui_reduced_motion() {
    use optics_ui::motion::{Hysteresis, Spring, SpringParams};

    let mut ui = Ui::headless().expect("create headless ui");
    assert!(!ui.reduced_motion());

    let mut spring = Spring::new(0.0);
    let mut badge = Hysteresis::new(false);
    let params = SpringParams::snappy();

    let mid = spring.advance(1.0, params, 1.0 / 60.0, ui.reduced_motion());
    assert!(mid > 0.0 && mid < 1.0);
    assert!(!badge.step(true, 0.0, 0.0, 0.0, 0.5, 1.0 / 60.0, ui.reduced_motion()));

    ui.set_reduced_motion(true);
    assert_eq!(
        spring.advance(1.0, params, 1.0 / 60.0, ui.reduced_motion()),
        1.0
    );
    assert!(badge.step(true, 0.0, 0.0, 0.0, 0.5, 1.0 / 60.0, ui.reduced_motion()));
}
