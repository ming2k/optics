/*
 * e2e_document_pipeline — one document, through every public layer, to pixels.
 *
 * This is the top of the pyramid. It owns no private headers and compiles no
 * library translation units: it links the public libraries exactly as an
 * application does, then proves the whole chain:
 *
 *   SVG source -> reticle document -> reticle raster      (document layer)
 *   SVG source -> lens runtime icon -> lens snapshot      (UI layer)
 *   snapshot -> immutable DisplayList -> serialized bytes (protocol layer)
 *   bytes -> deserialized DisplayList -> CPU canvas pixels (raster layer)
 *
 * The direct replay and the serialized round trip must be byte-identical. A
 * lossy serializer, a registry split, or a replay shortcut fails here even
 * when every unit test is green.
 */

#include "lens_test.h"
#include "optics_test.h"

#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <lens/icon.h>
#include <lens/lens.h>
#include <reticle/reticle.h>

#include <string.h>

/* Three independent paint runs so a collapsed or channel-swapped icon is
 * unmistakable. The same document feeds the direct raster and the UI icon. */
static const char SVG_ART[] = "<svg viewBox='0 0 24 24'>"
                              "<circle cx='12' cy='12' r='9' fill='none' stroke='#ff0000' "
                              "stroke-width='2'/>"
                              "<circle cx='12' cy='12' r='3' fill='#00ff00'/>"
                              "<path d='M8 12 L11 15 L16 8' fill='none' stroke='#0000ff' "
                              "stroke-width='2'/>"
                              "</svg>";

#define CANVAS_EXTENT 48u

typedef struct ink {
    long red, green, blue, other;
} ink;

static ink classify(const uint8_t *pixels, uint32_t width, uint32_t height, uint32_t stride) {
    ink k = {};
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t *p = pixels + (size_t)y * stride + (size_t)x * 4u;
            int r = p[0], g = p[1], b = p[2];
            if (r + g + b < 24)
                continue;
            if (r > g + 40 && r > b + 40)
                k.red++;
            else if (g > r + 40 && g > b + 40)
                k.green++;
            else if (b > r + 40 && b > g + 40)
                k.blue++;
            else
                k.other++;
        }
    }
    return k;
}

/* Play a display list into a CPU canvas and copy the pixels out. The caller
 * owns the returned buffer; `stride` is bytes. */
static bool replay(const flux_display_list *list, uint8_t *out, size_t out_size, uint32_t *width,
                   uint32_t *height, uint32_t *out_stride) {
    flux_canvas *canvas = nullptr;
    if (flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                               .backend = FLUX_CANVAS_BACKEND_CPU,
                                               .scale = 1.0f,
                                               .width = CANVAS_EXTENT,
                                               .height = CANVAS_EXTENT},
                           &canvas) != FLUX_OK)
        return false;
    flux_color clear = {};
    flux_result result =
        flux_canvas_begin(canvas, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                           .clear_color = &clear});
    if (result == FLUX_OK)
        result = flux_canvas_submit_display_list(
            canvas, &(flux_canvas_submit_display_list_opts){.list = list});
    if (result == FLUX_OK)
        result = flux_canvas_end(canvas);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *pixels =
        result == FLUX_OK ? flux_canvas_read_pixels(canvas, &w, &h, &stride) : nullptr;
    bool ok = pixels != nullptr && (size_t)stride * h <= out_size;
    if (ok) {
        memcpy(out, pixels, (size_t)stride * h);
        *width = w;
        *height = h;
        *out_stride = stride;
    }
    flux_canvas_release(canvas);
    return ok;
}

