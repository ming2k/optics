/*
 * widget_gallery/main.c — Flagship multi-page Optics UI Showcase (ADR-0171).
 */

#include "gallery.h"

#include <stdio.h>
#include <string.h>

static const char *const NAV_LABELS[WG_PAGE_COUNT] = {
    "Typography", "Controls", "Layout & Data", "Overlays & DnD", "Motion",
};

static const float TEXT_SCALES[3] = {0.90f, 1.00f, 1.15f};

static void wg_init(wg_app *app) {
    *app = (wg_app){
        .page = WG_PAGE_TYPOGRAPHY,
        .sidebar_width = 232.0f,
        .dark = true,
        .text_scale_idx = 1,
        .reduced_motion = false,
        .opt_autosave = true,
        .opt_vsync = true,
        .exposure = 0.65f,
        .contrast = 1.10f,
        .density_idx = 1,
        .active_tab = 0,
        .tab_count = 3,
        .selected_row = 0,
        .spring_preset = 0,
        .stiffness = 480.0f,
        .damping = 0.90f,
        .target = 0.0f,
    };

    const char *seed = "Optics immediate-mode UI with HarfBuzz shaping & IME support.";
    app->note_len = strlen(seed);
    memcpy(app->note_buf, seed, app->note_len + 1);
    snprintf(app->last_menu_action, sizeof app->last_menu_action, "None yet");
    snprintf(app->last_drop_payload, sizeof app->last_drop_payload, "Drop a draggable card here");

    app->tree_items[0] = (optics_ui_tree_item){
        .label = "optics", .depth = 0, .is_leaf = false, .expanded = true, .badge = "workspace"};
    app->tree_items[1] = (optics_ui_tree_item){
        .label = "libs/gfx", .depth = 1, .is_leaf = true, .selected = true, .badge = "Vulkan"};
    app->tree_items[2] =
        (optics_ui_tree_item){.label = "libs/ui", .depth = 1, .is_leaf = true, .badge = "headless"};
    app->tree_items[3] = (optics_ui_tree_item){
        .label = "libs/window", .depth = 1, .is_leaf = true, .badge = "Wayland"};
    app->tree_items[4] =
        (optics_ui_tree_item){.label = "libs/app", .depth = 1, .is_leaf = true, .badge = "shell"};
    app->tree_items[5] = (optics_ui_tree_item){
        .label = "examples", .depth = 0, .is_leaf = false, .expanded = true, .badge = "8 bins"};
    app->tree_items[6] = (optics_ui_tree_item){
        .label = "showcase/widget_gallery", .depth = 1, .is_leaf = true, .badge = "active"};

    app->grid_cols[0] = (optics_ui_datagrid_col){
        .title = "Module", .width = 140.0f, .sortable = true, .resizable = true};
    app->grid_cols[1] = (optics_ui_datagrid_col){
        .title = "Library", .width = 160.0f, .sortable = true, .resizable = true};
    app->grid_cols[2] = (optics_ui_datagrid_col){
        .title = "Dependencies", .width = 180.0f, .sortable = false, .resizable = true};
    app->grid_cols[3] = (optics_ui_datagrid_col){
        .title = "Role", .width = 240.0f, .sortable = false, .resizable = true};
}

