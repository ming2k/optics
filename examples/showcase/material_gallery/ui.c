/*
 * ui.c — top floating control dock, material-specific view selectors,
 * standalone circular dark/light mode toggle, frosted subtitle pill,
 * and left-hand material interaction column.
 */

#include "gallery.h"

void gallery_apply_theme(gallery_app *app, lens *ui) {
    if (!ui || !app)
        return;
    lens_theme t;
    switch (app->view) {
    case VIEW_GLASS:
        t = lens_theme_for_material(LENS_MATERIAL_LIQUID_GLASS, app->dark_mode);
        break;
    case VIEW_FROST:
        t = lens_theme_for_material(LENS_MATERIAL_FROSTED, app->dark_mode);
        break;
    case VIEW_ACRYLIC:
        t = lens_theme_for_material(LENS_MATERIAL_ACRYLIC, app->dark_mode);
        break;
    case VIEW_MICA:
        t = lens_theme_for_material(LENS_MATERIAL_MICA, app->dark_mode);
        break;
    case VIEW_GRID:
    default:
        t = app->dark_mode ? lens_theme_dark() : lens_theme_default();
        break;
    }
    t.corner_radius = 8.0f;
    lens_set_theme(ui, t);
}

void gallery_ui_build(gallery_app *app, lens *ui, const lens_input *in) {
    /* Handle keyboard shortcuts */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == LENS_KEY_ESCAPE || key == 'q' || key == 'Q') {
            iris_window_close();
            return;
        } else if (key == '1') {
            app->view = VIEW_GRID;
            gallery_apply_theme(app, ui);
        } else if (key == '2') {
            app->view = VIEW_GLASS;
            gallery_apply_theme(app, ui);
        } else if (key == '3') {
            app->view = VIEW_FROST;
            gallery_apply_theme(app, ui);
        } else if (key == '4') {
            app->view = VIEW_ACRYLIC;
            gallery_apply_theme(app, ui);
        } else if (key == '5') {
            app->view = VIEW_MICA;
            gallery_apply_theme(app, ui);
        } else if (key == ' ' || key == 'p' || key == 'P') {
            app->animating = !app->animating;
        } else if (key == 'b' || key == 'B') {
            app->backdrop = (backdrop_kind)((app->backdrop + 1) % BACKDROP_COUNT);
        } else if (key == 'd' || key == 'D') {
            app->dark_mode = !app->dark_mode;
            gallery_apply_theme(app, ui);
        } else if (key == 'f' || key == 'F') {
            app->inactive_fallback = !app->inactive_fallback;
        }
    }

    if (app->animating)
        iris_request_animation_frame();

    /* Main UI container */
    lens_column_begin(ui, &(lens_layout_opts){.pad = 12.0f, .gap = 8.0f, .cross = LENS_STRETCH});

    /* ================================================================== */
    /*  Row 1: Top Navigation Bar                                         */
    /* ================================================================== */
    lens_row_begin(ui, &(lens_layout_opts){.gap = 8.0f, .cross = LENS_CENTER});

    /* 1. View selector segmented capsule */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3.0f,
                           .gap = 2.0f,
                           .cross = LENS_CENTER,
                           .radius = 8.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(16, 18, 26, 210)
                                                : flux_color_rgba(255, 255, 255, 210),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                    : flux_color_rgba(0, 0, 0, 25),
                       });

    /* 4-Up Grid */
    bool is_grid = (app->view == VIEW_GRID);
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_grid",
                        .style = is_grid ? (lens_style){
                            .fields = LENS_STYLE_BG | LENS_STYLE_BORDER | LENS_STYLE_BORDER_WIDTH | LENS_STYLE_FG,
                            .bg = app->dark_mode ? flux_color_rgba(50, 60, 80, 210) : flux_color_rgba(220, 225, 235, 240),
                            .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 40) : flux_color_rgba(0, 0, 0, 30),
                            .border_width = 1.0f,
                            .fg = app->dark_mode ? flux_color_rgba(250, 250, 252, 255) : flux_color_rgba(15, 23, 42, 255),
                        } : (lens_style)LENS_STYLE_INIT,
                    },
                .label = "4-Up Grid",
                .variant = is_grid ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_GRID;
        gallery_apply_theme(app, ui);
    }

    /* Liquid Glass */
    bool is_glass = (app->view == VIEW_GLASS);
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_glass",
                        .style = is_glass ? (lens_style){
                            .fields = LENS_STYLE_BG | LENS_STYLE_BORDER | LENS_STYLE_BORDER_WIDTH | LENS_STYLE_FG,
                            /* Liquid glass signature: translucent glossy core + bright specular rim */
                            .bg = app->dark_mode ? flux_color_rgba(255, 255, 255, 45) : flux_color_rgba(255, 255, 255, 185),
                            .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 160) : flux_color_rgba(255, 255, 255, 240),
                            .border_width = 1.5f,
                            .fg = app->dark_mode ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255),
                        } : (lens_style)LENS_STYLE_INIT,
                    },
                .label = "Liquid Glass",
                .variant = is_glass ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_GLASS;
        gallery_apply_theme(app, ui);
    }

    /* Frosted Glass */
    bool is_frost = (app->view == VIEW_FROST);
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_frost",
                        .style = is_frost ? (lens_style){
                            .fields = LENS_STYLE_BG | LENS_STYLE_BORDER | LENS_STYLE_BORDER_WIDTH | LENS_STYLE_FG,
                            .bg = app->dark_mode ? flux_color_rgba(14, 165, 233, 45) : flux_color_rgba(255, 255, 255, 200),
                            .border = flux_color_rgba(14, 165, 233, 140),
                            .border_width = 1.0f,
                            .fg = app->dark_mode ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255),
                        } : (lens_style)LENS_STYLE_INIT,
                    },
                .label = "Frosted",
                .variant = is_frost ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_FROST;
        gallery_apply_theme(app, ui);
    }

    /* Acrylic */
    bool is_acrylic = (app->view == VIEW_ACRYLIC);
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_acrylic",
                        .style = is_acrylic ? (lens_style){
                            .fields = LENS_STYLE_BG | LENS_STYLE_BORDER | LENS_STYLE_BORDER_WIDTH | LENS_STYLE_FG,
                            .bg = app->dark_mode ? flux_color_rgba(99, 102, 241, 45) : flux_color_rgba(240, 244, 248, 210),
                            .border = flux_color_rgba(99, 102, 241, 140),
                            .border_width = 1.0f,
                            .fg = app->dark_mode ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255),
                        } : (lens_style)LENS_STYLE_INIT,
                    },
                .label = "Acrylic",
                .variant = is_acrylic ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_ACRYLIC;
        gallery_apply_theme(app, ui);
    }

    /* Mica */
    bool is_mica = (app->view == VIEW_MICA);
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_mica",
                        .style = is_mica ? (lens_style){
                            .fields = LENS_STYLE_BG | LENS_STYLE_BORDER | LENS_STYLE_BORDER_WIDTH | LENS_STYLE_FG,
                            .bg = app->dark_mode ? flux_color_rgba(30, 43, 62, 190) : flux_color_rgba(239, 243, 248, 230),
                            .border = flux_color_rgba(59, 104, 152, 140),
                            .border_width = 1.0f,
                            .fg = app->dark_mode ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255),
                        } : (lens_style)LENS_STYLE_INIT,
                    },
                .label = "Mica",
                .variant = is_mica ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_MICA;
        gallery_apply_theme(app, ui);
    }

    lens_close(ui); /* view selector capsule */

    /* 2. Backdrop Dropdown capsule */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3.0f,
                           .gap = 2.0f,
                           .cross = LENS_CENTER,
                           .radius = 8.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(16, 18, 26, 210)
                                                : flux_color_rgba(255, 255, 255, 210),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                    : flux_color_rgba(0, 0, 0, 25),
                       });

    lens_response bd_resp =
        lens_button(ui, &(lens_button_opts){
                            .box = {.id = "btn_backdrop_dropdown"},
                            .icon = g_backdrops[app->backdrop].icon,
                            .label = g_backdrops[app->backdrop].label,
                            .variant = lens_place_is_open(ui, "backdrop_menu") ? LENS_BUTTON_PRIMARY
                                                                               : LENS_BUTTON_SUBTLE,
                        });
    if (bd_resp.clicked) {
        lens_place_toggle(ui, "backdrop_menu");
    }
    app->backdrop_btn_rect = bd_resp.rect;

    lens_close(ui); /* backdrop capsule */

    /* 3. Spacer pushing the theme toggle to the far right */
    lens_row_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f}});
    lens_close(ui);

    /* 4. Standalone Circular Dark/Light Mode Button on the far right (with visible decoration) */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 0.0f,
                           .cross = LENS_CENTER,
                           .radius = 18.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(20, 24, 38, 220)
                                                : flux_color_rgba(255, 255, 255, 220),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 35)
                                                    : flux_color_rgba(0, 0, 0, 30),
                       });
    if (lens_button(ui,
                    &(lens_button_opts){
                        .box =
                            {
                                .id = "toggle_theme",
                                .width = 36.0f,
                                .height = 36.0f,
                                .style =
                                    {
                                        .fields = LENS_STYLE_CORNER_RADIUS,
                                        .corner_radius = 18.0f,
                                    },
                            },
                        .icon = app->dark_mode ? LENS_ICON_MOON : LENS_ICON_SUN,
                        .variant = LENS_BUTTON_SUBTLE,
                    })
            .clicked) {
        app->dark_mode = !app->dark_mode;
        gallery_apply_theme(app, ui);
    }
    lens_close(ui);

    lens_close(ui); /* top row */

    /* Backdrop Dropdown Menu */
    if (lens_place_is_open(ui, "backdrop_menu")) {
        if (lens_place_begin(
                ui, &(lens_place_opts){
                        .box = {.id = "backdrop_menu"},
                        .band = LENS_BAND_POPUP,
                        .mode = LENS_PLACE_ANCHORED,
                        .rect = app->backdrop_btn_rect,
                        .transient = true,
                        .layout =
                            {
                                .box = {.min_width = 160.0f},
                                .pad = 5.0f,
                                .gap = 2.0f,
                                .cross = LENS_STRETCH,
                                .radius = 8.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? flux_color_rgba(20, 24, 34, 245)
                                                     : flux_color_rgba(255, 255, 255, 245),
                                .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 30)
                                                         : flux_color_rgba(0, 0, 0, 25),
                            },
                    })) {
            for (size_t i = 0; i < BACKDROP_COUNT; ++i) {
                char item_id[32];
                snprintf(item_id, sizeof(item_id), "bd_item_%zu", i);
                if (lens_selectable(ui,
                                    &(lens_selectable_opts){
                                        .box = {.id = item_id, .height = 28.0f},
                                        .label = g_backdrops[i].label,
                                        .icon = g_backdrops[i].icon,
                                        .selected = (app->backdrop == (backdrop_kind)i),
                                    })
                        .clicked) {
                    app->backdrop = (backdrop_kind)i;
                    lens_place_close(ui, "backdrop_menu");
                }
            }
            lens_place_end(ui);
        }
    }

    /* ================================================================== */
    /*  Row 2: Subtitle Caption enclosed in a frosted translucent pill   */
    /* ================================================================== */
    const char *title_text = "";
    switch (app->view) {
    case VIEW_GRID:
        title_text = "Prism Materials: Liquid Glass (TL), Frosted (TR), Acrylic (BL), Mica (BR)";
        break;
    case VIEW_GLASS:
        title_text = "Liquid Glass Suite: [Hero] G2 Squircle Physical Glass  |  [Top-R] Fluid "
                     "Metaball Fusion  |  [Bot-R] Amber Tint & Micro-Pill";
        break;
    case VIEW_FROST:
        title_text = "Frosted Glass: High-Purity Dual-Kawase Blur & Vibrancy Saturation Boost";
        break;
    case VIEW_ACRYLIC:
        title_text = "Acrylic: Dual-Kawase Blur, Luminance Plate Balancing & Procedural Grain";
        break;
    case VIEW_MICA:
        title_text = "Mica Foundation: Windows 11 Fluent Desktop Material (Base & Alt Surfaces)";
        break;
    }

    lens_row_begin(ui, &(lens_layout_opts){.cross = LENS_CENTER});
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 5.0f,
                           .radius = 14.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(16, 20, 30, 210)
                                                : flux_color_rgba(255, 255, 255, 210),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                    : flux_color_rgba(0, 0, 0, 25),
                       });
    lens_label(ui, &(lens_label_opts){
                       .box =
                           {
                               .style =
                                   {
                                       .fields = LENS_STYLE_FG,
                                       .fg = app->dark_mode ? flux_color_rgba(226, 232, 240, 255)
                                                            : flux_color_rgba(30, 41, 59, 255),
                                   },
                           },
                       .text = title_text,
                       .size = 12.0f,
                   });
    lens_close(ui); /* caption pill */
    lens_close(ui); /* caption row */

    /* ================================================================== */
    /*  Row 3: Left-hand Material-Specific Interactive Control Column    */
    /* ================================================================== */
    if (app->view != VIEW_GRID) {
        lens_row_begin(ui, &(lens_layout_opts){.cross = LENS_START});

        lens_column_begin(ui, &(lens_layout_opts){
                                  .pad = 6.0f,
                                  .gap = 6.0f,
                                  .radius = 10.0f,
                                  .border_width = 1.0f,
                                  .box = {.width = 124.0f},
                                  .bg = app->dark_mode ? flux_color_rgba(16, 20, 30, 210)
                                                       : flux_color_rgba(255, 255, 255, 210),
                                  .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                           : flux_color_rgba(0, 0, 0, 25),
                              });

        lens_label(ui,
                   &(lens_label_opts){
                       .box =
                           {
                               .style =
                                   {
                                       .fields = LENS_STYLE_FG,
                                       .fg = app->dark_mode ? flux_color_rgba(148, 163, 184, 255)
                                                            : flux_color_rgba(100, 116, 139, 255),
                                   },
                           },
                       .text = "PROPERTIES",
                       .size = 10.0f,
                   });

        switch (app->view) {
        case VIEW_GLASS:
            if (lens_button(
                    ui,
                    &(lens_button_opts){
                        .box = {.id = "ctrl_amber", .width = 112.0f, .height = 30.0f},
                        .icon = LENS_ICON_SUN,
                        .label = app->glass_amber ? "Amber Tint" : "Clear Glass",
                        .variant = app->glass_amber ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
                    })
                    .clicked) {
                app->glass_amber = !app->glass_amber;
            }
            break;

        case VIEW_FROST:
            if (lens_button(
                    ui,
                    &(lens_button_opts){
                        .box = {.id = "ctrl_vibrancy", .width = 112.0f, .height = 30.0f},
                        .icon = LENS_ICON_SLIDERS,
                        .label = app->frost_vibrancy ? "High Vibrancy" : "Neutral Sat",
                        .variant = app->frost_vibrancy ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
                    })
                    .clicked) {
                app->frost_vibrancy = !app->frost_vibrancy;
            }
            break;

        case VIEW_ACRYLIC:
            if (lens_button(
                    ui,
                    &(lens_button_opts){
                        .box = {.id = "ctrl_grain", .width = 112.0f, .height = 30.0f},
                        .icon = LENS_ICON_GRID,
                        .label = app->acrylic_grain ? "Grain On" : "Grain Off",
                        .variant = app->acrylic_grain ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
                    })
                    .clicked) {
                app->acrylic_grain = !app->acrylic_grain;
            }
            break;

        case VIEW_MICA:
            if (lens_button(ui,
                            &(lens_button_opts){
                                .box = {.id = "ctrl_fallback", .width = 112.0f, .height = 30.0f},
                                .icon = app->inactive_fallback ? LENS_ICON_EYE_OFF : LENS_ICON_EYE,
                                .label = app->inactive_fallback ? "Inactive" : "Active Win",
                                .variant = app->inactive_fallback ? LENS_BUTTON_PRIMARY
                                                                  : LENS_BUTTON_SUBTLE,
                            })
                    .clicked) {
                app->inactive_fallback = !app->inactive_fallback;
            }
            if (lens_button(ui,
                            &(lens_button_opts){
                                .box = {.id = "ctrl_alt", .width = 112.0f, .height = 30.0f},
                                .icon = LENS_ICON_LAYERS,
                                .label = app->mica_alt ? "Mica Alt" : "Mica Base",
                                .variant = app->mica_alt ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
                            })
                    .clicked) {
                app->mica_alt = !app->mica_alt;
            }
            break;

        default:
            break;
        }

        lens_close(ui); /* sidebar column */
        lens_close(ui); /* sidebar row */
    }

    lens_close(ui); /* main container */
}
