/*
 * widget_gallery/page_motion.c — Closed-form spring physics playground (<optics/ui/motion.h>).
 */

#include "gallery.h"

#include <math.h>
#include <stdio.h>

void wg_page_motion(optics_ui *ui, const ds_palette *p, wg_app *app) {
    ds_page_header(
        ui, p, "Motion",
        "Closed-form analytic damped harmonic springs from header-only <optics/ui/motion.h>.");

    /* Advance spring each frame */
    const optics_ui_spring_params params = {
        .stiffness = app->stiffness,
        .damping = app->damping,
    };
    optics_ui_spring_advance(&app->spring, app->target, params, optics_ui_dt(ui),
                             app->reduced_motion);
    const bool settled = optics_ui_spring_settled(&app->spring, app->target, 1e-3f, 1e-3f);
    if (!settled) {
        optics_app_request_frame();
    }

    ds_card_begin(ui, p, "Spring Playground",
                  "Toggle target between 0.0 and 1.0 or tune stiffness and damping live.");

    static const optics_ui_segmented_item presets[3] = {
        {.label = "Snappy"},
        {.label = "Gentle"},
        {.label = "Bouncy"},
    };
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    if (optics_ui_segmented_control(ui, &(optics_ui_segmented_opts){
                                            .id = "wg_spring_preset",
                                            .items = presets,
                                            .item_count = 3,
                                            .selected_index = &app->spring_preset,
                                        })) {
        optics_ui_spring_params pr = app->spring_preset == 0   ? optics_ui_spring_snappy()
                                     : app->spring_preset == 1 ? optics_ui_spring_gentle()
                                                               : optics_ui_spring_bouncy();
        app->stiffness = pr.stiffness;
        app->damping = pr.damping;
    }
    if (optics_ui_button(ui,
                         &(optics_ui_button_opts){
                             .label = app->target < 0.5f ? "Impulse -> 1.0" : "Impulse -> 0.0",
                             .variant = OPTICS_UI_BUTTON_PRIMARY,
                         })
            .clicked) {
        app->target = app->target < 0.5f ? 1.0f : 0.0f;
        optics_app_request_frame();
    }
    optics_ui_close(ui);

    /* Visual spring track */
    float clamped = fmaxf(0.0f, fminf(1.0f, app->spring.value));
    optics_ui_style track_st = OPTICS_UI_STYLE_INIT;
    track_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_CORNER_RADIUS;
    track_st.bg = p->subtle_bg;
    track_st.corner_radius = 12.0f;

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = 6,
                                .cross = OPTICS_UI_CENTER,
                                .box = {.height = 44.0f, .style = track_st},
                            });
    if (clamped > 0.005f) {
        optics_ui_spacer(ui, clamped * 440.0f);
    }
    optics_ui_style puck_st = OPTICS_UI_STYLE_INIT;
    puck_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_CORNER_RADIUS;
    puck_st.bg = p->accent;
    puck_st.corner_radius = 10.0f;
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .box = {.width = 32.0f, .height = 32.0f, .style = puck_st},
                            });
    optics_ui_close(ui);
    optics_ui_close(ui);

    char readout[128];
    snprintf(readout, sizeof readout, "value = %.3f   velocity = %.2f/s   target = %.1f   %s",
             app->spring.value, app->spring.velocity, app->target,
             settled ? "(settled)" : "(integrating)");
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = readout,
                        .size = 13.0f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });

    /* Stiffness & damping sliders */
    char stiff_str[32], damp_str[32];
    snprintf(stiff_str, sizeof stiff_str, "%.0f", app->stiffness);
    snprintf(damp_str, sizeof damp_str, "%.2f", app->damping);

    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Stiffness", .box = {.width = 96.0f}});
    optics_ui_flex(ui, 1.0f);
    optics_ui_slider(ui, &(optics_ui_slider_opts){
                             .box = {.id = "wg_stiff"},
                             .value = &app->stiffness,
                             .min = 80.0f,
                             .max = 900.0f,
                         });
    optics_ui_label(ui, &(optics_ui_label_opts){.text = stiff_str, .box = {.width = 56.0f}});
    optics_ui_close(ui);

    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Damping", .box = {.width = 96.0f}});
    optics_ui_flex(ui, 1.0f);
    optics_ui_slider(ui, &(optics_ui_slider_opts){
                             .box = {.id = "wg_damp"},
                             .value = &app->damping,
                             .min = 0.25f,
                             .max = 1.40f,
                         });
    optics_ui_label(ui, &(optics_ui_label_opts){.text = damp_str, .box = {.width = 56.0f}});
    optics_ui_close(ui);
    ds_card_end(ui);
}
