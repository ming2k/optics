/*
 * ui.c — top floating control dock, material-specific view selectors,
 * standalone circular dark/light mode toggle, bottom-right info badge,
 * and integrated property controls.
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
    case VIEW_RAW:
    default:
        t = app->dark_mode ? lens_theme_dark() : lens_theme_default();
        break;
    }
    t.corner_radius = 8.0f;
    lens_set_theme(ui, t);
}

static lens_button_opts gallery_tab_button_opts(const char *id, const char *label, bool selected,
                                                bool dark) {
    lens_style st = LENS_STYLE_INIT;
    st.fields = LENS_STYLE_CORNER_RADIUS | LENS_STYLE_FG | LENS_STYLE_BG | LENS_STYLE_BORDER |
                LENS_STYLE_BORDER_WIDTH;
    st.corner_radius = 14.0f;
    if (selected) {
        st.bg = dark ? flux_color_rgba(255, 255, 255, 36) : flux_color_rgba(15, 23, 42, 22);
        st.border = dark ? flux_color_rgba(255, 255, 255, 65) : flux_color_rgba(15, 23, 42, 45);
        st.border_width = 1.0f;
        st.fg = dark ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255);
    } else {
        st.bg = 0;
        st.border = 0;
        st.border_width = 0.0f;
        st.fg = dark ? flux_color_rgba(148, 163, 184, 210) : flux_color_rgba(100, 116, 139, 210);
    }
    return (lens_button_opts){
        .box =
            {
                .id = id,
                .height = 28.0f,
                .style = st,
            },
        .label = label,
        .variant = LENS_BUTTON_SUBTLE,
    };
}

static lens_button_opts gallery_ctrl_button_opts(const char *id, const char *label,
                                                 lens_icon_id icon, bool active, bool dark) {
    lens_style st = LENS_STYLE_INIT;
    st.fields = LENS_STYLE_CORNER_RADIUS | LENS_STYLE_FG | LENS_STYLE_BG | LENS_STYLE_BORDER |
                LENS_STYLE_BORDER_WIDTH;
    st.corner_radius = 14.0f;
    if (active) {
        st.bg = dark ? flux_color_rgba(255, 255, 255, 34) : flux_color_rgba(15, 23, 42, 20);
        st.border = dark ? flux_color_rgba(255, 255, 255, 60) : flux_color_rgba(15, 23, 42, 40);
        st.border_width = 1.0f;
        st.fg = dark ? flux_color_rgba(255, 255, 255, 255) : flux_color_rgba(15, 23, 42, 255);
    } else {
        st.bg = 0;
        st.border = 0;
        st.border_width = 0.0f;
        st.fg = dark ? flux_color_rgba(148, 163, 184, 210) : flux_color_rgba(100, 116, 139, 210);
    }
    return (lens_button_opts){
        .box =
            {
                .id = id,
                .height = 28.0f,
                .style = st,
            },
        .icon = icon,
        .label = label,
        .variant = LENS_BUTTON_SUBTLE,
    };
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
            app->view = VIEW_RAW;
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
    /*  Row 1: Top Floating Dock (Zen Minimalist Unified Island)          */
    /* ================================================================== */
    lens_row_begin(ui, &(lens_layout_opts){.gap = 8.0f, .cross = LENS_CENTER});

    /* 1. View selector segmented capsule */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3.0f,
                           .gap = 3.0f,
                           .cross = LENS_CENTER,
                           .radius = 16.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                : flux_color_rgba(255, 255, 255, 195),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                    : flux_color_rgba(0, 0, 0, 20),
                       });

    lens_button_opts b_raw =
        gallery_tab_button_opts("btn_raw", "Raw Backdrop", app->view == VIEW_RAW, app->dark_mode);
    if (lens_button(ui, &b_raw).clicked) {
        app->view = VIEW_RAW;
        gallery_apply_theme(app, ui);
    }

    lens_button_opts b_glass = gallery_tab_button_opts("btn_glass", "Liquid Glass",
                                                       app->view == VIEW_GLASS, app->dark_mode);
    if (lens_button(ui, &b_glass).clicked) {
        app->view = VIEW_GLASS;
        gallery_apply_theme(app, ui);
    }

    lens_button_opts b_frost =
        gallery_tab_button_opts("btn_frost", "Frosted", app->view == VIEW_FROST, app->dark_mode);
    if (lens_button(ui, &b_frost).clicked) {
        app->view = VIEW_FROST;
        gallery_apply_theme(app, ui);
    }

    lens_button_opts b_acrylic = gallery_tab_button_opts("btn_acrylic", "Acrylic",
                                                         app->view == VIEW_ACRYLIC, app->dark_mode);
    if (lens_button(ui, &b_acrylic).clicked) {
        app->view = VIEW_ACRYLIC;
        gallery_apply_theme(app, ui);
    }

    lens_button_opts b_mica =
        gallery_tab_button_opts("btn_mica", "Mica", app->view == VIEW_MICA, app->dark_mode);
    if (lens_button(ui, &b_mica).clicked) {
        app->view = VIEW_MICA;
        gallery_apply_theme(app, ui);
    }

    lens_close(ui); /* view selector capsule */

    /* 2. Backdrop Selector Capsule (Single click directly cycles to next backdrop) */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3.0f,
                           .gap = 2.0f,
                           .cross = LENS_CENTER,
                           .radius = 16.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                : flux_color_rgba(255, 255, 255, 195),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                    : flux_color_rgba(0, 0, 0, 20),
                       });

    lens_response bd_resp = lens_button(
        ui, &(lens_button_opts){
                .box =
                    {
                        .id = "btn_backdrop_dropdown",
                        .height = 28.0f,
                        .style =
                            {
                                .fields = LENS_STYLE_CORNER_RADIUS | LENS_STYLE_FG,
                                .corner_radius = 14.0f,
                                .fg = app->dark_mode ? flux_color_rgba(226, 232, 240, 240)
                                                     : flux_color_rgba(30, 41, 59, 240),
                            },
                    },
                .icon = g_backdrops[app->backdrop].icon,
                .label = g_backdrops[app->backdrop].label,
                .variant = LENS_BUTTON_SUBTLE,
            });
    if (bd_resp.clicked) {
        app->backdrop = (backdrop_kind)((app->backdrop + 1) % BACKDROP_COUNT);
    }
    app->backdrop_btn_rect = bd_resp.rect;

    lens_close(ui); /* backdrop capsule */

    /* 3. Animation Play / Pause Button */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 3.0f,
                           .gap = 2.0f,
                           .cross = LENS_CENTER,
                           .radius = 16.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                : flux_color_rgba(255, 255, 255, 195),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                    : flux_color_rgba(0, 0, 0, 20),
                       });

    if (lens_button(
            ui,
            &(lens_button_opts){
                .box =
                    {
                        .id = "btn_play_pause",
                        .height = 28.0f,
                        .style =
                            {
                                .fields = LENS_STYLE_CORNER_RADIUS | LENS_STYLE_FG,
                                .corner_radius = 14.0f,
                                .fg = app->animating
                                          ? (app->dark_mode ? flux_color_rgba(226, 232, 240, 240)
                                                            : flux_color_rgba(30, 41, 59, 240))
                                          : flux_color_rgba(251, 146, 60, 255),
                            },
                    },
                .icon = app->animating ? LENS_ICON_PAUSE : LENS_ICON_PLAY,
                .label = app->animating ? "Pause" : "Resume",
                .variant = LENS_BUTTON_SUBTLE,
            })
            .clicked) {
        app->animating = !app->animating;
    }
    lens_close(ui);

    /* 4. Integrated Material Property Controls (docked right in the top bar) */
    if (app->view != VIEW_RAW) {
        lens_row_begin(ui, &(lens_layout_opts){
                               .pad = 3.0f,
                               .gap = 4.0f,
                               .cross = LENS_CENTER,
                               .radius = 16.0f,
                               .border_width = 1.0f,
                               .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                    : flux_color_rgba(255, 255, 255, 195),
                               .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                        : flux_color_rgba(0, 0, 0, 20),
                           });

        switch (app->view) {
        case VIEW_GLASS: {
            lens_button_opts b = gallery_ctrl_button_opts(
                "ctrl_amber", app->glass_amber ? "Amber Tint" : "Clear Glass", LENS_ICON_SUN,
                app->glass_amber, app->dark_mode);
            if (lens_button(ui, &b).clicked)
                app->glass_amber = !app->glass_amber;
            break;
        }
        case VIEW_FROST: {
            lens_button_opts b = gallery_ctrl_button_opts(
                "ctrl_vibrancy", app->frost_vibrancy ? "High Vibrancy" : "Neutral Sat",
                LENS_ICON_SLIDERS, app->frost_vibrancy, app->dark_mode);
            if (lens_button(ui, &b).clicked)
                app->frost_vibrancy = !app->frost_vibrancy;
            break;
        }
        case VIEW_ACRYLIC: {
            lens_button_opts b = gallery_ctrl_button_opts(
                "ctrl_grain", app->acrylic_grain ? "Grain On" : "Grain Off", LENS_ICON_GRID,
                app->acrylic_grain, app->dark_mode);
            if (lens_button(ui, &b).clicked)
                app->acrylic_grain = !app->acrylic_grain;
            break;
        }
        case VIEW_MICA: {
            lens_button_opts b1 = gallery_ctrl_button_opts(
                "ctrl_fallback", app->inactive_fallback ? "Inactive" : "Active Win",
                app->inactive_fallback ? LENS_ICON_EYE_OFF : LENS_ICON_EYE, app->inactive_fallback,
                app->dark_mode);
            if (lens_button(ui, &b1).clicked)
                app->inactive_fallback = !app->inactive_fallback;

            lens_button_opts b2 =
                gallery_ctrl_button_opts("ctrl_alt", app->mica_alt ? "Mica Alt" : "Mica Base",
                                         LENS_ICON_LAYERS, app->mica_alt, app->dark_mode);
            if (lens_button(ui, &b2).clicked)
                app->mica_alt = !app->mica_alt;
            break;
        }
        default:
            break;
        }

        lens_close(ui);
    }

    /* 5. Spacer pushing the theme toggle to the far right */
    lens_row_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f}});
    lens_close(ui);

    /* 6. Standalone Circular Dark/Light Mode Button on the far right */
    lens_row_begin(ui, &(lens_layout_opts){
                           .pad = 0.0f,
                           .cross = LENS_CENTER,
                           .radius = 16.0f,
                           .border_width = 1.0f,
                           .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                : flux_color_rgba(255, 255, 255, 195),
                           .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                    : flux_color_rgba(0, 0, 0, 20),
                       });
    if (lens_button(ui,
                    &(lens_button_opts){
                        .box =
                            {
                                .id = "toggle_theme",
                                .width = 32.0f,
                                .height = 32.0f,
                                .style =
                                    {
                                        .fields = LENS_STYLE_CORNER_RADIUS | LENS_STYLE_FG,
                                        .corner_radius = 16.0f,
                                        .fg = app->dark_mode ? flux_color_rgba(251, 191, 36, 255)
                                                             : flux_color_rgba(79, 70, 229, 255),
                                    },
                            },
                        .icon = app->dark_mode ? LENS_ICON_SUN : LENS_ICON_MOON,
                        .variant = LENS_BUTTON_SUBTLE,
                    })
            .clicked) {
        app->dark_mode = !app->dark_mode;
        gallery_apply_theme(app, ui);
    }
    lens_close(ui);

    lens_close(ui); /* top row */

    /* ================================================================== */
    /*  Vertical Spacer pushing info card to the bottom                   */
    /* ================================================================== */
    lens_row_begin(ui, &(lens_layout_opts){.box = {.flex = 1.0f}});
    lens_close(ui);

    /* ================================================================== */
    /*  Bottom-Right Info & Optical Specification Badge                   */
    /* ================================================================== */
    const char *badge_title = "";
    const char *badge_desc = "";
    switch (app->view) {
    case VIEW_RAW:
        badge_title = "Raw Backdrop Baseline";
        badge_desc =
            "Zero material overlay. Unaltered optical control variable for baseline comparison.";
        break;
    case VIEW_GLASS:
        badge_title = "Liquid Glass Physical Optics";
        badge_desc = "Snell IOR 1.50 refraction · Cauchy chromatic dispersion · 360° dual rim · "
                     "1.5px Contact AO.";
        break;
    case VIEW_FROST:
        badge_title = "Frosted Glass (Non-distorting)";
        badge_desc = "Dual-Kawase pyramid blur · macOS vibrancy saturation boost · Procedural "
                     "blue-noise dithering.";
        break;
    case VIEW_ACRYLIC:
        badge_title = "Acrylic Translucent Surface";
        badge_desc = "Fluent dual-Kawase blur · Smoke/Pearl luminance plate · 1px highlight rim · "
                     "Procedural grain.";
        break;
    case VIEW_MICA:
        badge_title = "Mica & Mica Alt Foundation";
        badge_desc = "Screen-anchored wallpaper sampling · Soft blur & tinting · Mineral dither · "
                     "Inactive fallback.";
        break;
    }

    lens_row_begin(ui, &(lens_layout_opts){
                           .align = LENS_END,
                           .cross = LENS_END,
                       });
    lens_column_begin(ui, &(lens_layout_opts){
                              .pad = 8.0f,
                              .gap = 2.0f,
                              .radius = 12.0f,
                              .border_width = 1.0f,
                              .box = {.max_width = 400.0f},
                              .bg = app->dark_mode ? flux_color_rgba(15, 18, 26, 180)
                                                   : flux_color_rgba(255, 255, 255, 195),
                              .border = app->dark_mode ? flux_color_rgba(255, 255, 255, 24)
                                                       : flux_color_rgba(0, 0, 0, 20),
                          });
    lens_label(ui, &(lens_label_opts){
                       .box =
                           {
                               .style =
                                   {
                                       .fields = LENS_STYLE_FG,
                                       .fg = app->dark_mode ? flux_color_rgba(255, 255, 255, 240)
                                                            : flux_color_rgba(15, 23, 42, 240),
                                   },
                           },
                       .text = badge_title,
                       .size = 11.0f,
                   });
    lens_label(ui, &(lens_label_opts){
                       .box =
                           {
                               .style =
                                   {
                                       .fields = LENS_STYLE_FG,
                                       .fg = app->dark_mode ? flux_color_rgba(148, 163, 184, 210)
                                                            : flux_color_rgba(100, 116, 139, 210),
                                   },
                           },
                       .text = badge_desc,
                       .size = 10.0f,
                       .wrap = true,
                   });
    lens_close(ui); /* column */
    lens_close(ui); /* row */

    lens_close(ui); /* main container */
}
