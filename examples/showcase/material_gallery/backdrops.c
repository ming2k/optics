/*
 * backdrops.c — background generation for material evaluation:
 * dynamic bouncing TV screensaver card, spatial calibration test grid, pure black, and pure white.
 */

#include "gallery.h"
#include <math.h>

const backdrop_info g_backdrops[BACKDROP_COUNT] = {
    {BACKDROP_BOUNCING_TV, "Bouncing TV", LENS_ICON_TV},
    {BACKDROP_SPATIAL_GRID, "Spatial Grid", LENS_ICON_GRID},
    {BACKDROP_BLACK, "Pure Black", LENS_ICON_MOON},
    {BACKDROP_WHITE, "Pure White", LENS_ICON_SUN},
};

static void draw_bouncing_tv(flux_canvas *c, flux_arena *arena, float W, float H,
                             gallery_app *app) {
    /* 1. Base deep atmospheric gradient */
    {
        flux_gradient_stop bg_stops[3] = {
            {0.0f, flux_color_rgba_premul(12, 14, 22, 255)},
            {0.5f, flux_color_rgba_premul(18, 22, 34, 255)},
            {1.0f, flux_color_rgba_premul(10, 12, 20, 255)},
        };
        flux_brush bg =
            flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, bg_stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &bg);
    }

    /* 2. Soft background coordinate reference grid (subtle 64px grid lines) */
    {
        flux_path *grid = nullptr;
        (void)flux_path_create(&grid, arena);
        if (grid) {
            float step = 64.0f;
            for (float x = 0; x <= W; x += step) {
                flux_path_move_to(grid, x, 0);
                flux_path_line_to(grid, x, H);
            }
            for (float y = 0; y <= H; y += step) {
                flux_path_move_to(grid, 0, y);
                flux_path_line_to(grid, W, y);
            }
            flux_brush gb = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 12));
            flux_stroke_style gst = {.width = 1.0f};
            flux_canvas_stroke_path(c, grid, &gst, &gb);
        }
    }

    /* 3. The Bouncing TV Card */
    float box_w = 200.0f * app->scale;
    float box_h = 120.0f * app->scale;

    /* Initialize position and speed on first run or resize */
    if (!app->dvd_init) {
        app->dvd_x = 60.0f * app->scale;
        app->dvd_y = 90.0f * app->scale;
        app->dvd_vx = 180.0f * app->scale;
        app->dvd_vy = 130.0f * app->scale;
        app->dvd_bounce_count = 0;
        app->dvd_init = true;
    }

    /* Bounce physics update (when animating) */
    if (app->animating) {
        float dt = 0.016f;
        app->dvd_x += app->dvd_vx * dt;
        app->dvd_y += app->dvd_vy * dt;

        if (app->dvd_x <= 0.0f) {
            app->dvd_x = 0.0f;
            app->dvd_vx = fabsf(app->dvd_vx);
            app->dvd_bounce_count++;
        } else if (app->dvd_x + box_w >= W) {
            app->dvd_x = fmaxf(0.0f, W - box_w);
            app->dvd_vx = -fabsf(app->dvd_vx);
            app->dvd_bounce_count++;
        }

        if (app->dvd_y <= 0.0f) {
            app->dvd_y = 0.0f;
            app->dvd_vy = fabsf(app->dvd_vy);
            app->dvd_bounce_count++;
        } else if (app->dvd_y + box_h >= H) {
            app->dvd_y = fmaxf(0.0f, H - box_h);
            app->dvd_vy = -fabsf(app->dvd_vy);
            app->dvd_bounce_count++;
        }
    }

    float bx = app->dvd_x;
    float by = app->dvd_y;

    /* Glowing ambient halo */
    {
        flux_color glow_c;
        switch (app->dvd_bounce_count % 4) {
        case 0:
            glow_c = flux_color_rgba_premul(249, 115, 22, 90);
            break; /* Coral / Amber */
        case 1:
            glow_c = flux_color_rgba_premul(14, 165, 233, 90);
            break; /* Sky Cyan */
        case 2:
            glow_c = flux_color_rgba_premul(168, 85, 247, 90);
            break; /* Purple / Violet */
        case 3:
        default:
            glow_c = flux_color_rgba_premul(34, 197, 94, 90);
            break; /* Emerald */
        }
        flux_brush shadow_b = flux_brush_solid(glow_c);
        flux_geometry g_shadow = flux_geom_rrect(
            (flux_rect){bx - 12.0f, by - 8.0f, box_w + 24.0f, box_h + 20.0f}, 28.0f * app->scale);
        flux_canvas_draw_geometry(c, &g_shadow, &shadow_b);
    }

    /* Bouncing TV body: vibrant linear gradient */
    {
        flux_gradient_stop box_stops[3];
        switch (app->dvd_bounce_count % 4) {
        case 0: /* Sunrise Flare */
            box_stops[0] = (flux_gradient_stop){0.0f, flux_color_rgba_premul(244, 63, 94, 255)};
            box_stops[1] = (flux_gradient_stop){0.5f, flux_color_rgba_premul(249, 115, 22, 255)};
            box_stops[2] = (flux_gradient_stop){1.0f, flux_color_rgba_premul(234, 179, 8, 255)};
            break;
        case 1: /* Cyber Cyan */
            box_stops[0] = (flux_gradient_stop){0.0f, flux_color_rgba_premul(6, 182, 212, 255)};
            box_stops[1] = (flux_gradient_stop){0.5f, flux_color_rgba_premul(59, 130, 246, 255)};
            box_stops[2] = (flux_gradient_stop){1.0f, flux_color_rgba_premul(147, 51, 234, 255)};
            break;
        case 2: /* Neon Violet */
            box_stops[0] = (flux_gradient_stop){0.0f, flux_color_rgba_premul(168, 85, 247, 255)};
            box_stops[1] = (flux_gradient_stop){0.5f, flux_color_rgba_premul(236, 72, 153, 255)};
            box_stops[2] = (flux_gradient_stop){1.0f, flux_color_rgba_premul(244, 63, 94, 255)};
            break;
        case 3:
        default: /* Emerald Aurora */
            box_stops[0] = (flux_gradient_stop){0.0f, flux_color_rgba_premul(16, 185, 129, 255)};
            box_stops[1] = (flux_gradient_stop){0.5f, flux_color_rgba_premul(14, 165, 233, 255)};
            box_stops[2] = (flux_gradient_stop){1.0f, flux_color_rgba_premul(99, 102, 241, 255)};
            break;
        }
        flux_brush tv_b = flux_brush_linear_gradient(
            (flux_point){bx, by}, (flux_point){bx + box_w, by + box_h}, box_stops, 3);
        flux_geometry g_tv = flux_geom_rrect((flux_rect){bx, by, box_w, box_h}, 18.0f * app->scale);
        flux_canvas_draw_geometry(c, &g_tv, &tv_b);

        /* Crisp specular outline */
        flux_brush border_b = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 220));
        flux_geometry g_tv_border =
            flux_geom_rrect((flux_rect){bx, by, box_w, box_h}, 18.0f * app->scale);
        g_tv_border.stroke.width = 2.0f;
        flux_canvas_draw_geometry(c, &g_tv_border, &border_b);
    }

    /* High-contrast test geometry inside the TV box for optical refraction & dispersion analysis */
    {
        /* Diagonal calibration stripes */
        flux_path *tv_stripes = nullptr;
        (void)flux_path_create(&tv_stripes, arena);
        if (tv_stripes) {
            float step = 22.0f * app->scale;
            for (float d = -box_h; d < box_w + box_h; d += step) {
                float sx = bx + d;
                float sy = by;
                float ex = bx + d + box_h;
                float ey = by + box_h;
                if (sx < bx) {
                    sy += (bx - sx);
                    sx = bx;
                }
                if (ex > bx + box_w) {
                    ey -= (ex - (bx + box_w));
                    ex = bx + box_w;
                }
                if (sx < bx + box_w && ey > by) {
                    flux_path_move_to(tv_stripes, sx, sy);
                    flux_path_line_to(tv_stripes, ex, ey);
                }
            }
            flux_brush sb = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 110));
            flux_stroke_style sst = {.width = 3.0f * app->scale};
            flux_canvas_stroke_path(c, tv_stripes, &sst, &sb);
        }

        /* Center calibration disc */
        flux_canvas_fill_circle(c, bx + box_w * 0.5f, by + box_h * 0.5f, 22.0f * app->scale,
                                flux_color_rgba_premul(255, 255, 255, 240));
        flux_canvas_fill_circle(c, bx + box_w * 0.5f, by + box_h * 0.5f, 12.0f * app->scale,
                                flux_color_rgba_premul(20, 20, 30, 255));
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

void gallery_backdrop_render(gallery_app *app, flux_canvas *canvas, float W, float H) {
    if (app->backdrop == BACKDROP_BOUNCING_TV) {
        draw_bouncing_tv(canvas, &app->arena, W, H, app);
    } else if (app->backdrop == BACKDROP_SPATIAL_GRID) {
        draw_spatial_grid(canvas, &app->arena, W, H);
    }
    /* BACKDROP_BLACK and BACKDROP_WHITE are filled via clear_color in on_prepare */
}
