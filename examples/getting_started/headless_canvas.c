/*
 * headless_canvas.c — GPU-free software 2D vector rendering to PPM (ADR-0171).
 *
 * Uses <optics/gfx/canvas_cpu.h> to render analytic anti-aliased geometry on the
 * CPU without creating a Vulkan device, window, or display connection.
 *
 * Usage: headless_canvas [output.ppm]
 */

#include <optics/gfx/canvas_cpu.h>

#include <stdio.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "headless_canvas.ppm";

    optics_gfx_canvas *canvas = nullptr;
    optics_gfx_result res = optics_gfx_canvas_create(
        &(optics_gfx_canvas_desc){
            .type = OPTICS_GFX_TYPE_CANVAS_DESC,
            .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
            .scale = 1.0f,
            .width = 512,
            .height = 320,
        },
        &canvas);
    if (res != OPTICS_GFX_OK) {
        fprintf(stderr, "headless_canvas: create failed (%s)\n", optics_gfx_result_string(res));
        return 1;
    }

    optics_gfx_color bg = optics_gfx_color_rgba_premul(18, 22, 32, 255);
    res = optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                              .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                              .clear_color = &bg,
                                          });
    if (res != OPTICS_GFX_OK) {
        fprintf(stderr, "headless_canvas: begin failed (%s)\n", optics_gfx_result_string(res));
        optics_gfx_canvas_release(canvas);
        return 1;
    }

    /* Card backdrop */
    optics_gfx_geometry card = optics_gfx_geom_rrect((optics_gfx_rect){48, 40, 416, 240},
                                                     optics_gfx_corner_radii_uniform(20.0f), 0.0f);
    optics_gfx_brush card_fill =
        optics_gfx_brush_solid(optics_gfx_color_rgba_premul(30, 36, 52, 255));
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &card, .brush = &card_fill});

    /* Gradient badge */
    optics_gfx_gradient_stop stops[2] = {
        {.t = 0.0f, .color = optics_gfx_color_rgba_premul(56, 189, 248, 255)},
        {.t = 1.0f, .color = optics_gfx_color_rgba_premul(129, 140, 248, 255)},
    };
    optics_gfx_brush grad = optics_gfx_brush_linear_gradient(
        (optics_gfx_point){96.0f, 96.0f}, (optics_gfx_point){240.0f, 224.0f}, stops, 2);
    optics_gfx_geometry disc = optics_gfx_geom_circle(168.0f, 160.0f, 64.0f);
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &disc, .brush = &grad});

    /* Accent bars */
    optics_gfx_brush bar_brush =
        optics_gfx_brush_solid(optics_gfx_color_rgba_premul(226, 232, 240, 220));
    optics_gfx_geometry bar1 = optics_gfx_geom_rrect((optics_gfx_rect){264, 118, 160, 18},
                                                     optics_gfx_corner_radii_uniform(9.0f), 0.0f);
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &bar1, .brush = &bar_brush});

    optics_gfx_brush sub_brush =
        optics_gfx_brush_solid(optics_gfx_color_rgba_premul(148, 163, 184, 180));
    optics_gfx_geometry bar2 = optics_gfx_geom_rrect((optics_gfx_rect){264, 152, 124, 14},
                                                     optics_gfx_corner_radii_uniform(7.0f), 0.0f);
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &bar2, .brush = &sub_brush});

    optics_gfx_geometry bar3 = optics_gfx_geom_rrect((optics_gfx_rect){264, 182, 92, 14},
                                                     optics_gfx_corner_radii_uniform(7.0f), 0.0f);
    optics_gfx_canvas_draw_geometry(
        canvas, &(optics_gfx_canvas_draw_geometry_opts){.geom = &bar3, .brush = &sub_brush});

    res = optics_gfx_canvas_end(canvas);
    if (res != OPTICS_GFX_OK) {
        fprintf(stderr, "headless_canvas: end failed (%s)\n", optics_gfx_result_string(res));
        optics_gfx_canvas_release(canvas);
        return 1;
    }

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *px = optics_gfx_canvas_cpu_pixels(canvas, &w, &h, &stride);
    FILE *out = fopen(path, "wb");
    if (!out || !px) {
        fprintf(stderr, "headless_canvas: cannot write %s\n", path);
        if (out)
            fclose(out);
        optics_gfx_canvas_release(canvas);
        return 1;
    }

    fprintf(out, "P6\n%u %u\n255\n", w, h);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = row + (size_t)x * 4u;
            uint8_t rgb[3] = {p[0], p[1], p[2]};
            fwrite(rgb, 1, sizeof rgb, out);
        }
    }
    fclose(out);
    fprintf(stderr, "headless_canvas: wrote %s (%ux%u)\n", path, w, h);
    optics_gfx_canvas_release(canvas);
    return 0;
}
