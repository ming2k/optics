/*
 * modal.c — modal dialog with backdrop scrim and focus trapping.
 */

#include "overlays.h"

void draw_modal_dialog(lens *ui, const lens_input *in, overlay_app_state *state,
                       const shell_tones *tones) {
    if (!lens_place_is_open(ui, "modal"))
        return;

    /* 1. Backdrop scrim (LENS_PLACE_EXACT spanning full window) */
    flux_color scrim =
        state->dark_theme ? flux_color_rgba(0, 0, 0, 0xB0) : flux_color_rgba(255, 255, 255, 0x90);

    if (lens_place_begin(
            ui, &(lens_place_opts){
                    .box = {.id = "modal_scrim"},
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_EXACT,
                    .rect = {0, 0, in ? in->display_size.x : 0, in ? in->display_size.y : 0},
                    .layout = {.bg = scrim},
                })) {
        lens_place_end(ui);
    }

    /* 2. Centered dialog surface with Acrylic / Frosted material styling */
    if (lens_place_begin(
            ui, &(lens_place_opts){
                    .box =
                        {
                            .id = "modal_dialog",
                            .style =
                                {
                                    .fields = LENS_STYLE_MATERIAL | LENS_STYLE_CORNER_RADIUS,
                                    .material = LENS_MATERIAL_ACRYLIC,
                                    .corner_radius = 12.0f,
                                },
                        },
                    .band = LENS_BAND_POPUP,
                    .mode = LENS_PLACE_CENTERED,
                    .transient = true,
                    .layout =
                        {
                            .box = {.min_width = 340},
                            .pad = 24,
                            .gap = 14,
                            .bg = tones->card,
                            .radius = 12.0f,
                        },
                })) {

        lens_label(ui, &(lens_label_opts){.text = "Modal Confirmation", .size = 18.0f});
        lens_label(ui, &(lens_label_opts){
                           .text = "This modal traps interaction and provides smooth elevation.\n"
                                   "Press Escape or click outside to dismiss."});

        /* Action buttons row */
        lens_row_begin(ui, &(lens_layout_opts){.gap = 10, .cross = LENS_CENTER});
        lens_flex(ui, 1.0f);
        lens_spacer(ui, 0);

        if (lens_button(ui, &(lens_button_opts){.label = "Cancel"}).clicked) {
            printf("  [MODAL] Cancel clicked\n");
            lens_place_close(ui, "modal");
        }

        if (lens_button(ui,
                        &(lens_button_opts){
                            .label = "Confirm",
                            .variant = LENS_BUTTON_PRIMARY,
                        })
                .clicked) {
            printf("  [MODAL] Confirm clicked\n");
            lens_place_close(ui, "modal");
        }
        lens_close(ui);

        lens_place_end(ui);
    }
}
