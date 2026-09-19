/*
 * backdrops.c — background generation for material evaluation:
 * dynamic animated waves, sunset aurora gradient, spatial test grid, pure black, and pure white.
 */

#include "gallery.h"
#include <math.h>

const backdrop_info g_backdrops[BACKDROP_COUNT] = {
    {BACKDROP_DYNAMIC, "Dynamic Waves", LENS_ICON_APERTURE},
    {BACKDROP_AURORA, "Sunset Aurora", LENS_ICON_SUN},
    {BACKDROP_SPATIAL_GRID, "Spatial Grid", LENS_ICON_GRID},
    {BACKDROP_BLACK, "Pure Black", LENS_ICON_MOON},
    {BACKDROP_WHITE, "Pure White", LENS_ICON_SUN},
};

static void draw_dynamic_waves(flux_canvas *c, flux_arena *arena, float W, float H, float t) {
    /* 1. Base deep gradient */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(20, 15, 35, 255)},
            {0.5f, flux_color_rgba_premul(45, 25, 60, 255)},
            {1.0f, flux_color_rgba_premul(15, 30, 45, 255)},
        };
        flux_brush bg =
            flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &bg);
    }

    /* 2. Moving glowing radial orbs */
    {
        float x1 = W * 0.25f + sinf(t * 0.7f) * (W * 0.15f);
        float y1 = H * 0.35f + cosf(t * 0.8f) * (H * 0.15f);
        flux_gradient_stop stops1[2] = {
            {0.0f, flux_color_rgba_premul(255, 90, 50, 220)},
            {1.0f, flux_color_rgba_premul(255, 90, 50, 0)},
        };
        flux_brush b1 = flux_brush_radial_gradient((flux_point){x1, y1}, W * 0.35f, stops1, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b1);

        float x2 = W * 0.75f + cosf(t * 0.6f) * (W * 0.15f);
        float y2 = H * 0.65f + sinf(t * 0.5f) * (H * 0.15f);
        flux_gradient_stop stops2[2] = {
            {0.0f, flux_color_rgba_premul(50, 150, 255, 220)},
            {1.0f, flux_color_rgba_premul(50, 150, 255, 0)},
        };
        flux_brush b2 = flux_brush_radial_gradient((flux_point){x2, y2}, W * 0.35f, stops2, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b2);

        float x3 = W * 0.50f + sinf(t * 1.1f) * (W * 0.20f);
        float y3 = H * 0.50f + cosf(t * 0.9f) * (H * 0.20f);
        flux_gradient_stop stops3[2] = {
            {0.0f, flux_color_rgba_premul(240, 200, 60, 200)},
            {1.0f, flux_color_rgba_premul(240, 200, 60, 0)},
        };
        flux_brush b3 = flux_brush_radial_gradient((flux_point){x3, y3}, W * 0.25f, stops3, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b3);
    }

    /* 3. Moving diagonal high-contrast zebra stripes to test refraction & edge distortion */
    {
        flux_path *stripes = nullptr;
        (void)flux_path_create(&stripes, arena);
        if (stripes) {
            float spacing = 50.0f;
            float shift = sinf(t) * 15.0f;
            for (float x = -H; x < W + H; x += spacing) {
                flux_path_move_to(stripes, x + shift, 0);
                flux_path_line_to(stripes, x + H + shift, H);
            }
            flux_brush stripe_brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 45));
            flux_stroke_style st = {.width = 6.0f, .cap = FLUX_CAP_ROUND};
            flux_canvas_stroke_path(c, stripes, &st, &stripe_brush);
        }
    }

    /* 4. Moving geometric rings and discs */
    {
        flux_brush ring_brush = flux_brush_solid(flux_color_rgba_premul(120, 240, 180, 120));
        flux_stroke_style ring_st = {.width = 4.0f};
        flux_path *rings = nullptr;
        (void)flux_path_create(&rings, arena);
        if (rings) {
            float cx = W * 0.5f + cosf(t * 0.4f) * (W * 0.3f);
            float cy = H * 0.5f + sinf(t * 0.4f) * (H * 0.3f);
            flux_path_add_circle(rings, cx, cy, 60.0f);
            flux_path_add_circle(rings, cx, cy, 100.0f);
            flux_canvas_stroke_path(c, rings, &ring_st, &ring_brush);
        }
    }
}

