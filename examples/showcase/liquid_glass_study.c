/*
 * liquid_glass_study — headless A/B harness for the analytic liquid-glass
 * effect. Renders a deliberately hostile backdrop (fine stripes, text-like
 * rows, saturated blobs, bright/dark zones), composites liquid-glass bodies
 * over it through the real capture → blur → glass pipeline, reads the frame
 * back and writes a binary PPM for offline inspection.
 *
 * No window or GLFW required: uses a headless device + offscreen surface
 * (ADR-0013). Convert the result with:
 *
 *   ffmpeg -y -i glass_study.ppm glass_study.png
 *
 * Usage: liquid_glass_study [out.ppm] [refraction edge_width rim_light
 *          saturation brightness chromatic]  — trailing floats override the
 *        study defaults when present.
 */
#include <flux/effect.h>
#include <flux/flux.h>
#include <prism/prism.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define W 1280u
#define H 800u

static void fill(flux_canvas *c, float x, float y, float w, float h, uint8_t r, uint8_t g,
                 uint8_t b) {
    flux_canvas_fill_rect_color(c, (flux_rect){x, y, w, h}, flux_color_rgba(r, g, b, 255));
}

/* Hostile backdrop: saturated photo-like blobs, sub-pixel stripes, small
 * text rows and a hard bright/dark split, all crossed by the glass bodies. */
