/* tree_view.c — hierarchical outliner tree view pattern (ADR-Pattern-006). */

#include "../../include/lens/patterns.h"
#include "../internal.h"

#include <stdio.h>

typedef struct tree_view_state {
    int32_t focused_index;
} tree_view_state;

lens_tree_action lens_tree_view(lens *ui, const char *id_str, lens_tree_item *items,
                                uint32_t item_count, const lens_tree_opts *opts) {
    lens_tree_action action = {
        .kind = LENS_TREE_ACTION_NONE,
        .index = 0,
    };

    if (!ui || !items || item_count == 0)
        return action;

    lens_push_id(ui, id_str ? id_str : "tree_view");

    lens_id state_id = lens_current_id(ui, "tree_state");
    lens_node *state_node = lensi_store_touch(ui, state_id);
    tree_view_state *state =
        state_node ? (tree_view_state *)lens_node_state(state_node, sizeof(tree_view_state))
                   : nullptr;
    if (state && state->focused_index < 0) {
        state->focused_index = 0;
    }

    /* Keyboard navigation: Up / Down move selection, Left collapses, Right expands, Enter activates
     */
    for (uint32_t k = 0; k < ui->input.key_count; k++) {
        lens_key_event ev = ui->input.keys[k];
        if (!ev.pressed)
            continue;

        if (ev.key == LENS_KEY_UP) {
            if (state && state->focused_index > 0) {
                state->focused_index--;
                action.kind = LENS_TREE_ACTION_SELECT;
                action.index = (uint32_t)state->focused_index;
                lens_consume_key(ui, LENS_KEY_UP);
            }
        } else if (ev.key == LENS_KEY_DOWN) {
            if (state && state->focused_index + 1 < (int32_t)item_count) {
                state->focused_index++;
                action.kind = LENS_TREE_ACTION_SELECT;
                action.index = (uint32_t)state->focused_index;
                lens_consume_key(ui, LENS_KEY_DOWN);
            }
        } else if (ev.key == LENS_KEY_LEFT) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                lens_tree_item *cur = &items[state->focused_index];
                if (cur->expanded) {
                    cur->expanded = false;
                    action.kind = LENS_TREE_ACTION_TOGGLE;
                    action.index = (uint32_t)state->focused_index;
                } else if (cur->depth > 0) {
                    for (int p = state->focused_index - 1; p >= 0; p--) {
                        if (items[p].depth < cur->depth) {
                            state->focused_index = p;
                            action.kind = LENS_TREE_ACTION_SELECT;
                            action.index = (uint32_t)p;
                            break;
                        }
                    }
                }
                lens_consume_key(ui, LENS_KEY_LEFT);
            }
        } else if (ev.key == LENS_KEY_RIGHT) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                lens_tree_item *cur = &items[state->focused_index];
                if (!cur->is_leaf && !cur->expanded) {
                    cur->expanded = true;
                    action.kind = LENS_TREE_ACTION_TOGGLE;
                    action.index = (uint32_t)state->focused_index;
                }
                lens_consume_key(ui, LENS_KEY_RIGHT);
            }
        } else if (ev.key == LENS_KEY_RETURN) {
            if (state && state->focused_index >= 0 && state->focused_index < (int32_t)item_count) {
                action.kind = LENS_TREE_ACTION_ACTIVATE;
                action.index = (uint32_t)state->focused_index;
                lens_consume_key(ui, LENS_KEY_RETURN);
            }
        }
    }

    float row_h = (opts && opts->row_height > 0.0f) ? opts->row_height : 26.0f;
    float indent_w = (opts && opts->indent_per_level > 0.0f) ? opts->indent_per_level : 16.0f;

    bool scrollable = (opts && opts->height > 0.0f);
    if (scrollable) {
        lens_scroll_begin(ui, &(lens_scroll_opts){
                                  .box = {.id = "tree_scroll", .height = opts->height},
                                  .autohide = true,
                              });
    } else {
        lens_column_begin(ui, &(lens_layout_opts){.gap = 1.0f, .pad = 0.0f});
    }

    for (uint32_t i = 0; i < item_count; i++) {
        lens_tree_item *item = &items[i];
        char row_id[32];
        snprintf(row_id, sizeof(row_id), "node_%u", i);

        bool is_focused = (state && state->focused_index == (int32_t)i);
        flux_color row_bg =
            (item->selected || is_focused) ? ui->theme.color_active : (flux_color){0};

        lens_response row_resp =
            lens_pressable_begin(ui, &(lens_pressable_opts){
                                         .box = {.id = row_id, .height = row_h},
                                         .label = row_id,
                                         .layout =
                                             {
                                                 .gap = 4.0f,
                                                 .pad = 2.0f,
                                                 .bg = row_bg,
                                                 .radius = ui->theme.corner_radius,
                                                 .cross = LENS_CENTER,
                                             },
                                     });

        /* Row selection on click */
        if (row_resp.clicked && action.kind == LENS_TREE_ACTION_NONE) {
            if (state)
                state->focused_index = (int32_t)i;
            action.kind = LENS_TREE_ACTION_SELECT;
            action.index = i;
        }

        /* 1. Indentation level */
        if (item->depth > 0) {
            lens_spacer(ui, item->depth * indent_w);
        }

        /* 2. Disclosure toggle chevron */
        if (!item->is_leaf) {
            char chev_id[32];
            snprintf(chev_id, sizeof(chev_id), "chev_%u", i);
            lens_icon_id icon = item->expanded ? LENS_ICON_CHEVRON_DOWN : LENS_ICON_CHEVRON_RIGHT;

            lens_response btn_r =
                lens_button(ui, &(lens_button_opts){
                                    .box = {.id = chev_id, .width = 18.0f, .height = row_h - 4.0f},
                                    .icon = icon,
                                    .variant = LENS_BUTTON_SUBTLE,
                                });

            if (btn_r.clicked) {
                item->expanded = !item->expanded;
                action.kind = LENS_TREE_ACTION_TOGGLE;
                action.index = i;
            }
        } else {
            lens_spacer(ui, 18.0f);
        }

        /* 3. Item Icon */
        if (item->icon != 0) {
            lens_icon(ui, &(lens_icon_opts){
                              .id = item->icon,
                              .size = 14.0f,
                          });
        }

        /* 4. Item Label */
        lens_label(ui, &(lens_label_opts){
                           .box = {.flex = 1.0f},
                           .text = item->label ? item->label : "",
                       });

        /* 5. Optional Trailing Badge */
        if (item->badge && item->badge[0]) {
            lens_label(ui, &(lens_label_opts){
                               .box =
                                   {
                                       .style =
                                           {
                                               .fields = LENS_STYLE_FG,
                                               .fg = ui->theme.color_disabled,
                                           },
                                   },
                               .text = item->badge,
                           });
        }

        lens_pressable_end(ui);
    }

    if (scrollable) {
        lens_scroll_end(ui);
    } else {
        lens_column_end(ui);
    }

    lens_pop_id(ui);

    return action;
}
