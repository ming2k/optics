/*
 * widget_gallery/page_overlays_dnd.c — Popup menus, modals, and drag-and-drop.
 */

#include "gallery.h"

#include <stdio.h>
#include <string.h>

void wg_page_overlays_dnd(optics_ui *ui, const ds_palette *p, wg_app *app) {
    ds_page_header(ui, p, "Overlays & Drag-and-Drop",
                   "Z-banded popups, centered modal dialogs, and native drag-and-drop targets.");

    /* Overlays */
    ds_card_begin(ui, p, "Popup Menu & Modal Dialog",
                  "Anchored popup menu in POPUP band and centered modal in MODAL band.");
    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_MD, .cross = OPTICS_UI_CENTER});
    optics_ui_response menu_btn =
        optics_ui_button(ui, &(optics_ui_button_opts){.label = "Open Context Menu",
                                                      .variant = OPTICS_UI_BUTTON_PRIMARY});
    if (menu_btn.clicked) {
        app->menu_anchor = menu_btn.rect;
        optics_ui_place_toggle(ui, "wg_popup_menu");
    }
    if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "Open Modal Dialog"}).clicked) {
        optics_ui_place_open(ui, "wg_modal_dialog");
    }
    optics_ui_close(ui);

    char action_msg[96];
    snprintf(action_msg, sizeof action_msg, "Last overlay action: %s", app->last_menu_action);
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = action_msg,
                        .size = 12.5f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    ds_card_end(ui);

    /* Popup menu overlay */
    static const optics_ui_menu_item MENU_ITEMS[4] = {
        {.label = "Duplicate Item", .shortcut = "Ctrl+D"},
        {.label = "Inspect Semantics", .shortcut = "Ctrl+I"},
        {.is_separator = true},
        {.label = "Reset Selection", .shortcut = "Ctrl+R"},
    };
    int32_t picked = optics_ui_popup_menu(ui, &(optics_ui_menu_opts){
                                                  .id = "wg_popup_menu",
                                                  .anchor = app->menu_anchor,
                                                  .items = MENU_ITEMS,
                                                  .item_count = 4,
                                                  .width = 220.0f,
                                              });
    if (picked >= 0 && picked < 4 && MENU_ITEMS[picked].label) {
        snprintf(app->last_menu_action, sizeof app->last_menu_action, "%s",
                 MENU_ITEMS[picked].label);
    }

    /* Centered modal overlay */
    optics_ui_style modal_st = OPTICS_UI_STYLE_INIT;
    modal_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH |
                      OPTICS_UI_STYLE_CORNER_RADIUS;
    modal_st.bg = p->card_bg;
    modal_st.border = p->accent_border;
    modal_st.border_width = 1.0f;
    modal_st.corner_radius = 12.0f;

    if (optics_ui_place_begin(
            ui, &(optics_ui_place_opts){
                    .box = {.id = "wg_modal_dialog", .width = 380.0f, .style = modal_st},
                    .band = OPTICS_UI_BAND_MODAL,
                    .mode = OPTICS_UI_PLACE_CENTERED,
                    .transient = true,
                    .interactive = true,
                    .layout = {.pad = DS_SPACE_XL, .gap = DS_SPACE_MD},
                })) {
        optics_ui_label(ui, &(optics_ui_label_opts){
                                .text = "Confirm Workspace Reset",
                                .size = 16.0f,
                                .weight = 650.0f,
                            });
        optics_ui_label(
            ui, &(optics_ui_label_opts){
                    .text = "Modals render in OPTICS_UI_BAND_MODAL and dismiss cleanly on Escape "
                            "or outside click.",
                    .size = 13.0f,
                    .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                });
        optics_ui_row_begin(ui, &(optics_ui_layout_opts){.gap = DS_SPACE_SM});
        optics_ui_flex(ui, 1.0f);
        if (optics_ui_button(
                ui, &(optics_ui_button_opts){.label = "Cancel", .variant = OPTICS_UI_BUTTON_SUBTLE})
                .clicked) {
            snprintf(app->last_menu_action, sizeof app->last_menu_action, "Modal cancelled");
            optics_ui_place_close(ui, "wg_modal_dialog");
        }
        if (optics_ui_button(ui, &(optics_ui_button_opts){.label = "Confirm",
                                                          .variant = OPTICS_UI_BUTTON_PRIMARY})
                .clicked) {
            snprintf(app->last_menu_action, sizeof app->last_menu_action, "Modal confirmed");
            optics_ui_place_close(ui, "wg_modal_dialog");
        }
        optics_ui_close(ui);
        optics_ui_place_end(ui);
    }

    /* Drag and drop */
    ds_card_begin(ui, p, "Drag & Drop",
                  "Drag the source pill into the drop zone below (or drop text from another app).");
    optics_ui_response src = optics_ui_button(ui, &(optics_ui_button_opts){
                                                      .box = {.id = "wg_dnd_src"},
                                                      .label = "Drag me: optics-ui-payload",
                                                  });
    const char *payload = "optics-ui-payload";
    optics_ui_dnd_source(ui, &(optics_ui_dnd_source_desc){
                                 .id = src.id,
                                 .text = payload,
                                 .text_len = strlen(payload),
                                 .actions = 1u | 2u,
                                 .preview_rect = src.rect,
                             });

    optics_ui_style drop_st = OPTICS_UI_STYLE_INIT;
    drop_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH |
                     OPTICS_UI_STYLE_CORNER_RADIUS;
    drop_st.bg = p->subtle_bg;
    drop_st.border = p->card_border;
    drop_st.border_width = 1.0f;
    drop_st.corner_radius = 10.0f;

    optics_ui_response drop_box = optics_ui_pressable_begin(
        ui, &(optics_ui_pressable_opts){
                .box = {.id = "wg_dnd_target", .height = 68.0f, .style = drop_st},
                .layout = {.pad = DS_SPACE_LG},
            });
    char drop_msg[160];
    snprintf(drop_msg, sizeof drop_msg, "Drop target (%d received): %s", app->drop_count,
             app->last_drop_payload);
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = drop_msg,
                        .size = 13.0f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    optics_ui_pressable_end(ui);

    optics_ui_dnd_drop_info info = {};
    if (optics_ui_dnd_drop_target(ui,
                                  &(optics_ui_dnd_drop_target_opts){
                                      .id = drop_box.id,
                                      .accepted_actions = 1u | 2u,
                                      .out_info = &info,
                                  }) &&
        info.is_dropped) {
        char buf[128] = {};
        uint32_t n = optics_ui_take_drop(
            ui, &(optics_ui_take_drop_opts){.dst = buf, .cap = (uint32_t)sizeof(buf) - 1u});
        if (n > 0) {
            buf[n] = '\0';
            snprintf(app->last_drop_payload, sizeof app->last_drop_payload, "%s", buf);
        }
        app->drop_count++;
    }
    ds_card_end(ui);
}
