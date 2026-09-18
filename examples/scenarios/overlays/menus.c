/*
 * menus.c — anchored dropdown popups for the Overlays scenario.
 */

#include "overlays.h"

void menu_separator(lens *ui, const shell_tones *tn) {
    lens_size(ui, 0, 1);
    lens_row_begin(ui, &(lens_layout_opts){.pad = 0, .bg = tn->divider});
    lens_spacer(ui, 0);
    lens_close(ui);
}

void draw_anchored_menus(lens *ui, overlay_app_state *state, const shell_tones *tones) {
    /* ── File dropdown popup ───────────────────────────────────── */
    if (lens_place_is_open(ui, "file_menu")) {
        if (lens_place_begin(
                ui,
                &(lens_place_opts){
                    .box =
                        {
                            .id = "file_menu",
                            .style =
                                {
                                    .fields = LENS_STYLE_MATERIAL,
                                    .material = LENS_MATERIAL_ACRYLIC,
                                },
                        },
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_ANCHORED,
                    .rect = state->file_btn_rect,
                    .transient = true,
                    .layout = {.box = {.min_width = 140}, .pad = 6, .bg = tones->card, .radius = 6},
                })) {
            if (lens_button(ui, &(lens_button_opts){.label = "New"}).clicked) {
                printf("  [FILE] New\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Open"}).clicked) {
                printf("  [FILE] Open\n");
                lens_place_close(ui, "file_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Save"}).clicked) {
                printf("  [FILE] Save\n");
                lens_place_close(ui, "file_menu");
            }
            menu_separator(ui, tones);
            if (lens_button(ui, &(lens_button_opts){.label = "Exit"}).clicked) {
                printf("  [FILE] Exit\n");
                lens_place_close(ui, "file_menu");
                iris_window_close();
            }
            lens_place_end(ui);
        }
    }

    /* ── Edit dropdown popup ───────────────────────────────────── */
    if (lens_place_is_open(ui, "edit_menu")) {
        if (lens_place_begin(
                ui,
                &(lens_place_opts){
                    .box =
                        {
                            .id = "edit_menu",
                            .style =
                                {
                                    .fields = LENS_STYLE_MATERIAL,
                                    .material = LENS_MATERIAL_ACRYLIC,
                                },
                        },
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_ANCHORED,
                    .rect = state->edit_btn_rect,
                    .transient = true,
                    .layout = {.box = {.min_width = 140}, .pad = 6, .bg = tones->card, .radius = 6},
                })) {
            if (lens_button(ui, &(lens_button_opts){.label = "Cut"}).clicked) {
                printf("  [EDIT] Cut\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Copy"}).clicked) {
                printf("  [EDIT] Copy\n");
                lens_place_close(ui, "edit_menu");
            }
            if (lens_button(ui, &(lens_button_opts){.label = "Paste"}).clicked) {
                printf("  [EDIT] Paste\n");
                lens_place_close(ui, "edit_menu");
            }
            lens_place_end(ui);
        }
    }
}
