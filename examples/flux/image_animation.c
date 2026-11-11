/*
 * image_animation — 2D image transforms and sprite sheet animation (ADR-0097).
 *
 * Demonstrates:
 *   - Drawing images through flux_canvas with affine transforms;
 *   - Sub-rect sampling from a packed sprite strip;
 *   - Native Iris windowing with fractional HiDPI and system cursor.
 */

#include <flux/canvas.h>
#include <flux/flux.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CARD_SIZE = 96,
    SPRITE_SIZE = 48,
    SPRITE_FRAMES = 6,
};

typedef struct image_animation_app {
    flux_image *card_a;
    flux_image *card_b;
    flux_image *sprites;
    float time;
} image_animation_app;

static uint32_t pack_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static void make_card_pixels(uint32_t *pixels, int variant) {
    for (int y = 0; y < CARD_SIZE; ++y) {
        for (int x = 0; x < CARD_SIZE; ++x) {
            float u = (float)x / (float)(CARD_SIZE - 1);
            float v = (float)y / (float)(CARD_SIZE - 1);
            float dx = u - 0.5f;
            float dy = v - 0.5f;
            float r = sqrtf(dx * dx + dy * dy);

            bool border = (x < 3 || x >= CARD_SIZE - 3 || y < 3 || y >= CARD_SIZE - 3);
            uint8_t red = (uint8_t)(variant == 0 ? 40 + 180 * u : 210 - 130 * v);
            uint8_t green = (uint8_t)(variant == 0 ? 80 + 140 * v : 80 + 140 * u);
            uint8_t blue = (uint8_t)(variant == 0 ? 220 - 120 * u : 70 + 170 * (1.0f - r));

            if (r < 0.28f) {
                red = (uint8_t)(255 - red / 2);
                green = (uint8_t)(255 - green / 2);
                blue = (uint8_t)(255 - blue / 2);
            }
            if (border) {
                red = 255;
                green = 255;
                blue = 255;
            }
            pixels[y * CARD_SIZE + x] = pack_rgba(red, green, blue, 255);
        }
    }
}

static void make_sprite_pixels(uint32_t *pixels) {
    uint32_t cell[SPRITE_SIZE * SPRITE_SIZE];
    for (int frame = 0; frame < SPRITE_FRAMES; ++frame) {
        float phase = (float)frame / (float)SPRITE_FRAMES * 6.2831853f;
        for (int y = 0; y < SPRITE_SIZE; ++y) {
            for (int x = 0; x < SPRITE_SIZE; ++x) {
                float cx = (float)x - (float)SPRITE_SIZE * 0.5f;
                float cy = (float)y - (float)SPRITE_SIZE * 0.5f;
                float r = sqrtf(cx * cx + cy * cy);
                float a = atan2f(cy, cx) + phase;
                float petal = 0.5f + 0.5f * cosf(4.0f * a);
                float radius = 10.0f + 8.0f * petal;

                if (r <= radius) {
                    uint8_t red = (uint8_t)(240 - 20 * frame);
                    uint8_t green = (uint8_t)(90 + 25 * frame);
                    uint8_t blue = (uint8_t)(120 + 20 * frame);
                    cell[y * SPRITE_SIZE + x] = pack_rgba(red, green, blue, 255);
                } else if (r <= radius + 2.0f) {
                    cell[y * SPRITE_SIZE + x] = pack_rgba(255, 255, 255, 220);
                } else {
                    cell[y * SPRITE_SIZE + x] = 0;
                }
            }
        }
        for (int y = 0; y < SPRITE_SIZE; ++y) {
            for (int x = 0; x < SPRITE_SIZE; ++x)
                pixels[y * (SPRITE_FRAMES * SPRITE_SIZE) + frame * SPRITE_SIZE + x] =
                    cell[y * SPRITE_SIZE + x];
        }
    }
}

static flux_image *make_image(flux_device *device, uint32_t width, uint32_t height,
                              const uint32_t *pixels) {
    flux_image_desc desc = FLUX_IMAGE_DESC_INIT;
    desc.width = width;
    desc.height = height;
    desc.format = FLUX_FORMAT_RGBA8_UNORM;
    desc.initial_data = pixels;
    flux_image *image = NULL;
    return flux_image_create(device, &desc, &image) == FLUX_OK ? image : NULL;
}

static void record_image(flux_encoder *enc, flux_image *image, flux_rect dst, flux_rect src,
                         float opacity) {
    flux_geometry geom = flux_geom_rect(dst);
    flux_brush brush = {
        .kind = FLUX_BRUSH_IMAGE_PATTERN,
        .blend = FLUX_BLEND_SRC_OVER,
        .opacity = opacity,
        .image = {.image = image,
                  .src_rect = src,
                  .tint = flux_color_rgba_premul(255, 255, 255, 255)},
    };
    flux_encoder_draw_geometry(enc, &geom, &brush);
}