static void draw_spatial_grid(flux_canvas *c, flux_arena *arena, float W, float H) {
    /* Base deep dark slate background gradient */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(18, 22, 34, 255)},
            {0.5f, flux_color_rgba_premul(28, 36, 54, 255)},
            {1.0f, flux_color_rgba_premul(14, 20, 32, 255)},
        };
        flux_brush bg =
            flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &bg);
    }

    /* Static high-contrast diagonal zebra stripes for refraction and distortion analysis */
    {
        flux_path *stripes = nullptr;
        (void)flux_path_create(&stripes, arena);
        if (stripes) {
            float spacing = 48.0f;
            for (float x = -H; x < W + H; x += spacing) {
                flux_path_move_to(stripes, x, 0);
                flux_path_line_to(stripes, x + H, H);
            }
            flux_brush stripe_brush = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 55));
            flux_stroke_style st = {.width = 6.0f, .cap = FLUX_CAP_ROUND};
            flux_canvas_stroke_path(c, stripes, &st, &stripe_brush);
        }
    }

    /* Static concentric calibration circles in the center */
    {
        flux_brush ring_brush =
            flux_brush_solid(flux_color_rgba_premul(56, 189, 248, 140)); /* Sky cyan */
        flux_stroke_style ring_st = {.width = 3.0f};
        flux_path *rings = nullptr;
        (void)flux_path_create(&rings, arena);
        if (rings) {
            float cx = W * 0.5f;
            float cy = H * 0.5f;
            flux_path_add_circle(rings, cx, cy, 60.0f);
            flux_path_add_circle(rings, cx, cy, 120.0f);
            flux_path_add_circle(rings, cx, cy, 180.0f);
            flux_canvas_stroke_path(c, rings, &ring_st, &ring_brush);
        }
    }
}

static void draw_aurora(flux_canvas *c, float W, float H) {
    /* 1. Base scenic ambient gradient (sunset to deep night) */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(30, 20, 55, 255)}, /* Royal violet */
            {0.5f, flux_color_rgba_premul(75, 30, 65, 255)}, /* Crimson dusk */
            {1.0f, flux_color_rgba_premul(15, 45, 65, 255)}, /* Deep marine */
        };
        flux_brush bg =
            flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &bg);
    }

    /* 2. Golden sunset radial glow (upper right) */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(251, 146, 60, 220)}, /* Warm amber */
            {1.0f, flux_color_rgba_premul(251, 146, 60, 0)},
        };
        flux_brush b =
            flux_brush_radial_gradient((flux_point){W * 0.78f, H * 0.28f}, W * 0.42f, stops, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b);
    }

    /* 3. Deep cyan / electric teal aurora glow (lower left) */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(45, 212, 191, 200)}, /* Electric teal */
            {1.0f, flux_color_rgba_premul(45, 212, 191, 0)},
        };
        flux_brush b =
            flux_brush_radial_gradient((flux_point){W * 0.25f, H * 0.72f}, W * 0.40f, stops, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b);
    }

    /* 4. Vivid magenta / rose glow (center-top) */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(244, 63, 94, 180)}, /* Rose pink */
            {1.0f, flux_color_rgba_premul(244, 63, 94, 0)},
        };
        flux_brush b =
            flux_brush_radial_gradient((flux_point){W * 0.48f, H * 0.35f}, W * 0.30f, stops, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &b);
    }
}

void gallery_backdrop_render(gallery_app *app, flux_canvas *canvas, float W, float H) {
    if (app->backdrop == BACKDROP_DYNAMIC) {
        draw_dynamic_waves(canvas, &app->arena, W, H, app->time);
    } else if (app->backdrop == BACKDROP_SPATIAL_GRID) {
        draw_spatial_grid(canvas, &app->arena, W, H);
    } else if (app->backdrop == BACKDROP_AURORA) {
        draw_aurora(canvas, W, H);
    }
    /* BACKDROP_BLACK and BACKDROP_WHITE are filled via clear_color in on_prepare */
}
