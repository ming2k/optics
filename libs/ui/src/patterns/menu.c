/* menu.c — cascading popup menu and menubar pattern (ADR-Pattern-007). */

#include "../internal.h"
#include <optics/ui/patterns.h>

#include <stdio.h>

typedef struct popup_menu_state {
    int32_t focused_index;
} popup_menu_state;

int32_t optics_ui_popup_menu(optics_ui *ui, const optics_ui_menu_opts *opts) {
    if (!opts)
        return -1;
    const char *menu_id = opts->id;
    const optics_gfx_rect anchor_rect = opts->anchor;
    const optics_ui_menu_item *items = opts->items;
    const uint32_t item_count = opts->item_count;

    if (!ui || !menu_id || !items || item_count == 0)
        return -1;

    float menu_w = (opts && opts->width > 0.0f) ? opts->width : 180.0f;
    float item_h = (opts && opts->item_height > 0.0f) ? opts->item_height : 28.0f;

    optics_ui_place_opts place_opts = {
        .box =
            {
                .id = menu_id,
                .width = menu_w,
            },
        .band = OPTICS_UI_BAND_POPUP,
        .mode = OPTICS_UI_PLACE_ANCHORED,
        .rect = anchor_rect,
        .transient = true,
        .interactive = true,
        .layout =
            {
                .box = {.width = menu_w},
                .bg = ui->theme.color_surface_elevated,
                .border = ui->theme.color_border,
                .border_width = 1.0f,
                .radius = ui->theme.corner_radius > 0.0f ? ui->theme.corner_radius : 6.0f,
                .pad = 4.0f,
                .gap = 2.0f,
            },
    };

    if (!optics_ui_place_begin(ui, &place_opts))
        return -1;

    optics_ui_id state_id = optics_ui_current_id(ui, "menu_kb_state");
    optics_ui_node *state_node = optics_ui__store_touch(ui, state_id);
    popup_menu_state *state =
        state_node ? (popup_menu_state *)optics_ui_node_state(state_node, sizeof(popup_menu_state))
                   : nullptr;
    if (state && state->focused_index < -1) {
        state->focused_index = -1;
    }

    int32_t activated = -1;
    bool escape = false;

    /* Keyboard navigation: Down/Up cycle items, Enter activates, Escape dismisses */
    for (uint32_t k = 0; k < ui->input.key_count; k++) {
        optics_ui_key_event ev = ui->input.keys[k];
        if (!ev.pressed)
            continue;

        if (ev.key == OPTICS_UI_KEY_DOWN) {
            if (state) {
                int32_t cur = state->focused_index;
                for (uint32_t step = 1; step <= item_count; step++) {
                    int32_t nxt = (cur + (int32_t)step) % (int32_t)item_count;
                    if (!items[nxt].is_separator && !items[nxt].disabled) {
                        state->focused_index = nxt;
                        break;
                    }
                }
                optics_ui_consume_key(ui, OPTICS_UI_KEY_DOWN);
            }
        } else if (ev.key == OPTICS_UI_KEY_UP) {
            if (state) {
                int32_t cur = state->focused_index < 0 ? 0 : state->focused_index;
                for (uint32_t step = 1; step <= item_count; step++) {
                    int32_t nxt = (cur - (int32_t)step + (int32_t)item_count) % (int32_t)item_count;
                    if (!items[nxt].is_separator && !items[nxt].disabled) {
                        state->focused_index = nxt;
                        break;
                    }
                }
                optics_ui_consume_key(ui, OPTICS_UI_KEY_UP);
            }
        } else if (ev.key == OPTICS_UI_KEY_RETURN) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count &&
                !items[state->focused_index].disabled &&
                !items[state->focused_index].is_separator) {
                activated = state->focused_index;
                optics_ui_consume_key(ui, OPTICS_UI_KEY_RETURN);
            }
        } else if (ev.key == OPTICS_UI_KEY_ESCAPE) {
            escape = true;
            optics_ui_consume_key(ui, OPTICS_UI_KEY_ESCAPE);
        }
    }

    for (uint32_t i = 0; i < item_count; i++) {
        const optics_ui_menu_item *it = &items[i];

        if (it->is_separator) {
            optics_ui_separator(ui, &(optics_ui_separator_opts){
                                        .axis = OPTICS_UI_ROW,
                                        .thickness = 1.0f,
                                    });
            continue;
        }

        char item_id[32];
        snprintf(item_id, sizeof(item_id), "m_%u", i);

        bool is_focused = (state && state->focused_index == (int32_t)i);
        optics_gfx_color item_bg =
            (is_focused && !it->disabled) ? ui->theme.color_hover : (optics_gfx_color){};

        optics_ui_response r =
            optics_ui_pressable_begin(ui, &(optics_ui_pressable_opts){
                                              .box =
                                                  {
                                                      .id = item_id,
                                                      .width = menu_w - 8.0f,
                                                      .height = item_h,
                                                      .disabled = it->disabled,
                                                  },
                                              .label = it->label ? it->label : item_id,
                                              .layout =
                                                  {
                                                      .gap = 6.0f,
                                                      .pad = 4.0f,
                                                      .bg = item_bg,
                                                      .radius = ui->theme.corner_radius,
                                                      .cross = OPTICS_UI_CENTER,
                                                  },
                                          });

        if (r.hovered && state && !it->disabled) {
            state->focused_index = (int32_t)i;
        }

        if (it->icon != 0) {
            optics_ui_icon(ui, &(optics_ui_icon_opts){
                                   .id = it->icon,
                                   .size = 14.0f,
                               });
        }

        optics_ui_label(ui, &(optics_ui_label_opts){
                                .box = {.flex = 1.0f},
                                .text = it->label ? it->label : "",
                            });

        if (it->shortcut && it->shortcut[0]) {
            optics_ui_label(ui, &(optics_ui_label_opts){
                                    .box =
                                        {
                                            .style =
                                                {
                                                    .fields = OPTICS_UI_STYLE_FG,
                                                    .fg = ui->theme.color_disabled,
                                                },
                                        },
                                    .text = it->shortcut,
                                });
        }

        optics_ui_pressable_end(ui);

        if (r.clicked && !it->disabled) {
            activated = (int32_t)i;
        }
    }

    optics_ui_place_end(ui);

    if (activated >= 0 || escape) {
        optics_ui_place_close(ui, menu_id);
    }

    return activated;
}

