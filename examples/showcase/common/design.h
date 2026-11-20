/*
 * examples/showcase/common/design.h — shared visual design tokens and layout
 * helpers for Optics flagship showcase applications (ADR-0171).
 *
 * Strictly uses public <optics/ui.h> APIs. Enforces a consistent 8px spacing
 * scale, typographic hierarchy, card surfaces, and property rows across
 * widget_gallery, material_gallery, and scene_studio.
 */

#ifndef OPTICS_SHOWCASE_DESIGN_H
#define OPTICS_SHOWCASE_DESIGN_H

#include <optics/gfx/math.h>
#include <optics/ui.h>

#include <stdbool.h>
#include <stdint.h>

/* 8px spacing scale */
enum : int32_t {
    DS_SPACE_XS = 4,
    DS_SPACE_SM = 8,
    DS_SPACE_MD = 12,
    DS_SPACE_LG = 16,
    DS_SPACE_XL = 24,
    DS_SPACE_2XL = 32,
};

typedef struct ds_palette {
    optics_gfx_color canvas_bg;
    optics_gfx_color sidebar_bg;
    optics_gfx_color card_bg;
    optics_gfx_color card_border;
    optics_gfx_color subtle_bg;
    optics_gfx_color text_primary;
    optics_gfx_color text_secondary;
    optics_gfx_color text_muted;
    optics_gfx_color accent;
    optics_gfx_color accent_soft;
    optics_gfx_color accent_border;
} ds_palette;

static inline ds_palette ds_palette_for(bool dark) {
    if (dark) {
        return (ds_palette){
            .canvas_bg = optics_gfx_color_rgba(16, 18, 24, 255),
            .sidebar_bg = optics_gfx_color_rgba(21, 24, 32, 255),
            .card_bg = optics_gfx_color_rgba(26, 30, 40, 255),
            .card_border = optics_gfx_color_rgba(255, 255, 255, 22),
            .subtle_bg = optics_gfx_color_rgba(255, 255, 255, 14),
            .text_primary = optics_gfx_color_rgba(241, 245, 249, 255),
            .text_secondary = optics_gfx_color_rgba(148, 163, 184, 255),
            .text_muted = optics_gfx_color_rgba(100, 116, 139, 255),
            .accent = optics_gfx_color_rgba(96, 165, 250, 255),
            .accent_soft = optics_gfx_color_rgba(59, 130, 246, 38),
            .accent_border = optics_gfx_color_rgba(96, 165, 250, 96),
        };
    }
    return (ds_palette){
        .canvas_bg = optics_gfx_color_rgba(246, 248, 252, 255),
        .sidebar_bg = optics_gfx_color_rgba(239, 242, 248, 255),
        .card_bg = optics_gfx_color_rgba(255, 255, 255, 255),
        .card_border = optics_gfx_color_rgba(15, 23, 42, 24),
        .subtle_bg = optics_gfx_color_rgba(15, 23, 42, 12),
        .text_primary = optics_gfx_color_rgba(15, 23, 42, 255),
        .text_secondary = optics_gfx_color_rgba(71, 85, 105, 255),
        .text_muted = optics_gfx_color_rgba(100, 116, 139, 255),
        .accent = optics_gfx_color_rgba(37, 99, 235, 255),
        .accent_soft = optics_gfx_color_rgba(37, 99, 235, 24),
        .accent_border = optics_gfx_color_rgba(37, 99, 235, 72),
    };
}

static inline void ds_apply_showcase_theme(optics_ui *ui, bool dark) {
    optics_ui_theme th = dark ? optics_ui_theme_dark() : optics_ui_theme_default();
    const ds_palette p = ds_palette_for(dark);
    th.color_bg = p.canvas_bg;
    th.color_fg = p.text_primary;
    th.color_border = p.card_border;
    th.color_accent = p.accent;
    th.corner_radius = 8.0f;
    optics_ui_set_theme(ui, th);
}

static inline void ds_page_header(optics_ui *ui, const ds_palette *p, const char *title,
                                  const char *subtitle) {
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){.gap = DS_SPACE_XS});
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = title,
                            .size = 24.0f,
                            .weight = 650.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_primary}},
                        });
    if (subtitle && subtitle[0]) {
        optics_ui_label(
            ui, &(optics_ui_label_opts){
                    .text = subtitle,
                    .size = 13.5f,
                    .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                });
    }
    optics_ui_close(ui);
}

static inline void ds_card_begin(optics_ui *ui, const ds_palette *p, const char *title,
                                 const char *caption) {
    optics_ui_style st = OPTICS_UI_STYLE_INIT;
    st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH |
                OPTICS_UI_STYLE_CORNER_RADIUS;
    st.bg = p->card_bg;
    st.border = p->card_border;
    st.border_width = 1.0f;
    st.corner_radius = 12.0f;

    optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                   .pad = DS_SPACE_LG,
                                   .gap = DS_SPACE_MD,
                                   .box = {.style = st},
                               });
    if (title && title[0]) {
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){.gap = 2});
        optics_ui_label(
            ui, &(optics_ui_label_opts){
                    .text = title,
                    .size = 15.0f,
                    .weight = 600.0f,
                    .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_primary}},
                });
        if (caption && caption[0]) {
            optics_ui_label(
                ui, &(optics_ui_label_opts){
                        .text = caption,
                        .size = 12.5f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
        }
        optics_ui_close(ui);
    }
}

static inline void ds_card_end(optics_ui *ui) {
    optics_ui_close(ui);
}

static inline void ds_badge(optics_ui *ui, const ds_palette *p, const char *text) {
    optics_ui_style st = OPTICS_UI_STYLE_INIT;
    st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH |
                OPTICS_UI_STYLE_CORNER_RADIUS;
    st.bg = p->accent_soft;
    st.border = p->accent_border;
    st.border_width = 1.0f;
    st.corner_radius = 10.0f;

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 6,
                                .box = {.style = st},
                            });
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = text,
                            .size = 11.5f,
                            .weight = 600.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->accent}},
                        });
    optics_ui_close(ui);
}

#endif /* OPTICS_SHOWCASE_DESIGN_H */