static void on_build(optics_ui *ui, const optics_ui_input *in, void *user) {
    wg_app *app = user;

    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == OPTICS_UI_KEY_ESCAPE) {
            optics_app_quit();
            return;
        }
        if (key >= '1' && key <= '5') {
            app->page = (wg_page)(key - '1');
        }
    }

    const ds_palette p = ds_palette_for(app->dark);

    /* Root horizontal split: Sidebar | Handle | Main Area */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){.cross = OPTICS_UI_STRETCH});

    /* Sidebar */
    optics_ui_style sb_style = OPTICS_UI_STYLE_INIT;
    sb_style.fields = OPTICS_UI_STYLE_BG;
    sb_style.bg = p.sidebar_bg;
    optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                   .pad = DS_SPACE_LG,
                                   .gap = DS_SPACE_SM,
                                   .cross = OPTICS_UI_STRETCH,
                                   .box = {.width = app->sidebar_width, .style = sb_style},
                               });

    optics_ui_row_begin(ui,
                        &(optics_ui_layout_opts){.gap = DS_SPACE_SM, .cross = OPTICS_UI_CENTER});
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Optics",
                            .size = 18.0f,
                            .weight = 700.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p.text_primary}},
                        });
    ds_badge(ui, &p, "Gallery");
    optics_ui_close(ui);

    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "SECTIONS",
                            .size = 11.0f,
                            .weight = 650.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p.text_muted}},
                        });

    for (uint32_t i = 0; i < WG_PAGE_COUNT; i++) {
        bool selected = (app->page == (wg_page)i);
        if (optics_ui_selectable(ui,
                                 &(optics_ui_selectable_opts){
                                     .label = NAV_LABELS[i],
                                     .selected = selected,
                                 })
                .clicked) {
            app->page = (wg_page)i;
        }
    }

    optics_ui_flex(ui, 1.0f);
    optics_ui_separator(ui, &(optics_ui_separator_opts){});

    /* Appearance controls pinned to bottom of sidebar */
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "APPEARANCE",
                            .size = 11.0f,
                            .weight = 650.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p.text_muted}},
                        });

    if (optics_ui_button(ui,
                         &(optics_ui_button_opts){
                             .label = app->dark ? "Switch to Light" : "Switch to Dark",
                             .variant = OPTICS_UI_BUTTON_SUBTLE,
                         })
            .clicked) {
        app->dark = !app->dark;
        ds_apply_showcase_theme(ui, app->dark);
    }

    static const optics_ui_segmented_item scale_items[3] = {
        {.label = "90%"},
        {.label = "100%"},
        {.label = "115%"},
    };
    if (optics_ui_segmented_control(ui, &(optics_ui_segmented_opts){
                                            .id = "wg_text_scale",
                                            .items = scale_items,
                                            .item_count = 3,
                                            .selected_index = &app->text_scale_idx,
                                            .compact = true,
                                        })) {
        optics_ui_set_text_scale(ui, TEXT_SCALES[app->text_scale_idx]);
    }

    if (optics_ui_checkbox(ui,
                           &(optics_ui_checkbox_opts){
                               .label = "Reduced motion",
                               .value = &app->reduced_motion,
                           })
            .changed) {
        optics_ui_set_reduced_motion(ui, app->reduced_motion);
    }

    optics_ui_close(ui); /* sidebar */

    /* Interactive vertical split handle */
    optics_ui_split_handle_v(ui, "wg_sidebar_split", &app->sidebar_width,
                             &(optics_ui_split_opts){
                                 .min_size = 196.0f,
                                 .max_size = 340.0f,
                                 .handle_width = 1.0f,
                                 .hit_expand = 4.0f,
                             });

    /* Main scrollable canvas */
    optics_ui_flex(ui, 1.0f);
    if (optics_ui_scroll_begin(ui, &(optics_ui_scroll_opts){.box = {.id = "wg_main_scroll"}})) {
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                       .pad = DS_SPACE_XL,
                                       .gap = DS_SPACE_XL,
                                       .cross = OPTICS_UI_STRETCH,
                                   });

        switch (app->page) {
        case WG_PAGE_TYPOGRAPHY:
            wg_page_typography(ui, &p, app);
            break;
        case WG_PAGE_CONTROLS:
            wg_page_controls(ui, &p, app);
            break;
        case WG_PAGE_LAYOUT_DATA:
            wg_page_layout_data(ui, &p, app);
            break;
        case WG_PAGE_OVERLAYS_DND:
            wg_page_overlays_dnd(ui, &p, app);
            break;
        case WG_PAGE_MOTION:
            wg_page_motion(ui, &p, app);
            break;
        case WG_PAGE_COUNT:
        default:
            break;
        }

        optics_ui_close(ui);
        optics_ui_scroll_end(ui);
    }

    optics_ui_close(ui); /* root row */
}

static bool on_start(optics_ui *ui, optics_gfx_device *device, void *user) {
    (void)device;
    wg_app *app = user;
    ds_apply_showcase_theme(ui, app->dark);
    return true;
}

int main(void) {
    wg_app app;
    wg_init(&app);
    return optics_app_run(&(optics_app_opts){
        .title = "Optics — Widget Gallery",
        .app_id = "org.optics.showcase.widget-gallery",
        .width = 1120,
        .height = 760,
        .dark = app.dark,
        .start = on_start,
        .build = on_build,
        .user = &app,
    });
}
