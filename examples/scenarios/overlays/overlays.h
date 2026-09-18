/*
 * overlays.h — data models, layout contracts, and component seams for the
 * Overlays scenario (popups, dropdown menus, and modal dialogs).
 */

#ifndef SCENARIOS_OVERLAYS_H
#define SCENARIOS_OVERLAYS_H

#include <iris/app.h>
#include <iris/window.h>
#include <lens/lens.h>
#include <lens/material.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Shell tone helpers for layered surfaces */
typedef struct shell_tones {
    flux_color toolbar;
    flux_color sidebar;
    flux_color card;
    flux_color status_bar;
    flux_color divider;
} shell_tones;

static inline flux_color shell_shift(flux_color c, int d) {
    uint8_t r, g, b, a;
    flux_color_unpack(c, &r, &g, &b, &a);
    int rr = (int)r + d;
    if (rr < 0)
        rr = 0;
    if (rr > 255)
        rr = 255;
    int gg = (int)g + d;
    if (gg < 0)
        gg = 0;
    if (gg > 255)
        gg = 255;
    int bb = (int)b + d;
    if (bb < 0)
        bb = 0;
    if (bb > 255)
        bb = 255;
    return flux_color_rgba((uint8_t)rr, (uint8_t)gg, (uint8_t)bb, a);
}

static inline shell_tones shell_tones_from(const lens_theme *th) {
    uint8_t r, g, b, a;
    flux_color_unpack(th->color_bg, &r, &g, &b, &a);
    int sum = (int)r + g + b;
    int up = sum < 384 ? +1 : -1;
    return (shell_tones){
        .toolbar = shell_shift(th->color_bg, up * -10),
        .sidebar = shell_shift(th->color_bg, up * 14),
        .card = shell_shift(th->color_bg, up * 28),
        .status_bar = shell_shift(th->color_bg, up * -18),
        .divider = shell_shift(th->color_bg, up * 18),
    };
}

/* State for the entire overlays scenario */
typedef struct overlay_app_state {
    bool dark_theme;
    char user_name[64];
    flux_rect file_btn_rect;
    flux_rect edit_btn_rect;
    bool smoke_mode;
    uint32_t smoke_frames;
} overlay_app_state;

/* Menu separator helper */
void menu_separator(lens *ui, const shell_tones *tn);

/* Sub-component declarations */
void draw_toolbar(lens *ui, overlay_app_state *state, const shell_tones *tones);
void draw_main_content(lens *ui, overlay_app_state *state, const lens_theme *theme);
void draw_anchored_menus(lens *ui, overlay_app_state *state, const shell_tones *tones);
void draw_modal_dialog(lens *ui, const lens_input *in, overlay_app_state *state,
                       const shell_tones *tones);

#endif /* SCENARIOS_OVERLAYS_H */
