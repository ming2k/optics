/*
 * canvas_hello — canonical 2D vector drawing starter for Flux (ADR-0097).
 *
 * Demonstrates 2D canvas primitives: paths, fills, strokes, gradients,
 * rounded rectangles, and image drawing, running natively on Iris.
 */

#include <flux/canvas.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct canvas_hello_app {
    flux_image *checker;
    flux_arena arena;
    float time;
} canvas_hello_app;

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    canvas_hello_app *app = user;

    if (flux_arena_init(&app->arena, 64 * 1024, nullptr) != FLUX_OK)
        return false;

    /* Procedurally generated 64x64 magenta/cyan checker texture */
    enum { TEX_W = 64, TEX_H = 64 };
    uint32_t pixels[TEX_W * TEX_H];
    for (int yy = 0; yy < TEX_H; ++yy) {
        for (int xx = 0; xx < TEX_W; ++xx) {
            bool check = ((xx / 8) ^ (yy / 8)) & 1;
            pixels[yy * TEX_W + xx] = check ? 0xFFFF00FFu : 0xFF00FFFFu;
        }
    }

    flux_image_desc idesc = {
        .type = FLUX_TYPE_IMAGE_DESC,
        .width = TEX_W,
        .height = TEX_H,
        .format = FLUX_FORMAT_RGBA8_UNORM,
        .initial_data = pixels,
    };
    if (flux_image_create(device, &idesc, &app->checker) != FLUX_OK) {
        flux_arena_deinit(&app->arena);
        return false;
    }

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    canvas_hello_app *app = user;
    if (app->checker) {
        flux_image_release(app->checker);
        app->checker = nullptr;
    }
    flux_arena_deinit(&app->arena);
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    (void)ui;
    (void)user;
    for (uint32_t k = 0; k < in->key_count; k++) {
        if (in->keys[k].pressed && (in->keys[k].key == LENS_KEY_ESCAPE || in->keys[k].key == 'q' ||
                                    in->keys[k].key == 'Q')) {
            iris_window_close();
            return;
        }
    }
    iris_request_animation_frame();
}

