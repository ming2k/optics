/*
 * headless_svg.c — GPU-free SVG parsing and rasterization to PPM (ADR-0171).
 *
 * Usage: headless_svg [output.ppm]
 */

#include <optics/svg.h>

#include <stdio.h>

static const char ART[] =
    "<svg viewBox=\"0 0 64 64\">"
    "<defs><linearGradient id=\"g\" x1=\"0\" y1=\"0\" x2=\"64\" y2=\"64\">"
    "<stop offset=\"0\" stop-color=\"#57c7ff\"/>"
    "<stop offset=\"1\" stop-color=\"#b06bff\"/>"
    "</linearGradient></defs>"
    "<rect x=\"4\" y=\"4\" width=\"56\" height=\"56\" rx=\"14\" fill=\"url(#g)\"/>"
    "<circle cx=\"32\" cy=\"32\" r=\"12\" fill=\"#ffffff\" fill-opacity=\"0.85\"/>"
    "</svg>";

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "headless_svg.ppm";

    optics_svg_document *document = nullptr;
    optics_gfx_result result = optics_svg_parse(
        &(optics_svg_parse_opts){.text = ART, .length = OPTICS_SVG_LENGTH_MEASURE}, &document);
    if (result != OPTICS_GFX_OK) {
        fprintf(stderr, "headless_svg: parse failed (%d)\n", (int)result);
        return 1;
    }

    optics_svg_pixmap pixmap = {};
    result = optics_svg_rasterize(
        &(optics_svg_rasterize_opts){
            .document = document,
            .width = 256,
            .height = 256,
            .format = OPTICS_SVG_PIXELS_RGBA8_PREMUL,
            .paint = OPTICS_SVG_PAINT_THEME,
            .color = optics_gfx_color_rgba(255, 255, 255, 255),
        },
        &pixmap);
    if (result != OPTICS_GFX_OK) {
        fprintf(stderr, "headless_svg: rasterize failed (%d)\n", (int)result);
        optics_svg_release(document);
        return 1;
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "headless_svg: cannot open %s\n", path);
        optics_svg_pixmap_deinit(&pixmap);
        optics_svg_release(document);
        return 1;
    }

    fprintf(out, "P6\n%u %u\n255\n", pixmap.width, pixmap.height);
    for (uint32_t y = 0; y < pixmap.height; y++) {
        const uint8_t *row = pixmap.pixels + (size_t)y * pixmap.stride;
        for (uint32_t x = 0; x < pixmap.width; x++) {
            const uint8_t *pixel = row + (size_t)x * 4u;
            uint8_t alpha = pixel[3];
            uint8_t rgb[3] = {};
            if (alpha > 0) {
                for (int c = 0; c < 3; c++)
                    rgb[c] = (uint8_t)((pixel[c] * 255u + alpha / 2u) / alpha);
            }
            fwrite(rgb, 1, sizeof rgb, out);
        }
    }
    fclose(out);

    fprintf(stderr, "headless_svg: wrote %s (%ux%u)\n", path, pixmap.width, pixmap.height);
    optics_svg_pixmap_deinit(&pixmap);
    optics_svg_release(document);
    return 0;
}
