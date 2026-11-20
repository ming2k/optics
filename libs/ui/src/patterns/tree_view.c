/* tree_view.c — hierarchical outliner tree view pattern (ADR-Pattern-006). */

#include "../internal.h"
#include <optics/ui/patterns.h>

#include <stdio.h>

typedef struct tree_view_state {
    int32_t focused_index;
} tree_view_state;

optics_ui_tree_action optics_ui_tree_view(optics_ui *ui, const optics_ui_tree_opts *opts) {
    if (!opts)
        return (optics_ui_tree_action){};
    const char *id_str = opts->id;
    optics_ui_tree_item *items = opts->items;
    const uint32_t item_count = opts->item_count;

    optics_ui_tree_action action = {
        .kind = OPTICS_UI_TREE_ACTION_NONE,
        .index = 0,
    };

    if (!ui || !items || item_count == 0)
        return action;

    optics_ui_push_id(ui, id_str ? id_str : "tree_view");

    optics_ui_id state_id = optics_ui_current_id(ui, "tree_state");
    optics_ui_node *state_node = lensi_store_touch(ui, state_id);
    tree_view_state *state =
        state_node ? (tree_view_state *)optics_ui_node_state(state_node, sizeof(tree_view_state))
                   : nullptr;
    if (state && state->focused_index < 0) {
        state->focused_index = 0;
    }

    /* Keyboard navigation: Up / Down move selection, Left collapses, Right expands, Enter activates
     */
    for (uint32_t k = 0; k < ui->input.key_count; k++) {
        optics_ui_key_event ev = ui->input.keys[k];
        if (!ev.pressed)
            continue;

        if (ev.key == OPTICS_UI_KEY_UP) {
            if (state && state->focused_index > 0) {
                state->focused_index--;
                action.kind = OPTICS_UI_TREE_ACTION_SELECT;
                action.index = (uint32_t)state->focused_index;
                optics_ui_consume_key(ui, OPTICS_UI_KEY_UP);
            }
        } else if (ev.key == OPTICS_UI_KEY_DOWN) {
            if (state && state->focused_index + 1 < (int32_t)item_count) {
                state->focused_index++;
                action.kind = OPTICS_UI_TREE_ACTION_SELECT;
                action.index = (uint32_t)state->focused_index;
                optics_ui_consume_key(ui, OPTICS_UI_KEY_DOWN);
            }
        } else if (ev.key == OPTICS_UI_KEY_LEFT) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                optics_ui_tree_item *cur = &items[state->focused_index];
                if (cur->expanded) {
                    cur->expanded = false;
                    action.kind = OPTICS_UI_TREE_ACTION_TOGGLE;
                    action.index = (uint32_t)state->focused_index;
                } else if (cur->depth > 0) {
                    for (int p = state->focused_index - 1; p >= 0; p--) {
                        if (items[p].depth < cur->depth) {
                            state->focused_index = p;
                            action.kind = OPTICS_UI_TREE_ACTION_SELECT;
                            action.index = (uint32_t)p;
                            break;
                        }
                    }
                }
                optics_ui_consume_key(ui, OPTICS_UI_KEY_LEFT);
            }
        } else if (ev.key == OPTICS_UI_KEY_RIGHT) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                optics_ui_tree_item *cur = &items[state->focused_index];
                if (!cur->is_leaf && !cur->expanded) {
                    cur->expanded = true;
                    action.kind = OPTICS_UI_TREE_ACTION_TOGGLE;
                    action.index = (uint32_t)state->focused_index;
                }
                optics_ui_consume_key(ui, OPTICS_UI_KEY_RIGHT);
            }
        } else if (ev.key == OPTICS_UI_KEY_RETURN) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                action.kind = OPTICS_UI_TREE_ACTION_ACTIVATE;
                action.index = (uint32_t)state->focused_index;
                optics_ui_consume_key(ui, OPTICS_UI_KEY_RETURN);
            }
        }
    }

    float row_h = (opts && opts->row_height > 0.0f) ? opts->row_height : 26.0f;
    float indent_w = (opts && opts->indent_per_level > 0.0f) ? opts->indent_per_level : 16.0f;

    bool scrollable = (opts && opts->height > 0.0f);
    if (scrollable) {
        optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){
                                       .box = {.id = "tree_scroll", .height = opts->height},
                                       .autohide = true,
                                   });
    } else {
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){.gap = 1.0f, .pad = 0.0f});
    }

    for (uint32_t i = 0; i < item_count; i++) {
        optics_ui_tree_item *item = &items[i];
        char row_id[32];
        snprintf(row_id, sizeof(row_id), "node_%u", i);

        bool is_focused = (state && state->focused_index == (int32_t)i);
        optics_gfx_color row_bg =
            (item->selected || is_focused) ? ui->theme.color_active : (optics_gfx_color){};

        optics_ui_response row_resp =
            optics_ui_pressable_begin(ui, &(optics_ui_pressable_opts){
                                              .box = {.id = row_id, .height = row_h},
                                              .label = row_id,
                                              .layout =
                                                  {
                                                      .gap = 4.0f,
                                                      .pad = 2.0f,
                                                      .bg = row_bg,
                                                      .radius = ui->theme.corner_radius,
                                                      .cross = OPTICS_UI_CENTER,
                                                  },
                                          });

        /* Row selection on click */
        if (row_resp.clicked && action.kind == OPTICS_UI_TREE_ACTION_NONE) {
            if (state)
                state->focused_index = (int32_t)i;
            action.kind = OPTICS_UI_TREE_ACTION_SELECT;
            action.index = i;
        }

        /* 1. Indentation level */
        if (item->depth > 0) {
            optics_ui_spacer(ui, item->depth * indent_w);
        }

        /* 2. Disclosure toggle chevron */
        if (!item->is_leaf) {
            char chev_id[32];
            snprintf(chev_id, sizeof(chev_id), "chev_%u", i);
            optics_ui_icon_id icon =
                item->expanded ? OPTICS_UI_ICON_CHEVRON_DOWN : OPTICS_UI_ICON_CHEVRON_RIGHT;

            optics_ui_response btn_r = optics_ui_button(
                ui, &(optics_ui_button_opts){
                        .box = {.id = chev_id, .width = 18.0f, .height = row_h - 4.0f},
                        .icon = icon,
                        .variant = OPTICS_UI_BUTTON_SUBTLE,
                    });

            if (btn_r.clicked) {
                item->expanded = !item->expanded;
                action.kind = OPTICS_UI_TREE_ACTION_TOGGLE;
                action.index = i;
            }
        } else {
            optics_ui_spacer(ui, 18.0f);
        }

        /* 3. Item Icon */
        if (item->icon != 0) {
            optics_ui_icon(ui, &(optics_ui_icon_opts){
                                   .id = item->icon,
                                   .size = 14.0f,
                               });
        }

        /* 4. Item Label */
        optics_ui_label(ui, &(optics_ui_label_opts){
                                .box = {.flex = 1.0f},
                                .text = item->label ? item->label : "",
                            });

        /* 5. Optional Trailing Badge */
        if (item->badge && item->badge[0]) {
            optics_ui_label(ui, &(optics_ui_label_opts){
                                    .box =
                                        {
                                            .style =
                                                {
                                                    .fields = OPTICS_UI_STYLE_FG,
                                                    .fg = ui->theme.color_disabled,
                                                },
                                        },
                                    .text = item->badge,
                                });
        }

        optics_ui_pressable_end(ui);
    }

    if (scrollable) {
        optics_ui_scroll_end(ui);
    } else {
        optics_ui_column_end(ui);
    }

    optics_ui_pop_id(ui);

    return action;
}