static void on_paint(flux_canvas *canvas, flux_device *device, float scale, void *user) {
    (void)device;
    canvas_hello_app *app = user;
    app->time += 0.016f;
    float t = app->time;

    int32_t log_w = 960, log_h = 540;
    iris_window_get_geometry(&log_w, &log_h);
    float W = (float)log_w;
    float H = (float)log_h;

    flux_canvas_save(canvas);
    flux_canvas_scale(canvas, scale, scale);

    /* Backdrop tile pattern */
    for (int gy = 0; gy < 8; ++gy) {
        for (int gx = 0; gx < 14; ++gx) {
            float cell = W / 14.0f;
            uint8_t v = (uint8_t)(40 + 20 * ((gx + gy) & 1));
            flux_canvas_fill_rect_color(canvas,
                                        (flux_rect){gx * cell, gy * (H / 8.0f), cell, H / 8.0f},
                                        flux_color_rgba_premul(v, v, v + 8, 255));
        }
    }

    /* Three solid rectangles with translucent SRC_OVER */
    flux_canvas_fill_rect_color(canvas, (flux_rect){80, 80, 280, 180},
                                flux_color_rgba_premul(240, 80, 80, 200));
    flux_canvas_fill_rect_color(canvas, (flux_rect){200, 160, 280, 180},
                                flux_color_rgba_premul(80, 200, 90, 200));
    flux_canvas_fill_rect_color(canvas, (flux_rect){320, 240, 280, 180},
                                flux_color_rgba_premul(80, 130, 240, 200));

    /* Linear gradient over a rect */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(255, 80, 120, 255)},
            {0.5f, flux_color_rgba_premul(255, 220, 80, 255)},
            {1.0f, flux_color_rgba_premul(60, 200, 255, 255)},
        };
        flux_brush g =
            flux_brush_linear_gradient((flux_point){640, 80}, (flux_point){920, 280}, stops, 3);
        flux_canvas_fill_rect(canvas, (flux_rect){640, 80, 280, 200}, &g);
    }

    /* Radial gradient over a circle path */
    {
        float rcx = W * 0.85f, rcy = H * 0.78f;
        flux_path *rcirc = nullptr;
        (void)flux_path_create(&rcirc, &app->arena);
        if (rcirc) {
            flux_path_add_circle(rcirc, rcx, rcy, 90.0f);
            flux_gradient_stop stops[3] = {
                {0.0f, flux_color_rgba_premul(255, 255, 255, 255)},
                {0.7f, flux_color_rgba_premul(180, 100, 255, 240)},
                {1.0f, flux_color_rgba_premul(30, 10, 80, 200)},
            };
            flux_brush g = flux_brush_radial_gradient((flux_point){rcx, rcy}, 90.0f, stops, 3);
            flux_canvas_fill_path(canvas, rcirc, FLUX_FILL_NON_ZERO, &g);
        }
    }

    /* Rounded rect via fill_path */
    flux_path *rrect = nullptr;
    (void)flux_path_create(&rrect, &app->arena);
    if (rrect) {
        flux_path_add_round_rect(rrect, (flux_rect){60, 360, 380, 140}, 32.0f);
        flux_brush p = flux_brush_solid(0xFF000000u);
        p.solid.color = flux_color_rgba_premul(255, 220, 120, 230);
        flux_canvas_fill_path(canvas, rrect, FLUX_FILL_NON_ZERO, &p);
    }

    /* Animated circle */
    flux_path *circ = nullptr;
    (void)flux_path_create(&circ, &app->arena);
    if (circ) {
        float cx = W * 0.75f + 40.0f * cosf(t);
        float cy = H * 0.55f + 40.0f * sinf(t * 1.3f);
        flux_path_add_circle(circ, cx, cy, 90.0f);
        flux_brush p = flux_brush_solid(0xFF000000u);
        p.solid.color = flux_color_rgba_premul(220, 140, 240, 220);
        flux_canvas_fill_path(canvas, circ, FLUX_FILL_NON_ZERO, &p);
    }

    /* Concave 5-point star */
    flux_path *star = nullptr;
    (void)flux_path_create(&star, &app->arena);
    if (star) {
        float cx = W * 0.5f, cy = H * 0.78f;
        float r_out = 64.0f, r_in = 26.0f;
        for (int k = 0; k < 10; ++k) {
            float a = (float)k * 3.14159265359f / 5.0f - 1.5707963f;
            float r = (k & 1) ? r_in : r_out;
            float px = cx + r * cosf(a);
            float py = cy + r * sinf(a);
            if (k == 0)
                flux_path_move_to(star, px, py);
            else
                flux_path_line_to(star, px, py);
        }
        flux_path_close(star);
        flux_brush p = flux_brush_solid(0xFF000000u);
        p.solid.color = flux_color_rgba_premul(120, 240, 180, 240);
        flux_canvas_fill_path(canvas, star, FLUX_FILL_NON_ZERO, &p);
    }

    /* Stroked sine wave */
    flux_path *wave = nullptr;
    (void)flux_path_create(&wave, &app->arena);
    if (wave) {
        float y0 = H * 0.65f;
        flux_path_move_to(wave, 40.0f, y0 + 30.0f * sinf(t * 2.0f));
        for (float x = 60.0f; x <= W - 40.0f; x += 20.0f) {
            flux_path_line_to(wave, x, y0 + 30.0f * sinf(x * 0.02f + t * 2.0f));
        }
        flux_stroke_style st = {.width = 5.0f, .cap = FLUX_CAP_ROUND, .join = FLUX_JOIN_ROUND};
        flux_brush b = flux_brush_solid(flux_color_rgba_premul(255, 255, 255, 220));
        flux_canvas_stroke_path(canvas, wave, &st, &b);
    }

    /* Sampled checkerboard texture */
    if (app->checker) {
        flux_rect dst = {W - 180.0f, 40.0f, 140.0f, 140.0f};
        flux_canvas_draw_image(canvas, app->checker, dst, nullptr);
    }

    flux_canvas_restore(canvas);
    flux_arena_reset(&app->arena);
}

int main(void) {
    canvas_hello_app app = {0};
    printf("flux canvas_hello — native Iris window (Esc to quit)\n");
    return iris_app_run(&(iris_app_opts){
        .title = "flux canvas — hello",
        .app_id = "org.optics.flux.canvas-hello",
        .width = 960,
        .height = 540,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .paint = on_paint,
        .user = &app,
    });
}
