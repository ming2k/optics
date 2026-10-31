/* menu.c — cascading popup menu and menubar pattern (ADR-Pattern-007). */

#include "../../include/lens/patterns.h"
#include "../internal.h"

#include <stdio.h>

typedef struct popup_menu_state {
    int32_t focused_index;
} popup_menu_state;

int32_t lens_popup_menu(lens *ui, const char *menu_id, flux_rect anchor_rect,
                        const lens_menu_item *items, uint32_t item_count,
                        const lens_menu_opts *opts) {
    if (!ui || !menu_id || !items || item_count == 0)
        return -1;

    float menu_w = (opts && opts->width > 0.0f) ? opts->width : 180.0f;
    float item_h = (opts && opts->item_height > 0.0f) ? opts->item_height : 28.0f;

    lens_place_opts place_opts = {
        .box =
            {
                .id = menu_id,
                .width = menu_w,
            },
        .band = LENS_BAND_POPUP,
        .mode = LENS_PLACE_ANCHORED,
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

    if (!lens_place_begin(ui, &place_opts))
        return -1;

    lens_id state_id = lens_current_id(ui, "menu_kb_state");
    lens_node *state_node = lensi_store_touch(ui, state_id);
    popup_menu_state *state =
        state_node ? (popup_menu_state *)lens_node_state(state_node, sizeof(popup_menu_state))
                   : nullptr;
    if (state && state->focused_index < -1) {
        state->focused_index = -1;
    }

    int32_t activated = -1;
    bool escape = false;

    /* Keyboard navigation: Down/Up cycle items, Enter activates, Escape dismisses */
    for (uint32_t k = 0; k < ui->input.key_count; k++) {
        lens_key_event ev = ui->input.keys[k];
        if (!ev.pressed)
            continue;

        if (ev.key == LENS_KEY_DOWN) {
            if (state) {
                int32_t cur = state->focused_index;
                for (uint32_t step = 1; step <= item_count; step++) {
                    int32_t nxt = (cur + (int32_t)step) % (int32_t)item_count;
                    if (!items[nxt].is_separator && !items[nxt].disabled) {
                        state->focused_index = nxt;
                        break;
                    }
                }
                lens_consume_key(ui, LENS_KEY_DOWN);
            }
        } else if (ev.key == LENS_KEY_UP) {
            if (state) {
                int32_t cur = state->focused_index < 0 ? 0 : state->focused_index;
                for (uint32_t step = 1; step <= item_count; step++) {
                    int32_t nxt = (cur - (int32_t)step + (int32_t)item_count) % (int32_t)item_count;
                    if (!items[nxt].is_separator && !items[nxt].disabled) {
                        state->focused_index = nxt;
                        break;
                    }
                }
                lens_consume_key(ui, LENS_KEY_UP);
            }
        } else if (ev.key == LENS_KEY_RETURN) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count &&
                !items[state->focused_index].disabled &&
                !items[state->focused_index].is_separator) {
                activated = state->focused_index;
                lens_consume_key(ui, LENS_KEY_RETURN);
            }
        } else if (ev.key == LENS_KEY_ESCAPE) {
            escape = true;
            lens_consume_key(ui, LENS_KEY_ESCAPE);
        }
    }

    for (uint32_t i = 0; i < item_count; i++) {
        const lens_menu_item *it = &items[i];

        if (it->is_separator) {
            lens_separator(ui, &(lens_separator_opts){
                                   .axis = LENS_ROW,
                                   .thickness = 1.0f,
                               });
            continue;
        }

        char item_id[32];
        snprintf(item_id, sizeof(item_id), "m_%u", i);

        bool is_focused = (state && state->focused_index == (int32_t)i);
        flux_color item_bg =
            (is_focused && !it->disabled) ? ui->theme.color_hover : (flux_color){0};

        lens_response r = lens_pressable_begin(ui, &(lens_pressable_opts){
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
                                                               .cross = LENS_CENTER,
                                                           },
                                                   });

        if (r.hovered && state && !it->disabled) {
            state->focused_index = (int32_t)i;
        }

        if (it->icon != 0) {
            lens_icon(ui, &(lens_icon_opts){
                              .id = it->icon,
                              .size = 14.0f,
                          });
        }

        lens_label(ui, &(lens_label_opts){
                           .box = {.flex = 1.0f},
                           .text = it->label ? it->label : "",
                       });

        if (it->shortcut && it->shortcut[0]) {
            lens_label(ui, &(lens_label_opts){
                               .box =
                                   {
                                       .style =
                                           {
                                               .fields = LENS_STYLE_FG,
                                               .fg = ui->theme.color_disabled,
                                           },
                                   },
                               .text = it->shortcut,
                           });
        }

        lens_pressable_end(ui);

        if (r.clicked && !it->disabled) {
            activated = (int32_t)i;
        }
    }

    lens_place_end(ui);

    if (activated >= 0 || escape) {
        lens_place_close(ui, menu_id);
    }

    return activated;
}

int32_t lens_menubar(lens *ui, const char *id_str, const lens_menubar_item *items,
                     uint32_t item_count, flux_rect *out_anchor) {
    if (!ui || !items || item_count == 0)
        return -1;

    int32_t current_open = -1;
    for (uint32_t i = 0; i < item_count; i++) {
        if (items[i].menu_id && lens_place_is_open(ui, items[i].menu_id)) {
            current_open = (int32_t)i;
            break;
        }
    }

    lens_row_begin(ui, &(lens_layout_opts){
                           .gap = 2.0f,
                           .pad = 2.0f,
                           .cross = LENS_CENTER,
                       });

    int32_t result_open = current_open;
    const char *pending_open = nullptr;
    const char *pending_close = nullptr;

    for (uint32_t i = 0; i < item_count; i++) {
        const lens_menubar_item *it = &items[i];
        bool is_open = (current_open == (int32_t)i);
        char btn_id[64];
        snprintf(btn_id, sizeof(btn_id), "%s_%s", id_str ? id_str : "menubar",
                 it->title ? it->title : "item");

        lens_response r = lens_button(ui, &(lens_button_opts){
                                              .box = {.id = btn_id},
                                              .label = it->title,
                                              .variant = LENS_BUTTON_SUBTLE,
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

    lens_row_end(ui);

    if (pending_close) {
        lens_place_close(ui, pending_close);
    }
    if (pending_open) {
        lens_place_open(ui, pending_open);
    }

    return result_open;
}
