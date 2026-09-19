/*
 * gallery.h — shared definitions, data models, and contracts for material_gallery.
 */

#ifndef MATERIAL_GALLERY_H
#define MATERIAL_GALLERY_H

#include <flux/effect.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <iris/cursor.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>
#include <prism/prism.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum view_mode {
    VIEW_GRID = 1,
    VIEW_GLASS = 2,
    VIEW_FROST = 3,
    VIEW_ACRYLIC = 4,
    VIEW_MICA = 5,
} view_mode;

typedef enum backdrop_kind {
    BACKDROP_DYNAMIC = 0,
    BACKDROP_AURORA = 1,
    BACKDROP_SPATIAL_GRID = 2,
    BACKDROP_BLACK = 3,
    BACKDROP_WHITE = 4,
    BACKDROP_COUNT,
} backdrop_kind;

typedef struct backdrop_info {
    backdrop_kind kind;
    const char *label;
    lens_icon_id icon;
} backdrop_info;

extern const backdrop_info g_backdrops[BACKDROP_COUNT];

typedef struct gallery_layout {
    float margin_x;
    float header_h;
    float margin_bottom;
    float avail_w;
    float avail_h;
    float polarity;
    float scale;
} gallery_layout;

typedef struct gallery_app {
    view_mode view;
    backdrop_kind backdrop;
    flux_rect backdrop_btn_rect;
    bool animating;
    bool dark_mode;
    bool inactive_fallback;
    bool glass_amber;
    bool frost_vibrancy;
    bool acrylic_grain;
    bool mica_alt;
    float time;

    /* GPU resources */
    flux_arena arena;
    flux_image *capture;
    flux_blur_filter *blur_filter;
    prism_liquid_glass_filter *glass_filter;
    prism_frosted_filter *frosted_filter;
    prism_acrylic_filter *acrylic_filter;
    prism_mica_filter *mica_filter;

    /* Frame outputs */
    flux_image *glass_out;
    flux_image *frost_out;
    flux_image *acrylic_out;
    flux_image *mica_out;

    uint32_t width_px;
    uint32_t height_px;
    float scale;
    bool smoke_mode;
    uint32_t smoke_frames;
} gallery_app;

/* UI and theme */
void gallery_apply_theme(gallery_app *app, lens *ui);
void gallery_ui_build(gallery_app *app, lens *ui, const lens_input *in);

/* Backdrop provider */
void gallery_backdrop_render(gallery_app *app, flux_canvas *canvas, float W, float H);

/* View preparers */
void gallery_view_grid_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                               const gallery_layout *l);
void gallery_view_glass_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                const gallery_layout *l);
void gallery_view_frosted_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l);
void gallery_view_acrylic_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                                  const gallery_layout *l);
void gallery_view_mica_prepare(gallery_app *app, flux_frame *frame, flux_image *blurred,
                               const gallery_layout *l);

#endif /* MATERIAL_GALLERY_H */
