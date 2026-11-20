/*
 * widget_gallery/gallery.h — shared state for the Optics Widget Gallery showcase.
 */

#ifndef OPTICS_WIDGET_GALLERY_H
#define OPTICS_WIDGET_GALLERY_H

#include "../common/design.h"

#include <optics/app.h>
#include <optics/motion.h>
#include <optics/ui.h>
#include <optics/ui/patterns.h>

#include <stdbool.h>
#include <stdint.h>

typedef enum wg_page : uint32_t {
    WG_PAGE_TYPOGRAPHY = 0,
    WG_PAGE_CONTROLS = 1,
    WG_PAGE_LAYOUT_DATA = 2,
    WG_PAGE_OVERLAYS_DND = 3,
    WG_PAGE_MOTION = 4,
    WG_PAGE_COUNT = 5,
} wg_page;

typedef struct wg_app {
    /* Shell state */
    wg_page page;
    float sidebar_width;
    bool dark;
    uint32_t text_scale_idx; /* 0 = 90%, 1 = 100%, 2 = 115% */
    bool reduced_motion;

    /* Controls page */
    int primary_clicks;
    bool opt_autosave;
    bool opt_telemetry;
    bool opt_vsync;
    float exposure;
    float contrast;
    uint32_t density_idx;
    char note_buf[256];
    size_t note_len;

    /* Layout & Data page */
    uint32_t active_tab;
    uint32_t tab_count;
    optics_ui_tree_item tree_items[7];
    optics_ui_datagrid_col grid_cols[4];
    uint32_t selected_row;

    /* Overlays & DnD page */
    char last_menu_action[64];
    char last_drop_payload[128];
    int drop_count;
    optics_gfx_rect menu_anchor;

    /* Motion page */
    uint32_t spring_preset; /* 0 = Snappy, 1 = Gentle, 2 = Bouncy */
    float stiffness;
    float damping;
    float target;
    optics_motion_spring spring;
} wg_app;

void wg_page_typography(optics_ui *ui, const ds_palette *p, wg_app *app);
void wg_page_controls(optics_ui *ui, const ds_palette *p, wg_app *app);
void wg_page_layout_data(optics_ui *ui, const ds_palette *p, wg_app *app);
void wg_page_overlays_dnd(optics_ui *ui, const ds_palette *p, wg_app *app);
void wg_page_motion(optics_ui *ui, const ds_palette *p, wg_app *app);

#endif /* OPTICS_WIDGET_GALLERY_H */
