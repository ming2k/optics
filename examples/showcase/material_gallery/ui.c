/*
 * ui.c — top floating control dock, material-specific view selectors,
 * standalone circular dark/light mode toggle, bottom-right info badge,
 * and integrated property controls.
 */

#include "gallery.h"

void gallery_apply_theme(gallery_app *app, optics_ui *ui) {
    if (!ui || !app)
        return;
    optics_ui_theme t;
    switch (app->view) {
    case VIEW_GLASS:
        t = optics_ui_theme_for_material(OPTICS_UI_MATERIAL_LIQUID_GLASS, app->dark_mode);
        break;
    case VIEW_FROST:
        t = optics_ui_theme_for_material(OPTICS_UI_MATERIAL_FROSTED, app->dark_mode);
        break;
    case VIEW_ACRYLIC:
        t = optics_ui_theme_for_material(OPTICS_UI_MATERIAL_ACRYLIC, app->dark_mode);
        break;
    case VIEW_MICA:
        t = optics_ui_theme_for_material(OPTICS_UI_MATERIAL_MICA, app->dark_mode);
        break;
    case VIEW_RAW:
    default:
        t = app->dark_mode ? optics_ui_theme_dark() : optics_ui_theme_default();
        break;
    }
    t.corner_radius = 8.0f;
    optics_ui_set_theme(ui, t);
}

static optics_ui_button_opts gallery_tab_button_opts(const char *id, const char *label,
                                                     bool selected, bool dark) {
    optics_ui_style st = OPTICS_UI_STYLE_INIT;
    st.fields = OPTICS_UI_STYLE_CORNER_RADIUS | OPTICS_UI_STYLE_FG | OPTICS_UI_STYLE_BG |
                OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH;
    st.corner_radius = 14.0f;
    if (selected) {
        st.bg =
            dark ? optics_gfx_color_rgba(255, 255, 255, 36) : optics_gfx_color_rgba(15, 23, 42, 22);
        st.border =
            dark ? optics_gfx_color_rgba(255, 255, 255, 65) : optics_gfx_color_rgba(15, 23, 42, 45);
        st.border_width = 1.0f;
        st.fg = dark ? optics_gfx_color_rgba(255, 255, 255, 255)
                     : optics_gfx_color_rgba(15, 23, 42, 255);
    } else {
        st.bg = 0;
        st.border = 0;
        st.border_width = 0.0f;
        st.fg = dark ? optics_gfx_color_rgba(148, 163, 184, 210)
                     : optics_gfx_color_rgba(100, 116, 139, 210);
    }
    return (optics_ui_button_opts){
        .box =
            {
                .id = id,
                .height = 28.0f,
                .style = st,
            },
        .label = label,
        .variant = OPTICS_UI_BUTTON_SUBTLE,
    };
}

static optics_ui_button_opts gallery_ctrl_button_opts(const char *id, const char *label,
                                                      optics_ui_icon_id icon, bool active,
                                                      bool dark) {
    optics_ui_style st = OPTICS_UI_STYLE_INIT;
    st.fields = OPTICS_UI_STYLE_CORNER_RADIUS | OPTICS_UI_STYLE_FG | OPTICS_UI_STYLE_BG |
                OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH;
    st.corner_radius = 14.0f;
    if (active) {
        st.bg =
            dark ? optics_gfx_color_rgba(255, 255, 255, 34) : optics_gfx_color_rgba(15, 23, 42, 20);
        st.border =
            dark ? optics_gfx_color_rgba(255, 255, 255, 60) : optics_gfx_color_rgba(15, 23, 42, 40);
        st.border_width = 1.0f;
        st.fg = dark ? optics_gfx_color_rgba(255, 255, 255, 255)
                     : optics_gfx_color_rgba(15, 23, 42, 255);
    } else {
        st.bg = 0;
        st.border = 0;
        st.border_width = 0.0f;
        st.fg = dark ? optics_gfx_color_rgba(148, 163, 184, 210)
                     : optics_gfx_color_rgba(100, 116, 139, 210);
    }
    return (optics_ui_button_opts){
        .box =
            {
                .id = id,
                .height = 28.0f,
                .style = st,
            },
        .icon = icon,
        .label = label,
        .variant = OPTICS_UI_BUTTON_SUBTLE,
    };
}

