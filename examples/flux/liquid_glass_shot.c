/*
 * liquid_glass_shot — headless offscreen snapshot of the liquid-glass
 * effect using REAL backdrop capture (ADR-0017). Renders the chaotic
 * scene into a capture target, blurs it, composites, and writes the
 * frame to liquid_glass_shot.ppm. No window/display needed.
 *
 * Mirrors examples/liquid_glass.c but on an offscreen surface.
 */
#include <flux/effect.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SHOT_W 960
#define SHOT_H 600

static void draw_chaos(flux_canvas *c, flux_arena *arena, float W, float H, flux_image *noise_img) {
    /* Base warm gradient wash. */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(235, 205, 175, 255)}, // top-leftish warm beige
            {1.0f, flux_color_rgba_premul(110, 60, 100, 255)},  // bottom-rightish dark magenta
        };
        flux_paint g = flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){W, H}, stops, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &g);
    }

    /* Top right orange/red radial */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(210, 80, 50, 200)},
            {1.0f, flux_color_rgba_premul(210, 80, 50, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.85f, H * 0.15f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr;
        (void)flux_path_create(&p, arena);
        if (p) {
            flux_path_add_rect(p, (flux_rect){0, 0, W, H});
            flux_canvas_fill_path(c, p, &g);
        }
    }

    /* Bottom left purple/blue radial */
    {
        flux_gradient_stop stops[2] = {
            {0.0f, flux_color_rgba_premul(80, 70, 140, 200)},
            {1.0f, flux_color_rgba_premul(80, 70, 140, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.15f, H * 0.85f}, W * 0.6f, stops, 2);
        flux_path *p = nullptr;
        (void)flux_path_create(&p, arena);
        if (p) {
            flux_path_add_rect(p, (flux_rect){0, 0, W, H});
            flux_canvas_fill_path(c, p, &g);
        }
    }

    /* Noise overlay */
    if (noise_img) {
        for (float y = 0; y < H; y += 256.0f) {
            for (float x = 0; x < W; x += 256.0f) {
                flux_canvas_draw_image(c, noise_img, (flux_rect){x, y, 256.0f, 256.0f}, nullptr);
            }
        }
    }
}

int main(void) {
    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .headless = true,
        .frames_in_flight = 1,
        .validation = FLUX_VALIDATION_OFF,
    };
    flux_device *device = nullptr;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        fprintf(stderr, "device create failed\n");
        return 1;
    }

    flux_surface_desc sdesc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .width = SHOT_W,
        .height = SHOT_H,
    };
    flux_surface *surface = nullptr;
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        fprintf(stderr, "offscreen surface create failed\n");
        flux_device_release(device);
        return 1;
    }

    flux_canvas_desc cdesc = {.type = FLUX_TYPE_CANVAS_DESC, .surface = surface};
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create(&cdesc, &canvas) != FLUX_OK) {
        fprintf(stderr, "canvas create failed\n");
        flux_surface_release(surface);
        flux_device_release(device);
        return 1;
    }

    flux_arena arena;
    if (flux_arena_init(&arena, 128 * 1024, nullptr) != FLUX_OK)
        return 1;

    VkFormat sfmt = flux_surface_vk_format(surface);
    flux_format target_fmt =
        (sfmt == VK_FORMAT_B8G8R8A8_UNORM) ? FLUX_FORMAT_BGRA8_UNORM : FLUX_FORMAT_RGBA8_UNORM;
    flux_image *capture = nullptr;
    if (flux_image_create_render_target(device, SHOT_W, SHOT_H, target_fmt, &capture) != FLUX_OK) {
        {
            flux_error_info ei;
            flux_get_last_error(&ei);
            fprintf(stderr, "render-target create failed: %s\n", ei.message ? ei.message : "?");
        }
        return 1;
    }
    /* Create static noise texture for grain. */
    flux_image *noise_img = nullptr;
    {
        uint8_t *noise_data = (uint8_t *)malloc(256 * 256 * 4);
        if (noise_data) {
            for (int i = 0; i < 256 * 256 * 4; i += 4) {
                uint8_t v = rand() % 256;
                uint8_t a = 10 + (rand() % 15);
                noise_data[i] = (uint8_t)((v * a) / 255);
                noise_data[i + 1] = (uint8_t)((v * a) / 255);
                noise_data[i + 2] = (uint8_t)((v * a) / 255);
                noise_data[i + 3] = a;
            }
            flux_image_desc ndesc = FLUX_IMAGE_DESC_INIT;
            ndesc.width = 256;
            ndesc.height = 256;
            ndesc.format = FLUX_FORMAT_RGBA8_UNORM;
            ndesc.initial_data = noise_data;
            (void)flux_image_create(device, &ndesc, &noise_img);
            free(noise_data);
        }
    }

    float W = (float)SHOT_W, H = (float)SHOT_H, t = 1.3f;

    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(surface, nullptr, &frame);
    if (r != FLUX_OK) {
        fprintf(stderr, "begin_frame: %s\n", flux_result_string(r));
        return 1;
    }

    /* CAPTURE */
    {
        flux_color clear = flux_color_rgba(18, 14, 28, 255);
        r = flux_canvas_begin_target(canvas, frame, capture, &clear);
        if (r != FLUX_OK) {
            fprintf(stderr, "begin_target: %s\n", flux_result_string(r));
            return 1;
        }
        draw_chaos(canvas, &arena, W, H, noise_img);
        flux_arena_reset(&arena);
        flux_canvas_end_target(canvas);
    }

    /* BLUR */
    flux_image *blurred = nullptr;
    flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
    bd.input = capture;
    /* Keep within the tested regime: sigma 6 (radius 18). The earlier
     * sigma=18 (radius 54) at 960x600 made a single frame exceed the
     * 2 s frame fence on modest / software Vulkan hosts, which starves
     * the display compositor and freezes the screen. */
    bd.sigma = 6.0f;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
    r = flux_effect_blur(cmd, &bd, &blurred);
    if (r != FLUX_OK) {
        fprintf(stderr, "blur: %s\n", flux_result_string(r));
        return 1;
    }

    /* COMPOSITE */
    flux_color clear = flux_color_rgba(10, 10, 14, 255);
    r = flux_canvas_begin(canvas, frame, &clear);
    if (r != FLUX_OK) {
        fprintf(stderr, "canvas_begin: %s\n", flux_result_string(r));
        return 1;
    }

    flux_canvas_draw_image(canvas, capture, (flux_rect){0, 0, W, H}, nullptr);

    float gw = fminf(W, H) * 0.46f;
    float gh = gw * 0.62f;
    float gx = W * 0.5f - gw * 0.5f + 70.0f * cosf(t * 0.4f);
    float gy = H * 0.5f - gh * 0.5f + 40.0f * sinf(t * 0.6f);
    float gr = gh * 0.30f;
    flux_rect glass = {gx, gy, gw, gh};
    float cx = gx + gw * 0.5f, cy = gy + gh * 0.5f;

    /* shadow */
    {
        flux_path *sh = nullptr;
        (void)flux_path_create(&sh, &arena);
        if (sh) {
            flux_path_add_round_rect(sh, (flux_rect){gx + 6, gy + 12, gw, gh}, gr);
            flux_paint p = flux_paint_default();
            p.color = flux_color_rgba_premul(0, 0, 0, 90);
            flux_canvas_fill_path(canvas, sh, &p);
        }
    }
    /* blurred backdrop scoped to glass */
    flux_canvas_save(canvas);
    flux_canvas_clip_rect(canvas, glass);
    flux_canvas_draw_image(canvas, blurred, (flux_rect){0, 0, W, H}, nullptr);
    flux_canvas_restore(canvas);

    flux_path *shape = nullptr;
    (void)flux_path_create(&shape, &arena);
    if (shape)
        flux_path_add_round_rect(shape, glass, gr);

    if (shape) {
        flux_gradient_stop stops[4] = {
            {0.00f, flux_color_rgba_premul(115, 130, 150, 205)},
            {0.50f, flux_color_rgba_premul(95, 110, 132, 175)},
            {0.85f, flux_color_rgba_premul(78, 92, 116, 130)},
            {1.00f, flux_color_rgba_premul(60, 74, 98, 80)},
        };
        flux_paint vol =
            flux_paint_radial_gradient((flux_point){cx, cy}, fmaxf(gw, gh) * 0.62f, stops, 4);
        flux_canvas_fill_path(canvas, shape, &vol);
    }
    if (shape) {
        flux_gradient_stop stops[4] = {
            {0.50f, flux_color_rgba_premul(0, 0, 0, 0)},
            {0.78f, flux_color_rgba_premul(255, 255, 255, 40)},
            {0.93f, flux_color_rgba_premul(255, 255, 255, 160)},
            {1.00f, flux_color_rgba_premul(255, 255, 255, 235)},
        };
        flux_paint fres =
            flux_paint_radial_gradient((flux_point){cx, cy}, fmaxf(gw, gh) * 0.55f, stops, 4);
        flux_canvas_fill_path(canvas, shape, &fres);
    }
    if (shape) {
        flux_gradient_stop stops[3] = {
            {0.00f, flux_color_rgba_premul(255, 255, 255, 110)},
            {0.45f, flux_color_rgba_premul(255, 255, 255, 30)},
            {1.00f, flux_color_rgba_premul(0, 0, 0, 0)},
        };
        flux_paint sheen = flux_paint_linear_gradient((flux_point){gx, gy},
                                                      (flux_point){gx, gy + gh * 0.55f}, stops, 3);
        flux_canvas_fill_path(canvas, shape, &sheen);
    }
    {
        flux_path *rim = nullptr;
        (void)flux_path_create(&rim, &arena);
        if (rim) {
            flux_path_add_round_rect(rim, (flux_rect){gx + 1, gy + 1, gw - 2, gh - 2}, gr - 1);
            flux_paint sp = flux_paint_default();
            sp.color = flux_color_rgba_premul(255, 235, 210, 55);
            sp.stroke_width = 1.0f;
            sp.join = FLUX_JOIN_ROUND;
            sp.cap = FLUX_CAP_ROUND;
            flux_canvas_stroke_path(canvas, rim, &sp);
        }
        flux_path *hair = nullptr;
        (void)flux_path_create(&hair, &arena);
        if (hair) {
            flux_path_add_round_rect(hair, glass, gr);
            flux_paint sp = flux_paint_default();
            sp.color = flux_color_rgba_premul(255, 255, 255, 150);
            sp.stroke_width = 1.0f;
            sp.join = FLUX_JOIN_ROUND;
            flux_canvas_stroke_path(canvas, hair, &sp);
        }
    }

    flux_arena_reset(&arena);
    flux_canvas_end(canvas);

    int rc = 0;
    r = flux_frame_submit(frame);
    if (r != FLUX_OK) {
        fprintf(stderr, "submit failed\n");
        rc = 1;
        goto fail;
    }
    r = flux_frame_present(frame);
    if (r != FLUX_OK) {
        fprintf(stderr, "present failed: %s\n", flux_result_string(r));
        rc = 1;
        goto fail;
    }

    /* read back + write PPM */
    size_t bytes = (size_t)SHOT_W * SHOT_H * 4;
    uint8_t *px = malloc(bytes);
    if (!px) {
        rc = 1;
        goto fail;
    }
    r = flux_surface_read_pixels(surface, px, bytes);
    if (r != FLUX_OK) {
        /* A timeout leaves the frame's command buffer still running on
         * the device. Drain it below before tearing anything down, or
         * the abandoned work keeps the (possibly shared / software)
         * device pinned and freezes the display compositor. */
        fprintf(stderr, "read_pixels failed: %s\n", flux_result_string(r));
        free(px);
        rc = 1;
        goto fail;
    }

    FILE *fp = fopen("liquid_glass_shot.ppm", "wb");
    if (!fp) {
        perror("fopen");
        free(px);
        rc = 1;
        goto fail;
    }
    fprintf(fp, "P6\n%d %d\n255\n", SHOT_W, SHOT_H);
    for (size_t i = 0; i < (size_t)SHOT_W * SHOT_H; ++i) {
        uint8_t a = px[i * 4 + 3];
        uint8_t rgb[3];
        rgb[0] = a ? (uint8_t)(px[i * 4 + 0] * 255u / a) : 0;
        rgb[1] = a ? (uint8_t)(px[i * 4 + 1] * 255u / a) : 0;
        rgb[2] = a ? (uint8_t)(px[i * 4 + 2] * 255u / a) : 0;
        fwrite(rgb, 1, 3, fp);
    }
    fclose(fp);
    printf("wrote liquid_glass_shot.ppm (%dx%d)\n", SHOT_W, SHOT_H);
    free(px);

fail:
    flux_device_wait_idle(device);
    flux_effect_reset(device);
    flux_arena_destroy(&arena);
    flux_image_release(capture);
    flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    flux_device_release(device);
    return rc;
}
