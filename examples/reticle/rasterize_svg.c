/*
 * rasterize_svg — headless reticle starter: parse an SVG document once and
 * rasterize it to a PPM file. No GPU, device, window or compositor.
 *
 * Usage: rasterize_svg [output.ppm]
 */

#include <reticle/reticle.h>

#include <stdio.h>
#include <string.h>

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
    const char *path = argc > 1 ? argv[1] : "reticle_demo.ppm";

    reticle_document *document = nullptr;
    flux_result result = reticle_parse(
        &(reticle_parse_opts){.text = ART, .length = RETICLE_LENGTH_MEASURE}, &document);
    if (result != FLUX_OK) {
        fprintf(stderr, "rasterize_svg: parse failed (%d)\n", (int)result);
        return 1;
    }

    reticle_pixmap pixmap = {};
    result = reticle_rasterize(
        &(reticle_rasterize_opts){
            .document = document,
            .width = 256,
            .height = 256,
            .supersample = 4,
            .format = RETICLE_PIXELS_RGBA8_PREMUL,
            .paint = RETICLE_PAINT_THEME,
            .color = flux_color_rgba(255, 255, 255, 255),
        },
        &pixmap);
    if (result != FLUX_OK) {
        fprintf(stderr, "rasterize_svg: rasterize failed (%d)\n", (int)result);
        reticle_release(document);
        return 1;
    }

    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "rasterize_svg: cannot open %s\n", path);
        reticle_pixmap_deinit(&pixmap);
        reticle_release(document);
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
                for (int channel = 0; channel < 3; channel++)
                    rgb[channel] = (uint8_t)((pixel[channel] * 255u + alpha / 2u) / alpha);
            }
            fwrite(rgb, 1, sizeof rgb, out);
        }
    }
    fclose(out);

    fprintf(stderr, "rasterize_svg: wrote %s (%ux%u)\n", path, pixmap.width, pixmap.height);
    reticle_pixmap_deinit(&pixmap);
    reticle_release(document);
    return 0;
}
