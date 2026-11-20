/*
 * widget_gallery/page_layout_data.c — TabStrip, Grid, TreeView, and DataGrid.
 */

#include "gallery.h"

#include <stdio.h>

void wg_page_layout_data(optics_ui *ui, const ds_palette *p, wg_app *app) {
    ds_page_header(ui, p, "Layout & Data",
                   "Tab strips, responsive CSS-style Grid, hierarchical TreeView, and DataGrid.");

    /* Tab strip */
    ds_card_begin(ui, p, "Tab Strip",
                  "Closable tabs with interactive selection and new-tab action.");
    static const char *const TAB_TITLES[6] = {
        "Overview", "Shaders", "Pipeline", "Textures", "Metrics", "Console",
    };
    optics_ui_tab_item tabs[6] = {};
    if (app->tab_count < 1)
        app->tab_count = 1;
    if (app->tab_count > 6)
        app->tab_count = 6;
    for (uint32_t i = 0; i < app->tab_count; i++) {
        tabs[i] = (optics_ui_tab_item){.title = TAB_TITLES[i], .closable = app->tab_count > 1};
    }
    optics_ui_tab_action tab_act =
        optics_ui_tab_strip(ui, &(optics_ui_tab_strip_opts){
                                    .id = "wg_tabs",
                                    .tabs = tabs,
                                    .tab_count = app->tab_count,
                                    .active_index = app->active_tab,
                                    .show_new_button = app->tab_count < 6,
                                });
    if (tab_act.kind == OPTICS_UI_TAB_ACTION_SELECT) {
        app->active_tab = tab_act.index;
    } else if (tab_act.kind == OPTICS_UI_TAB_ACTION_NEW && app->tab_count < 6) {
        app->active_tab = app->tab_count;
        app->tab_count++;
    } else if (tab_act.kind == OPTICS_UI_TAB_ACTION_CLOSE && app->tab_count > 1) {
        app->tab_count--;
        if (app->active_tab >= app->tab_count)
            app->active_tab = app->tab_count - 1;
    }

    char tab_info[96];
    snprintf(tab_info, sizeof tab_info, "Active tab: %s (%u open)", TAB_TITLES[app->active_tab],
             app->tab_count);
    optics_ui_label(ui,
                    &(optics_ui_label_opts){
                        .text = tab_info,
                        .size = 13.0f,
                        .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                    });
    ds_card_end(ui);

    /* Responsive 3-column Grid */
    ds_card_begin(ui, p, "Responsive Grid", "Equal-track 3-column metric cards.");
    optics_ui_grid_begin(
        ui, &(optics_ui_grid_opts){.columns = 3, .col_gap = DS_SPACE_MD, .row_gap = DS_SPACE_MD});
    static const struct {
        const char *k;
        const char *v;
        const char *sub;
    } metrics[3] = {
        {"FRAME ALLOCS", "0 bytes", "Steady-state arena"},
        {"LIBRARIES", "9 modules", "One-way DAG"},
        {"GPU API", "Vulkan 1.3", "Dynamic rendering"},
    };
    for (int i = 0; i < 3; i++) {
        optics_ui_style tile_st = OPTICS_UI_STYLE_INIT;
        tile_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_CORNER_RADIUS;
        tile_st.bg = p->subtle_bg;
        tile_st.corner_radius = 8.0f;
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                       .pad = DS_SPACE_MD,
                                       .gap = 4,
                                       .box = {.style = tile_st},
                                   });
        optics_ui_label(ui,
                        &(optics_ui_label_opts){
                            .text = metrics[i].k,
                            .size = 11.0f,
                            .weight = 650.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_muted}},
                        });
        optics_ui_label(ui, &(optics_ui_label_opts){
                                .text = metrics[i].v,
                                .size = 18.0f,
                                .weight = 650.0f,
                            });
        optics_ui_label(
            ui, &(optics_ui_label_opts){
                    .text = metrics[i].sub,
                    .size = 12.0f,
                    .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p->text_secondary}},
                });
        optics_ui_close(ui);
    }
    optics_ui_grid_end(ui);
    ds_card_end(ui);

    /* TreeView */
    ds_card_begin(ui, p, "Tree View", "Expandable hierarchy with trailing metadata badges.");
    optics_ui_tree_action tree_act = optics_ui_tree_view(ui, &(optics_ui_tree_opts){
                                                                 .id = "wg_tree",
                                                                 .items = app->tree_items,
                                                                 .item_count = 7,
                                                                 .height = 196.0f,
                                                             });
    if (tree_act.kind == OPTICS_UI_TREE_ACTION_SELECT && tree_act.index < 7) {
        for (uint32_t i = 0; i < 7; i++)
            app->tree_items[i].selected = (i == tree_act.index);
    }
    ds_card_end(ui);

    /* DataGrid */
    ds_card_begin(ui, p, "Data Grid",
                  "Sortable headers, draggable column dividers, and striped rows.");
    static const char *const CELLS[5 * 4] = {
        "gfx",
        "liboptics_gfx",
        "Vulkan 1.3",
        "RHI, 2D/3D canvas, compute",
        "text",
        "liboptics_text",
        "gfx, HarfBuzz",
        "Shaping, layout, glyph atlas",
        "window",
        "liboptics_window",
        "gfx",
        "Wayland / Win32 / Cocoa",
        "ui",
        "liboptics_ui",
        "gfx, text, svg",
        "Headless immediate-mode UI",
        "app",
        "liboptics_app",
        "window, ui",
        "Application shell",
    };
    optics_ui_datagrid(ui, &(optics_ui_datagrid_opts){
                               .id = "wg_datagrid",
                               .cols = app->grid_cols,
                               .col_count = 4,
                               .cell_texts = CELLS,
                               .row_count = 5,
                               .selected_row = &app->selected_row,
                               .height = 184.0f,
                               .striped = true,
                           });
    ds_card_end(ui);
}