void gallery_ui_build(gallery_app *app, optics_ui *ui, const optics_ui_input *in) {
    /* Handle keyboard shortcuts */
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == OPTICS_UI_KEY_ESCAPE || key == 'q' || key == 'Q') {
            optics_window_close();
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
        optics_window_request_animation_frame();

    /* Main UI container */
    optics_ui_column_begin(
        ui, &(optics_ui_layout_opts){.pad = 12.0f, .gap = 8.0f, .cross = OPTICS_UI_STRETCH});

    /* ================================================================== */
    /*  Row 1: Top Floating Dock (Zen Minimalist Unified Island)          */
    /* ================================================================== */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = 8.0f, .cross = OPTICS_UI_CENTER});

    /* 1. View selector segmented capsule */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 3.0f,
                                .gap = 3.0f,
                                .cross = OPTICS_UI_CENTER,
                                .radius = 16.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                     : optics_gfx_color_rgba(255, 255, 255, 195),
                                .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                         : optics_gfx_color_rgba(0, 0, 0, 20),
                            });

    optics_ui_button_opts b_raw =
        gallery_tab_button_opts("btn_raw", "Raw Backdrop", app->view == VIEW_RAW, app->dark_mode);
    if (optics_ui_button(ui, &b_raw).clicked) {
        app->view = VIEW_RAW;
        gallery_apply_theme(app, ui);
    }

    optics_ui_button_opts b_glass = gallery_tab_button_opts(
        "btn_glass", "Liquid Glass", app->view == VIEW_GLASS, app->dark_mode);
    if (optics_ui_button(ui, &b_glass).clicked) {
        app->view = VIEW_GLASS;
        gallery_apply_theme(app, ui);
    }

    optics_ui_button_opts b_frost =
        gallery_tab_button_opts("btn_frost", "Frosted", app->view == VIEW_FROST, app->dark_mode);
    if (optics_ui_button(ui, &b_frost).clicked) {
        app->view = VIEW_FROST;
        gallery_apply_theme(app, ui);
    }

    optics_ui_button_opts b_acrylic = gallery_tab_button_opts(
        "btn_acrylic", "Acrylic", app->view == VIEW_ACRYLIC, app->dark_mode);
    if (optics_ui_button(ui, &b_acrylic).clicked) {
        app->view = VIEW_ACRYLIC;
        gallery_apply_theme(app, ui);
    }

    optics_ui_button_opts b_mica =
        gallery_tab_button_opts("btn_mica", "Mica", app->view == VIEW_MICA, app->dark_mode);
    if (optics_ui_button(ui, &b_mica).clicked) {
        app->view = VIEW_MICA;
        gallery_apply_theme(app, ui);
    }

    optics_ui_close(ui); /* view selector capsule */

    /* 2. Backdrop Selector Capsule (Single click directly cycles to next backdrop) */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 3.0f,
                                .gap = 2.0f,
                                .cross = OPTICS_UI_CENTER,
                                .radius = 16.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                     : optics_gfx_color_rgba(255, 255, 255, 195),
                                .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                         : optics_gfx_color_rgba(0, 0, 0, 20),
                            });

    optics_ui_response bd_resp = optics_ui_button(
        ui, &(optics_ui_button_opts){
                .box =
                    {
                        .id = "btn_backdrop_dropdown",
                        .height = 28.0f,
                        .style =
                            {
                                .fields = OPTICS_UI_STYLE_CORNER_RADIUS | OPTICS_UI_STYLE_FG,
                                .corner_radius = 14.0f,
                                .fg = app->dark_mode ? optics_gfx_color_rgba(226, 232, 240, 240)
                                                     : optics_gfx_color_rgba(30, 41, 59, 240),
                            },
                    },
                .icon = g_backdrops[app->backdrop].icon,
                .label = g_backdrops[app->backdrop].label,
                .variant = OPTICS_UI_BUTTON_SUBTLE,
            });
    if (bd_resp.clicked) {
        app->backdrop = (backdrop_kind)((app->backdrop + 1) % BACKDROP_COUNT);
    }
    app->backdrop_btn_rect = bd_resp.rect;

    optics_ui_close(ui); /* backdrop capsule */

    /* 3. Animation Play / Pause Button */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 3.0f,
                                .gap = 2.0f,
                                .cross = OPTICS_UI_CENTER,
                                .radius = 16.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                     : optics_gfx_color_rgba(255, 255, 255, 195),
                                .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                         : optics_gfx_color_rgba(0, 0, 0, 20),
                            });

    if (optics_ui_button(
            ui,
            &(optics_ui_button_opts){
                .box =
                    {
                        .id = "btn_play_pause",
                        .height = 28.0f,
                        .style =
                            {
                                .fields = OPTICS_UI_STYLE_CORNER_RADIUS | OPTICS_UI_STYLE_FG,
                                .corner_radius = 14.0f,
                                .fg = app->animating
                                          ? (app->dark_mode
                                                 ? optics_gfx_color_rgba(226, 232, 240, 240)
                                                 : optics_gfx_color_rgba(30, 41, 59, 240))
                                          : optics_gfx_color_rgba(251, 146, 60, 255),
                            },
                    },
                .icon = app->animating ? OPTICS_UI_ICON_PAUSE : OPTICS_UI_ICON_PLAY,
                .label = app->animating ? "Pause" : "Resume",
                .variant = OPTICS_UI_BUTTON_SUBTLE,
            })
            .clicked) {
        app->animating = !app->animating;
    }
    optics_ui_close(ui);

    /* 4. Integrated Material Property Controls (docked right in the top bar) */
    if (app->view != VIEW_RAW) {
        optics_ui_row_begin(ui,
                            &(optics_ui_layout_opts){
                                .pad = 3.0f,
                                .gap = 4.0f,
                                .cross = OPTICS_UI_CENTER,
                                .radius = 16.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                     : optics_gfx_color_rgba(255, 255, 255, 195),
                                .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                         : optics_gfx_color_rgba(0, 0, 0, 20),
                            });

        switch (app->view) {
        case VIEW_GLASS: {
            optics_ui_button_opts b = gallery_ctrl_button_opts(
                "ctrl_amber", app->glass_amber ? "Amber Tint" : "Clear Glass", OPTICS_UI_ICON_SUN,
                app->glass_amber, app->dark_mode);
            if (optics_ui_button(ui, &b).clicked)
                app->glass_amber = !app->glass_amber;
            break;
        }
        case VIEW_FROST: {
            optics_ui_button_opts b = gallery_ctrl_button_opts(
                "ctrl_vibrancy", app->frost_vibrancy ? "High Vibrancy" : "Neutral Sat",
                OPTICS_UI_ICON_SLIDERS, app->frost_vibrancy, app->dark_mode);
            if (optics_ui_button(ui, &b).clicked)
                app->frost_vibrancy = !app->frost_vibrancy;
            break;
        }
        case VIEW_ACRYLIC: {
            optics_ui_button_opts b = gallery_ctrl_button_opts(
                "ctrl_grain", app->acrylic_grain ? "Grain On" : "Grain Off", OPTICS_UI_ICON_GRID,
                app->acrylic_grain, app->dark_mode);
            if (optics_ui_button(ui, &b).clicked)
                app->acrylic_grain = !app->acrylic_grain;
            break;
        }
        case VIEW_MICA: {
            optics_ui_button_opts b1 = gallery_ctrl_button_opts(
                "ctrl_fallback", app->inactive_fallback ? "Inactive" : "Active Win",
                app->inactive_fallback ? OPTICS_UI_ICON_EYE_OFF : OPTICS_UI_ICON_EYE,
                app->inactive_fallback, app->dark_mode);
            if (optics_ui_button(ui, &b1).clicked)
                app->inactive_fallback = !app->inactive_fallback;

            optics_ui_button_opts b2 =
                gallery_ctrl_button_opts("ctrl_alt", app->mica_alt ? "Mica Alt" : "Mica Base",
                                         OPTICS_UI_ICON_LAYERS, app->mica_alt, app->dark_mode);
            if (optics_ui_button(ui, &b2).clicked)
                app->mica_alt = !app->mica_alt;
            break;
        }
        default:
            break;
        }

        optics_ui_close(ui);
    }

    /* 5. Spacer pushing the theme toggle to the far right */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box = {.flex = 1.0f}});
    optics_ui_close(ui);

    /* 6. Standalone Circular Dark/Light Mode Button on the far right */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 0.0f,
                                .cross = OPTICS_UI_CENTER,
                                .radius = 16.0f,
                                .border_width = 1.0f,
                                .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                     : optics_gfx_color_rgba(255, 255, 255, 195),
                                .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                         : optics_gfx_color_rgba(0, 0, 0, 20),
                            });
    if (optics_ui_button(
            ui,
            &(optics_ui_button_opts){
                .box =
                    {
                        .id = "toggle_theme",
                        .width = 32.0f,
                        .height = 32.0f,
                        .style =
                            {
                                .fields = OPTICS_UI_STYLE_CORNER_RADIUS | OPTICS_UI_STYLE_FG,
                                .corner_radius = 16.0f,
                                .fg = app->dark_mode ? optics_gfx_color_rgba(251, 191, 36, 255)
                                                     : optics_gfx_color_rgba(79, 70, 229, 255),
                            },
                    },
                .icon = app->dark_mode ? OPTICS_UI_ICON_SUN : OPTICS_UI_ICON_MOON,
                .variant = OPTICS_UI_BUTTON_SUBTLE,
            })
            .clicked) {
        app->dark_mode = !app->dark_mode;
        gallery_apply_theme(app, ui);
    }
    optics_ui_close(ui);

    optics_ui_close(ui); /* top row */

    /* ================================================================== */
    /*  Vertical Spacer pushing info card to the bottom                   */
    /* ================================================================== */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.box = {.flex = 1.0f}});
    optics_ui_close(ui);

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

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .align = OPTICS_UI_END,
                                .cross = OPTICS_UI_END,
                            });
    optics_ui_column_begin(ui,
                           &(optics_ui_layout_opts){
                               .pad = 8.0f,
                               .gap = 2.0f,
                               .radius = 12.0f,
                               .border_width = 1.0f,
                               .box = {.max_width = 400.0f},
                               .bg = app->dark_mode ? optics_gfx_color_rgba(15, 18, 26, 180)
                                                    : optics_gfx_color_rgba(255, 255, 255, 195),
                               .border = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 24)
                                                        : optics_gfx_color_rgba(0, 0, 0, 20),
                           });
    optics_ui_label(
        ui, &(optics_ui_label_opts){
                .box =
                    {
                        .style =
                            {
                                .fields = OPTICS_UI_STYLE_FG,
                                .fg = app->dark_mode ? optics_gfx_color_rgba(255, 255, 255, 240)
                                                     : optics_gfx_color_rgba(15, 23, 42, 240),
                            },
                    },
                .text = badge_title,
                .size = 11.0f,
            });
    optics_ui_label(
        ui, &(optics_ui_label_opts){
                .box =
                    {
                        .style =
                            {
                                .fields = OPTICS_UI_STYLE_FG,
                                .fg = app->dark_mode ? optics_gfx_color_rgba(148, 163, 184, 210)
                                                     : optics_gfx_color_rgba(100, 116, 139, 210),
                            },
                    },
                .text = badge_desc,
                .size = 10.0f,
                .wrap = true,
            });
    optics_ui_close(ui); /* column */
    optics_ui_close(ui); /* row */

    optics_ui_close(ui); /* main container */
}