static void draw_panel(flux_encoder *enc, flux_rect panel) {
    flux_geometry fill = flux_geom_rrect(panel, 22.0f);
    flux_brush fill_brush = flux_brush_solid(flux_color_rgba_premul(27, 31, 46, 245));
    flux_encoder_draw_geometry(enc, &fill, &fill_brush);

    flux_geometry stroke = flux_geom_rrect(panel, 22.0f);
    stroke.stroke.width = 1.5f;
    flux_brush stroke_brush = flux_brush_solid(flux_color_rgba_premul(80, 92, 125, 160));
    flux_encoder_draw_geometry(enc, &stroke, &stroke_brush);
}

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    image_animation_app *app = user;

    uint32_t card_a_pixels[CARD_SIZE * CARD_SIZE];
    uint32_t card_b_pixels[CARD_SIZE * CARD_SIZE];
    uint32_t sprite_pixels[SPRITE_FRAMES * SPRITE_SIZE * SPRITE_SIZE];
    make_card_pixels(card_a_pixels, 0);
    make_card_pixels(card_b_pixels, 1);
    make_sprite_pixels(sprite_pixels);

    app->card_a = make_image(device, CARD_SIZE, CARD_SIZE, card_a_pixels);
    app->card_b = make_image(device, CARD_SIZE, CARD_SIZE, card_b_pixels);
    app->sprites = make_image(device, SPRITE_FRAMES * SPRITE_SIZE, SPRITE_SIZE, sprite_pixels);
    if (!app->card_a || !app->card_b || !app->sprites) {
        fprintf(stderr, "could not create animation textures\n");
        return false;
    }
    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    image_animation_app *app = user;
    if (app->sprites)
        flux_image_release(app->sprites);
    if (app->card_b)
        flux_image_release(app->card_b);
    if (app->card_a)
        flux_image_release(app->card_a);
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

static void on_record(flux_encoder *enc, float scale, void *user) {
    image_animation_app *app = user;
    app->time += 0.016f;
    float time = app->time;

    int32_t log_w = 1100, log_h = 720;
    iris_window_get_geometry(&log_w, &log_h);
    float width = (float)log_w;
    float height = (float)log_h;

    flux_encoder_save(enc);
    flux_encoder_scale(enc, scale, scale);

    float margin = 24.0f;
    float gap = 18.0f;
    float panel_w = (width - margin * 2.0f - gap) * 0.5f;
    float panel_h = (height - margin * 2.0f - gap) * 0.5f;
    flux_rect panels[4] = {
        {margin, margin, panel_w, panel_h},
        {margin + panel_w + gap, margin, panel_w, panel_h},
        {margin, margin + panel_h + gap, panel_w, panel_h},
        {margin + panel_w + gap, margin + panel_h + gap, panel_w, panel_h},
    };
    for (int i = 0; i < 4; ++i)
        draw_panel(enc, panels[i]);

    /* Panel 0: Orbit & rotate */
    {
        flux_rect p = panels[0];
        float cx = p.x + p.w * 0.5f;
        float cy = p.y + p.h * 0.5f;
        float angle = time * 1.2f;
        float orbit = 40.0f;

        flux_encoder_save(enc);
        flux_encoder_translate(enc, cx + cosf(time * 0.8f) * orbit, cy + sinf(time * 0.8f) * orbit);
        flux_encoder_rotate(enc, angle);
        record_image(enc, app->card_a,
                     (flux_rect){-CARD_SIZE * 0.5f, -CARD_SIZE * 0.5f, CARD_SIZE, CARD_SIZE},
                     (flux_rect){0}, 1.0f);
        flux_encoder_restore(enc);
    }

    /* Panel 1: Sprite strip animation */
    {
        flux_rect p = panels[1];
        float cx = p.x + p.w * 0.5f;
        float cy = p.y + p.h * 0.5f;
        int frame = (int)(time * 8.0f) % SPRITE_FRAMES;
        flux_rect src = {(float)frame / (float)SPRITE_FRAMES, 0.0f, 1.0f / (float)SPRITE_FRAMES,
                         1.0f};
        float dest_size = 140.0f;
        record_image(
            enc, app->sprites,
            (flux_rect){cx - dest_size * 0.5f, cy - dest_size * 0.5f, dest_size, dest_size}, src,
            1.0f);
    }

    /* Panel 2: Pulse & Crossfade */
    {
        flux_rect p = panels[2];
        float cx = p.x + p.w * 0.5f;
        float cy = p.y + p.h * 0.5f;
        float s = 1.0f + 0.35f * sinf(time * 2.5f);
        float fade = 0.5f + 0.5f * sinf(time * 1.5f);

        flux_encoder_save(enc);
        flux_encoder_translate(enc, cx, cy);
        flux_encoder_scale(enc, s, s);

        flux_rect dst = {-CARD_SIZE * 0.5f, -CARD_SIZE * 0.5f, CARD_SIZE, CARD_SIZE};
        record_image(enc, app->card_a, dst, (flux_rect){0}, 1.0f - fade);
        record_image(enc, app->card_b, dst, (flux_rect){0}, fade);
        flux_encoder_restore(enc);
    }

    /* Panel 3: Shear & Wobble */
    {
        flux_rect p = panels[3];
        float cx = p.x + p.w * 0.5f;
        float cy = p.y + p.h * 0.5f;
        float shx = sinf(time * 2.0f) * 0.45f;
        float shy = cosf(time * 1.7f) * 0.25f;

        flux_encoder_save(enc);
        flux_encoder_translate(enc, cx, cy);
        flux_mat3x2 skew_m = {.m = {1.0f, shy, shx, 1.0f, 0.0f, 0.0f}};
        flux_encoder_transform(enc, skew_m);
        record_image(enc, app->card_b,
                     (flux_rect){-CARD_SIZE * 0.5f, -CARD_SIZE * 0.5f, CARD_SIZE, CARD_SIZE},
                     (flux_rect){0}, 1.0f);
        flux_encoder_restore(enc);
    }

    flux_encoder_restore(enc);
}

int main(void) {
    image_animation_app app = {0};
    printf("flux image_animation — native Iris window (Esc to quit)\n");
    return iris_app_run(&(iris_app_opts){
        .title = "flux image animation",
        .app_id = "org.optics.flux.image-animation",
        .width = 1100,
        .height = 720,
        .dark = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .record = on_record,
        .user = &app,
    });
}