static void draw_study_scene(flux_canvas *c) {
    /* Base vertical wash: deep indigo -> teal -> warm sand. */
    {
        flux_gradient_stop stops[3] = {
            {0.0f, flux_color_rgba_premul(34, 38, 84, 255)},
            {0.55f, flux_color_rgba_premul(28, 110, 112, 255)},
            {1.0f, flux_color_rgba_premul(204, 142, 92, 255)},
        };
        flux_paint g = flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){0, H}, stops, 3);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &g);
    }
    /* Saturated photo blobs. */
    {
        flux_gradient_stop red[2] = {
            {0.0f, flux_color_rgba_premul(226, 64, 44, 235)},
            {1.0f, flux_color_rgba_premul(226, 64, 44, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.16f, H * 0.28f}, 330.0f, red, 2);
        flux_canvas_fill_rect(c, (flux_rect){0, 0, W * 0.5f, H * 0.7f}, &g);
    }
    {
        flux_gradient_stop gold[2] = {
            {0.0f, flux_color_rgba_premul(246, 196, 52, 220)},
            {1.0f, flux_color_rgba_premul(246, 196, 52, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.46f, H * 0.60f}, 300.0f, gold, 2);
        flux_canvas_fill_rect(c, (flux_rect){W * 0.2f, H * 0.3f, W * 0.55f, H * 0.7f}, &g);
    }
    {
        flux_gradient_stop azure[2] = {
            {0.0f, flux_color_rgba_premul(52, 120, 235, 225)},
            {1.0f, flux_color_rgba_premul(52, 120, 235, 0)},
        };
        flux_paint g =
            flux_paint_radial_gradient((flux_point){W * 0.86f, H * 0.24f}, 360.0f, azure, 2);
        flux_canvas_fill_rect(c, (flux_rect){W * 0.55f, 0, W * 0.45f, H * 0.6f}, &g);
    }

    /* "Newspaper": white panel with thin dark text rows (legibility). */
    fill(c, 500.0f, 60.0f, 330.0f, 420.0f, 244, 244, 240);
    for (float y = 92.0f; y < 460.0f; y += 26.0f) {
        float w = 300.0f - fmodf(y * 7.0f, 90.0f);
        fill(c, 518.0f, y, w, 7.0f, 38, 38, 42);
        fill(c, 518.0f, y + 12.0f, w * 0.72f, 5.0f, 96, 96, 100);
    }
    /* Headline block. */
    fill(c, 518.0f, 74.0f, 220.0f, 12.0f, 18, 18, 22);

    /* Fine stripe field: 2px on / 2px off verticals (resolution probe). */
    for (float x = 40.0f; x < 460.0f; x += 4.0f)
        fill(c, x, 360.0f, 2.0f, 240.0f, 250, 250, 252);
    /* Checkerboard 6px (refraction displacement probe). */
    for (int gy = 0; gy < 8; ++gy)
        for (int gx = 0; gx < 16; ++gx)
            if ((gx + gy) & 1)
                fill(c, 60.0f + gx * 6.0f, 620.0f + gy * 6.0f, 6.0f, 6.0f, 250, 250, 250);

    /* Dark zone with bright sparks (dark-adaptation probe). */
    fill(c, 900.0f, 60.0f, 340.0f, 420.0f, 16, 18, 26);
    for (int i = 0; i < 26; ++i) {
        float sx = 916.0f + (float)((i * 53) % 300);
        float sy = 76.0f + (float)((i * 97) % 380);
        fill(c, sx, sy, 10.0f, 10.0f, 252, 244, 220);
    }
    /* Pure white field: the uniform-bright separation probe the dock pill
     * must survive. */
    fill(c, 820.0f, 430.0f, 460.0f, 370.0f, 252, 252, 254);
    /* Bright/dark hard split running under the dock band. */
    fill(c, 0.0f, 520.0f, (float)W, 4.0f, 250, 250, 250);
}

int main(int argc, char **argv) {
    const char *out_path = argc > 1 ? argv[1] : "glass_study.ppm";
    /* r, ew, rim_light, sat, bri, ca, size_ref */
    float opt[7] = {12.0f, 20.0f, 0.55f, 1.08f, 1.0f, 1.5f, 72.0f};
    for (int i = 2; i < argc && i - 2 < 7; ++i)
        opt[i - 2] = (float)atof(argv[i]);

    flux_device_desc ddesc = FLUX_DEVICE_DESC_INIT;
    ddesc.headless = true;
    ddesc.frames_in_flight = 1;
    flux_device *device = nullptr;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        fprintf(stderr, "no headless Vulkan device\n");
        return 1;
    }

    flux_surface_readback_desc rb = FLUX_SURFACE_READBACK_DESC_INIT;
    rb.require_readback = true;
    flux_surface_desc sdesc = FLUX_SURFACE_DESC_INIT;
    sdesc.next = &rb;
    sdesc.width = W;
    sdesc.height = H;
    flux_surface *surface = nullptr;
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        fprintf(stderr, "offscreen surface failed\n");
        return 1;
    }
    flux_canvas_desc cdesc = FLUX_CANVAS_DESC_INIT;
    cdesc.surface = surface;
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create(&cdesc, &canvas) != FLUX_OK) {
        fprintf(stderr, "canvas failed\n");
        return 1;
    }
    flux_arena arena;
    if (flux_arena_init(&arena, 64 * 1024, nullptr) != FLUX_OK)
        return 1;

    flux_image *capture = nullptr;
    if (flux_image_create_render_target(device, W, H, FLUX_FORMAT_RGBA8_UNORM, &capture) !=
        FLUX_OK) {
        fprintf(stderr, "capture target failed\n");
        return 1;
    }
    flux_blur_filter *blur = nullptr;
    prism_liquid_glass_filter *glass = nullptr;
    if (flux_blur_filter_create(device, &blur) != FLUX_OK ||
        prism_liquid_glass_filter_create(device, &glass) != FLUX_OK)
        return 1;

    flux_frame *frame = nullptr;
    if (flux_surface_begin_frame(surface, nullptr, &frame) != FLUX_OK)
        return 1;

    /* 1. capture the scene */
    flux_color clear = flux_color_rgba(12, 12, 18, 255);
    if (flux_canvas_begin_target(canvas, frame, capture, &clear) != FLUX_OK)
        return 1;
    draw_study_scene(canvas);
    flux_canvas_end_target(canvas);

    /* 2. frost + glass */
    flux_image *blurred = nullptr;
    flux_effect_blur_desc bd = FLUX_EFFECT_BLUR_DESC_INIT;
    bd.input = capture;
    bd.sigma = 14.0f;
    if (flux_blur_filter_apply(blur, frame, &bd, &blurred) != FLUX_OK)
        return 1;

    const flux_rect pill = {60.0f, 560.0f, 1160.0f, 90.0f};
    const flux_rect panel = {430.0f, 140.0f, 420.0f, 260.0f};
    prism_liquid_glass_group groups[5] = {
        PRISM_LIQUID_GLASS_GROUP_INIT, PRISM_LIQUID_GLASS_GROUP_INIT, PRISM_LIQUID_GLASS_GROUP_INIT,
        PRISM_LIQUID_GLASS_GROUP_INIT, PRISM_LIQUID_GLASS_GROUP_INIT,
    };
    groups[0].shapes[0] = (prism_liquid_glass_shape){.bounds = pill, .corner_radius = 45.0f};
    groups[0].shadow_alpha = 0.20f;
    groups[0].shadow_blur = 12.0f;
    groups[0].shadow_offset_y = 6.0f;

    groups[1].shapes[0] = (prism_liquid_glass_shape){.bounds = panel, .corner_radius = 32.0f};
    groups[1].shadow_alpha = 0.22f;
    groups[1].shadow_blur = 14.0f;
    groups[1].shadow_offset_y = 7.0f;
    /* One selected region changes the parent body's optics; it is not
     * another glass body and therefore casts no nested rim or shadow. */
    groups[1].focus = (prism_liquid_glass_shape){.bounds = {474.0f, 184.0f, 212.0f, 156.0f},
                                                 .corner_radius = 18.0f};
    groups[1].focus_strength = 1.0f;

    groups[2].shapes[0] = (prism_liquid_glass_shape){.bounds = {120.0f, 200.0f, 96.0f, 96.0f},
                                                     .corner_radius = 48.0f};
    groups[2].shapes[1] = (prism_liquid_glass_shape){.bounds = {252.0f, 224.0f, 72.0f, 72.0f},
                                                     .corner_radius = 36.0f};
    groups[2].shape_count = 2;
    groups[2].blend_radius = 28.0f;
    groups[2].shadow_alpha = 0.20f;
    groups[2].shadow_blur = 10.0f;
    groups[2].shadow_offset_y = 5.0f;
    /* Cool accent tint: per-body tinting must read through the glass. */
    groups[2].tint_color = 0xC8E0FFu;

    /* HUD-chip scale with its own component shadow. */
    groups[3].shapes[0] = (prism_liquid_glass_shape){.bounds = {880.0f, 500.0f, 240.0f, 32.0f},
                                                     .corner_radius = 16.0f};
    groups[3].shadow_alpha = 0.16f;
    groups[3].shadow_blur = 4.0f;
    groups[3].shadow_offset_y = 2.0f;

    /* Dock-handle scale: a 6 px stadium indicator still casts a shadow. */
    groups[4].shapes[0] =
        (prism_liquid_glass_shape){.bounds = {560.0f, 700.0f, 140.0f, 6.0f}, .corner_radius = 3.0f};
    groups[4].shadow_alpha = 0.20f;
    groups[4].shadow_blur = 4.2f;
    groups[4].shadow_offset_y = 2.1f;
    prism_liquid_glass_desc gd = PRISM_LIQUID_GLASS_DESC_INIT;
    gd.input = capture;
    gd.blurred_input = blurred;
    gd.groups = groups;
    gd.group_count = 5;
    gd.refraction = opt[0];
    gd.edge_width = opt[1];
    gd.rim_light = opt[2];
    gd.saturation = opt[3];
    gd.brightness = opt[4];
    gd.chromatic_aberration = opt[5];
    gd.size_reference = opt[6];
    flux_image *glass_out = nullptr;
    if (prism_liquid_glass_filter_apply(glass, frame, &gd, &glass_out) != FLUX_OK)
        return 1;

    /* 3. composite */
    if (flux_canvas_begin(canvas, frame, &clear) != FLUX_OK)
        return 1;
    flux_canvas_draw_image(canvas, capture, (flux_rect){0, 0, W, H}, nullptr);
    flux_canvas_draw_image(canvas, glass_out, (flux_rect){0, 0, W, H}, nullptr);
    /* The dock's painted foreground layer over the collapsed handle:
     * white at 64/255, exactly as collapsing_dock_material draws it. */
    {
        flux_path *hp = nullptr;
        (void)flux_path_create(&hp, &arena);
        if (hp) {
            flux_path_add_round_rect(hp, (flux_rect){560.0f, 700.0f, 140.0f, 6.0f}, 3.0f);
            flux_paint paint = flux_paint_default();
            paint.color = flux_color_rgba_premul(255, 255, 255, 64);
            flux_canvas_fill_path(canvas, hp, &paint);
        }
    }
    flux_arena_reset(&arena);
    flux_canvas_end(canvas);

    if (flux_frame_request_readback(frame) != FLUX_OK)
        return 1;
    if (flux_frame_submit(frame) != FLUX_OK || flux_frame_present(frame) != FLUX_OK)
        return 1;

    static uint8_t px[W * H * 4u];
    if (flux_surface_read_pixels(surface, px, sizeof(px)) != FLUX_OK) {
        fprintf(stderr, "readback failed\n");
        return 1;
    }
    FILE *f = fopen(out_path, "wb");
    if (!f)
        return 1;
    fprintf(f, "P6\n%u %u\n255\n", W, H);
    for (uint32_t i = 0; i < W * H; ++i)
        fwrite(px + i * 4, 1, 3, f); /* drop alpha */
    fclose(f);
    printf("wrote %s (refraction=%.2f edge_width=%.2f rim_light=%.2f saturation=%.3f "
           "brightness=%.3f chromatic=%.2f)\n",
           out_path, opt[0], opt[1], opt[2], opt[3], opt[4], opt[5]);

    flux_device_wait_idle(device);
    prism_liquid_glass_filter_release(glass);
    flux_blur_filter_release(blur);
    flux_image_release(capture);
    flux_arena_destroy(&arena);
    flux_canvas_destroy(canvas);
    flux_surface_release(surface);
    flux_device_release(device);
    return 0;
}
