/*
 * ui.c — top floating control dock, material-specific view selectors,
 * boolean toggle buttons, and backdrop dropdown menu.
 */

#include "gallery.h"

void gallery_apply_theme(lens *ui, bool dark) {
    if (!ui)
        return;
    lens_theme t = dark ? lens_theme_dark() : lens_theme_default();
    if (dark) {
        /* Harmonious, refined dark mode:
         * Replaces harsh oversaturated electric blue (#3B82F6) with a balanced,
         * elegant sapphire tone (#3875D7), plus translucent glass surfaces. */
        t.color_accent = flux_color_rgba(56, 117, 215, 255); /* #3875D7 */
        t.color_bg = flux_color_rgba(18, 20, 28, 210);       /* dark glass capsule */
        t.color_hover = flux_color_rgba(255, 255, 255, 24);  /* luminous hover glow */
        t.color_active = flux_color_rgba(255, 255, 255, 42); /* soft press */
        t.color_border = flux_color_rgba(255, 255, 255, 28); /* hairline translucent border */
        t.color_fg = flux_color_rgba(242, 244, 250, 255);    /* soft off-white text */
        t.corner_radius = 7.0f;
    } else {
        /* Harmonious, luminous light mode */
        t.color_accent = flux_color_rgba(37, 99, 235, 255); /* royal blue #2563EB */
        t.color_bg = flux_color_rgba(255, 255, 255, 210);   /* pearl glass capsule */
        t.color_hover = flux_color_rgba(0, 0, 0, 18);       /* soft hover */
        t.color_active = flux_color_rgba(0, 0, 0, 32);
        t.color_border = flux_color_rgba(0, 0, 0, 25);
        t.color_fg = flux_color_rgba(15, 23, 42, 255);
        t.corner_radius = 7.0f;
    }
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
        } else if (key == '2') {
            app->view = VIEW_GLASS;
        } else if (key == '3') {
            app->view = VIEW_FROST;
        } else if (key == '4') {
            app->view = VIEW_ACRYLIC;
        } else if (key == '5') {
            app->view = VIEW_MICA;
        } else if (key == ' ' || key == 'p' || key == 'P') {
            app->animating = !app->animating;
        } else if (key == 'b' || key == 'B') {
            app->backdrop = (backdrop_kind)((app->backdrop + 1) % BACKDROP_COUNT);
        } else if (key == 'd' || key == 'D') {
            app->dark_mode = !app->dark_mode;
            gallery_apply_theme(ui, app->dark_mode);
        } else if (key == 'f' || key == 'F') {
            app->inactive_fallback = !app->inactive_fallback;
        }
    }

    if (app->animating)
        iris_request_animation_frame();

    /* Floating control bar at the top with native Lens widgets */
    lens_column_begin(ui, &(lens_layout_opts){.pad = 12, .gap = 6, .cross = LENS_STRETCH});
    lens_row_begin(ui, &(lens_layout_opts){.gap = 8, .cross = LENS_CENTER});

    /* 1. View selector segmented capsule with material-specific identities */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3,
                           .gap = 2,
                           .cross = LENS_CENTER,
                           .radius = 8.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(16, 18, 26, 210)
                                                : flux_color_rgba(255, 255, 255, 210),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                    : flux_color_rgba(0, 0, 0, 25),
                       });

    /* 4-Up Grid */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_grid",
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(71, 85, 105, 255)
                                                         : flux_color_rgba(100, 116, 139, 255),
                            },
                    },
                .label = "4-Up Grid",
                .icon = LENS_ICON_GRID,
                .variant = (app->view == VIEW_GRID) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_GRID;
    }

    /* Liquid Glass (Cyan/Sapphire Refractive Lens) */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_glass",
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(2, 132, 199, 255)
                                                         : flux_color_rgba(3, 105, 161, 255),
                            },
                    },
                .label = "Liquid Glass",
                .icon = LENS_ICON_DROPLET,
                .variant = (app->view == VIEW_GLASS) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_GLASS;
    }

    /* Frosted Glass (Vibrancy Lilac/Indigo Blur) */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_frost",
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(99, 102, 241, 255)
                                                         : flux_color_rgba(79, 70, 229, 255),
                            },
                    },
                .label = "Frosted Glass",
                .icon = LENS_ICON_WIND,
                .variant = (app->view == VIEW_FROST) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_FROST;
    }

    /* Acrylic (Fluent Teal/Cyan Material) */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_acrylic",
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(13, 148, 136, 255)
                                                         : flux_color_rgba(15, 118, 110, 255),
                            },
                    },
                .label = "Acrylic",
                .icon = LENS_ICON_LAYERS,
                .variant = (app->view == VIEW_ACRYLIC) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_ACRYLIC;
    }

    /* Mica (Wallpaper Mineral Slate-Blue) */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_mica",
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(59, 104, 152, 255)
                                                         : flux_color_rgba(37, 99, 235, 255),
                            },
                    },
                .label = "Mica & Mica Alt",
                .icon = LENS_ICON_MONITOR,
                .variant = (app->view == VIEW_MICA) ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->view = VIEW_MICA;
    }
    lens_close(ui); /* view selector capsule */

    /* 2. Controls icon segmented capsule (Boolean Toggles) */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3,
                           .gap = 2,
                           .cross = LENS_CENTER,
                           .radius = 8.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(16, 18, 26, 210)
                                                : flux_color_rgba(255, 255, 255, 210),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 28)
                                                    : flux_color_rgba(0, 0, 0, 25),
                       });

    /* Dark/Light boolean icon toggle (Moon / Sun) */
    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "toggle_theme",
                        .width = 34.0f,
                        .height = 30.0f,
                        .style =
                            {
                                .fields = LENS_STYLE_ACCENT,
                                .accent = app->dark_mode ? flux_color_rgba(245, 158, 11, 255)
                                                         : flux_color_rgba(99, 102, 241, 255),
                            },
                    },
                .icon = app->dark_mode ? LENS_ICON_MOON : LENS_ICON_SUN,
                .variant = LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->dark_mode = !app->dark_mode;
        gallery_apply_theme(ui, app->dark_mode);
    }

    /* Pause/Resume boolean animation icon toggle (Pause / Play) */
    if (lens_button(ui,
                    &(lens_button_opts){
                        .box =
                            {
                                .id = "toggle_anim",
                                .width = 34.0f,
                                .height = 30.0f,
                                .style =
                                    {
                                        .fields = LENS_STYLE_ACCENT,
                                        .accent = flux_color_rgba(234, 88, 12, 255),
                                    },
                            },
                        .icon = app->animating ? LENS_ICON_PAUSE : LENS_ICON_PLAY,
                        .variant = app->animating ? LENS_BUTTON_SUBTLE : LENS_BUTTON_PRIMARY,
                    })
            .clicked) {
        app->animating = !app->animating;
    }

    /* Mica Inactive Fallback boolean icon toggle (Eye / Eye-Off) */
    if (app->view == VIEW_MICA) {
        if (lens_button(
                ui,
                &(lens_button_opts){
                    .box =
                        {
                            .id = "toggle_fallback",
                            .width = 34.0f,
                            .height = 30.0f,
                            .style =
                                {
                                    .fields = LENS_STYLE_ACCENT,
                                    .accent = flux_color_rgba(59, 104, 152, 255),
                                },
                        },
                    .icon = app->inactive_fallback ? LENS_ICON_EYE_OFF : LENS_ICON_EYE,
                    .variant = app->inactive_fallback ? LENS_BUTTON_PRIMARY : LENS_BUTTON_SUBTLE,
                })
                .clicked) {
            app->inactive_fallback = !app->inactive_fallback;
        }
    }

    /* Backdrop dropdown trigger */
    lens_response bd_resp =
        lens_button(ui, &(lens_button_opts){
                            .box =
                                {
                                    .id = "btn_backdrop_dropdown",
                                },
                            .icon = g_backdrops[app->backdrop].icon,
                            .label = g_backdrops[app->backdrop].label,
                            .variant = lens_place_is_open(ui, "backdrop_menu") ? LENS_BUTTON_PRIMARY
                                                                               : LENS_BUTTON_SUBTLE,
                        });
    if (bd_resp.clicked) {
        lens_place_toggle(ui, "backdrop_menu");
    }
    app->backdrop_btn_rect = bd_resp.rect;

    lens_close(ui); /* controls capsule */

    lens_close(ui); /* row */

    /* 3. Backdrop Dropdown Popup Menu */
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

    /* Title / Material caption */
    const char *title_text = "";
    switch (app->view) {
    case VIEW_GRID:
        title_text = "Prism Materials: Liquid Glass (TL), Frosted (TR), Acrylic (BL), Mica (BR)";
        break;
    case VIEW_GLASS:
        title_text = "Liquid Glass Suite: [Hero] G2 Squircle & Focus Lens  |  [Top-R] Fluid "
                     "Metaball Fusion  |  [Bot-R] Amber Tint & Micro-Pill";
        break;
    case VIEW_FROST:
        title_text = "Frosted Glass: High-Purity Dual-Kawase Blur & Vibrancy Saturation Boost";
        break;
    case VIEW_ACRYLIC:
        title_text = "Acrylic: Dual-Kawase Blur, Luminance Plate Balancing & Procedural Grain";
        break;
    case VIEW_MICA:
        title_text = "Mica & Mica Alt: Wallpaper Sampling, Mineral Dither & Fallback State";
        break;
    }
    lens_label(ui, &(lens_label_opts){.text = title_text, .size = 13.0f});

    lens_close(ui); /* column */
}