int32_t optics_ui_menubar(optics_ui *ui, const optics_ui_menubar_opts *opts) {
    if (!ui || !opts || !opts->items || opts->item_count == 0)
        return -1;
    const char *id_str = opts->id;
    const optics_ui_menubar_item *items = opts->items;
    uint32_t item_count = opts->item_count;
    optics_gfx_rect *out_anchor = opts->out_anchor;

    int32_t current_open = -1;
    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].menu_id && optics_ui_place_is_open(ui, items[i].menu_id)) {
            current_open = (int32_t)i;
            break;
        }
    }

    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .gap = 2.0f,
                                .pad = 2.0f,
                                .cross = OPTICS_UI_CENTER,
                            });

    int32_t result_open = current_open;
    const char *pending_open = nullptr;
    const char *pending_close = nullptr;

    for (uint32_t i = 0; i < item_count; i++) {
        const optics_ui_menubar_item *it = &items[i];
        bool is_open = (current_open == (int32_t)i);
        char btn_id[64];
        snprintf(btn_id, sizeof(btn_id), "%s_%s", id_str ? id_str : "menubar",
                 it->title ? it->title : "item");

        optics_ui_response r = optics_ui_button(ui, &(optics_ui_button_opts){
                                                        .box = {.id = btn_id},
                                                        .label = it->title,
                                                        .variant = OPTICS_UI_BUTTON_SUBTLE,
                                                        .active = is_open,
                                                    });

        if (is_open && out_anchor) {
            *out_anchor = r.rect;
        }

        if (r.clicked) {
            if (is_open) {
                pending_close = it->menu_id;
                result_open = -1;
            } else {
                if (current_open >= 0) {
                    pending_close = items[current_open].menu_id;
                }
                pending_open = it->menu_id;
                result_open = (int32_t)i;
                if (out_anchor)
                    *out_anchor = r.rect;
            }
        } else if (r.hovered && current_open >= 0 && current_open != (int32_t)i) {
            /* Hover-switch while another menu is already open */
            pending_close = items[current_open].menu_id;
            pending_open = it->menu_id;
            result_open = (int32_t)i;
            if (out_anchor)
                *out_anchor = r.rect;
        }
    }

    optics_ui_row_end(ui);

    if (pending_close) {
        optics_ui_place_close(ui, pending_close);
    }
    if (pending_open) {
        optics_ui_place_open(ui, pending_open);
    }

    return result_open;
}