static void document_layer(void) {
    reticle_document *doc = nullptr;
    OT_REQUIRE(
        reticle_parse(&(reticle_parse_opts){.text = SVG_ART, .length = RETICLE_LENGTH_MEASURE},
                      &doc) == FLUX_OK);
    OT_EXPECT(doc != nullptr);

    reticle_pixmap pixmap = {};
    OT_REQUIRE(reticle_rasterize(
                   &(reticle_rasterize_opts){
                       .document = doc,
                       .width = 32,
                       .height = 32,
                       .format = RETICLE_PIXELS_RGBA8_PREMUL,
                       .paint = RETICLE_PAINT_THEME,
                   },
                   &pixmap) == FLUX_OK);
    OT_REQUIRE(pixmap.pixels != nullptr);
    ink direct = classify(pixmap.pixels, pixmap.width, pixmap.height, pixmap.stride);
    OT_EXPECT(direct.red > 0);
    OT_EXPECT(direct.green > 0);
    OT_EXPECT(direct.blue > 0);
    reticle_pixmap_deinit(&pixmap);
    reticle_release(doc);
    OT_EXPECT(reticle_live_documents() == 0);
}

static void ui_to_pixels(void) {
    lens_icon_id icon = lens_icon_register_svg(SVG_ART);
    OT_REQUIRE(icon != LENS_ICON_INVALID);

    lens *ui = nullptr;
    OT_REQUIRE(lens_create(&(lens_desc){.theme = lens_theme_dark()}, &ui) == FLUX_OK);

    lens_input input = {.display_size = {CANVAS_EXTENT, CANVAS_EXTENT}, .dt_seconds = 1.0f / 60.0f};
    lens_begin(ui, &input);
    lens_icon(ui, &(lens_icon_opts){.id = icon, .size = 32.0f});
    lens_test_end(ui);
    OT_EXPECT(!lens_overflowed(ui));
    OT_EXPECT(!lens_has_duplicate_ids(ui));

    lens_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(lens_snapshot_create(ui, &snapshot) == FLUX_OK);
    const flux_display_list *list = lens_snapshot_display_list(snapshot);
    OT_REQUIRE(list != nullptr);
    OT_EXPECT(flux_display_list_command_count(list) > 0);

    uint8_t direct_pixels[CANVAS_EXTENT * CANVAS_EXTENT * 4];
    uint32_t dw = 0, dh = 0, dstride = 0;
    OT_REQUIRE(replay(list, direct_pixels, sizeof direct_pixels, &dw, &dh, &dstride));
    ink direct = classify(direct_pixels, dw, dh, dstride);
    OT_EXPECT(direct.red > 0);
    OT_EXPECT(direct.green > 0);
    OT_EXPECT(direct.blue > 0);

    uint8_t *bytes = nullptr;
    size_t size = 0;
    flux_display_list_serialize_opts sopts = FLUX_DISPLAY_LIST_SERIALIZE_OPTS_INIT;
    sopts.out_data = &bytes;
    sopts.out_size = &size;
    OT_REQUIRE(flux_display_list_serialize(list, &sopts) == FLUX_OK);
    OT_EXPECT(bytes != nullptr && size > 0);

    flux_display_list *loaded = nullptr;
    flux_display_list_deserialize_opts dopts = FLUX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
    dopts.data = bytes;
    dopts.size = size;
    dopts.out_list = &loaded;
    OT_REQUIRE(flux_display_list_deserialize(&dopts) == FLUX_OK);
    OT_EXPECT(loaded != nullptr);
    OT_EXPECT(flux_display_list_command_count(loaded) > 0);

    uint8_t loaded_pixels[CANVAS_EXTENT * CANVAS_EXTENT * 4];
    uint32_t lw = 0, lh = 0, lstride = 0;
    OT_REQUIRE(replay(loaded, loaded_pixels, sizeof loaded_pixels, &lw, &lh, &lstride));
    OT_EXPECT(lw == dw && lh == dh && lstride == dstride);
    OT_EXPECT_MEM_EQ(loaded_pixels, direct_pixels, (size_t)dstride * dh);

    flux_display_list_serialized_free(bytes);
    flux_display_list_release(loaded);
    lens_snapshot_release(snapshot);
    lens_release(ui);
    /* The runtime-icon registry owns its parsed document for the process
     * lifetime, so the count is intentionally not zero here. */
}

int main(void) {
    document_layer();
    ui_to_pixels();
    return OT_REPORT();
}
