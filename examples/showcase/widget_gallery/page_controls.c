/*
 * widget_gallery/page_controls.c — Core interactive controls & text editing.
 */

#include "gallery.h"

#include <stdio.h>
#include <string.h>

void wg_page_controls(optics_ui *ui, const ds_palette *p, wg_app *app) {
    ds_page_header(ui, p, "Controls",
                   "Buttons, toggles, sliders, segmented controls, and IME-aware text editing.");

    /* Buttons */
    ds_card_begin(ui, p, "Buttons", "Primary, default, subtle, and disabled action hierarchy.");
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    if (optics_ui_button(ui,
                         &(optics_ui_button_opts){
                             .label = "Primary Action",
                             .variant = OPTICS_UI_BUTTON_PRIMARY,
                         })
            .clicked) {
        app->primary_clicks++;
    }
    if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "Secondary"}).clicked) {
        app->primary_clicks = 0;
    }
    optics_ui_button(ui, &(optics_ui_button_opts){
                             .label = "Subtle",
                             .variant = OPTICS_UI_BUTTON_SUBTLE,
                         });
    optics_ui_button(ui, &(optics_ui_button_opts){
                             .label = "Disabled",
                             .box = {.disabled = true},
                         });
    optics_ui_close(ui);

    char click_msg[64];
    snprintf(click_msg, sizeof click_msg, "Primary action triggered %d time%s.",
             app->primary_clicks, app->primary_clicks == 1 ? "" : "s");
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = click_msg,
                        .size = 12.5f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    ds_card_end(ui);

    /* Toggles & Segmented Control */
    ds_card_begin(ui, p, "Selection & Toggles", "Checkboxes and exclusive segmented choices.");
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_XL, .cross = OPTICS_UI_CENTER});
    optics_ui_checkbox(
        ui, &(optics_ui_checkbox_opts){.label = "Auto-save drafts", .value = &app->opt_autosave});
    optics_ui_checkbox(
        ui, &(optics_ui_checkbox_opts){.label = "Display VSync pacing", .value = &app->opt_vsync});
    optics_ui_checkbox(
        ui, &(optics_ui_checkbox_opts){.label = "Diagnostics", .value = &app->opt_telemetry});
    optics_ui_close(ui);

    static const optics_ui_segmented_item density_items[3] = {
        {.label = "Compact"},
        {.label = "Comfortable"},
        {.label = "Spacious"},
    };
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = "Density",
                        .size = 13.5f,
                        .box = {.width = 96.0f,
                                .style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    optics_ui_segmented_control(ui, &(optics_ui_segmented_opts){
                                        .id = "wg_density_seg",
                                        .items = density_items,
                                        .item_count = 3,
                                        .selected_index = &app->density_idx,
                                    });
    optics_ui_close(ui);
    ds_card_end(ui);

    /* Sliders */
    ds_card_begin(ui, p, "Continuous Sliders", "Pointer drag and keyboard fine adjustment.");
    char exp_str[32], con_str[32];
    snprintf(exp_str, sizeof exp_str, "%.2f EV", app->exposure);
    snprintf(con_str, sizeof con_str, "%.2fx", app->contrast);

    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Exposure", .box = {.width = 96.0f}});
    optics_ui_flex(ui, 1.0f);
    optics_ui_slider(ui, &(optics_ui_slider_opts){
                             .box = {.id = "wg_exposure"},
                             .value = &app->exposure,
                             .min = -2.0f,
                             .max = 2.0f,
                         });
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = exp_str,
                        .box = {.width = 64.0f,
                                .style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    optics_ui_close(ui);

    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){.text = "Contrast", .box = {.width = 96.0f}});
    optics_ui_flex(ui, 1.0f);
    optics_ui_slider(ui, &(optics_ui_slider_opts){
                             .box = {.id = "wg_contrast"},
                             .value = &app->contrast,
                             .min = 0.5f,
                             .max = 2.0f,
                         });
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = con_str,
                        .box = {.width = 64.0f,
                                .style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    optics_ui_close(ui);
    ds_card_end(ui);

    /* Text Editing */
    ds_card_begin(ui, p, "Text Input",
                  "Native clipboard, caret selection, and platform IME composition.");
    optics_ui_textedit(ui, &(optics_ui_textedit_opts){
                               .box = {.id = "wg_note_edit", .height = 38.0f},
                               .buf = app->note_buf,
                               .cap = sizeof app->note_buf,
                               .placeholder = "Type text here...",
                           });
    char len_msg[48];
    snprintf(len_msg, sizeof len_msg, "%zu / %zu bytes", strlen(app->note_buf),
             sizeof app->note_buf - 1);
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = len_msg,
                            .size = 12.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_muted}},
                        });
    ds_card_end(ui);
}
