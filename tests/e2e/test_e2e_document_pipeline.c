/*
 * e2e_document_pipeline — one document, through every public layer, to pixels.
 *
 * This is the top of the pyramid. It owns no private headers and compiles no
 * library translation units: it links the public libraries exactly as an
 * application does, then proves the whole chain:
 *
 *   SVG source -> svg document -> svg raster      (document layer)
 *   SVG source -> ui runtime icon -> ui snapshot      (UI layer)
 *   snapshot -> immutable DisplayList -> serialized bytes (protocol layer)
 *   bytes -> deserialized DisplayList -> CPU canvas pixels (raster layer)
 *
 * The direct replay and the serialized round trip must be byte-identical. A
 * lossy serializer, a registry split, or a replay shortcut fails here even
 * when every unit test is green.
 */

#include "optics_test.h"
#include "ui_test.h"

#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/svg.h>
#include <optics/ui.h>
#include <optics/ui/icon.h>

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
static bool replay(const optics_gfx_display_list *list, uint8_t *out, size_t out_size,
                   uint32_t *width, uint32_t *height, uint32_t *out_stride) {
    optics_gfx_canvas *canvas = nullptr;
    if (optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = CANVAS_EXTENT,
                                                           .height = CANVAS_EXTENT},
                                 &canvas) != OPTICS_GFX_OK)
        return false;
    optics_gfx_color clear = {};
    optics_gfx_result result = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .clear_color = &clear});
    if (result == OPTICS_GFX_OK)
        result = optics_gfx_canvas_submit_display_list(
            canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = list});
    if (result == OPTICS_GFX_OK)
        result = optics_gfx_canvas_end(canvas);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *pixels =
        result == OPTICS_GFX_OK ? optics_gfx_canvas_read_pixels(canvas, &w, &h, &stride) : nullptr;
    bool ok = pixels != nullptr && (size_t)stride * h <= out_size;
    if (ok) {
        memcpy(out, pixels, (size_t)stride * h);
        *width = w;
        *height = h;
        *out_stride = stride;
    }
    optics_gfx_canvas_release(canvas);
    return ok;
}

static void document_layer(void) {
    optics_svg_document *doc = nullptr;
    OT_REQUIRE(optics_svg_parse(
                   &(optics_svg_parse_opts){.text = SVG_ART, .length = OPTICS_SVG_LENGTH_MEASURE},
                   &doc) == OPTICS_GFX_OK);
    OT_EXPECT(doc != nullptr);

    optics_svg_pixmap pixmap = {};
    OT_REQUIRE(optics_svg_rasterize(
                   &(optics_svg_rasterize_opts){
                       .document = doc,
                       .width = 32,
                       .height = 32,
                       .format = OPTICS_SVG_PIXELS_RGBA8_PREMUL,
                       .paint = OPTICS_SVG_PAINT_THEME,
                   },
                   &pixmap) == OPTICS_GFX_OK);
    OT_REQUIRE(pixmap.pixels != nullptr);
    ink direct = classify(pixmap.pixels, pixmap.width, pixmap.height, pixmap.stride);
    OT_EXPECT(direct.red > 0);
    OT_EXPECT(direct.green > 0);
    OT_EXPECT(direct.blue > 0);
    optics_svg_pixmap_deinit(&pixmap);
    optics_svg_release(doc);
    OT_EXPECT(optics_svg_live_documents() == 0);
}

static void ui_to_pixels(void) {
    optics_ui_icon_id icon = optics_ui_icon_register_svg(SVG_ART);
    OT_REQUIRE(icon != OPTICS_UI_ICON_INVALID);

    optics_ui *ui = nullptr;
    OT_REQUIRE(optics_ui_create(&(optics_ui_desc){.theme = optics_ui_theme_dark()}, &ui) ==
               OPTICS_GFX_OK);

    optics_ui_input input = {.display_size = {CANVAS_EXTENT, CANVAS_EXTENT},
                             .dt_seconds = 1.0f / 60.0f};
    optics_ui_begin(ui, &input);
    optics_ui_icon(ui, &(optics_ui_icon_opts){.id = icon, .size = 32.0f});
    optics_ui_test_end(ui);
    OT_EXPECT(!optics_ui_overflowed(ui));
    OT_EXPECT(!optics_ui_has_duplicate_ids(ui));

    optics_ui_scene_snapshot *snapshot = nullptr;
    OT_REQUIRE(optics_ui_snapshot_create(ui, &snapshot) == OPTICS_GFX_OK);
    const optics_gfx_display_list *list = optics_ui_snapshot_display_list(snapshot);
    OT_REQUIRE(list != nullptr);
    OT_EXPECT(optics_gfx_display_list_command_count(list) > 0);

    uint8_t direct_pixels[CANVAS_EXTENT * CANVAS_EXTENT * 4];
    uint32_t dw = 0, dh = 0, dstride = 0;
    OT_REQUIRE(replay(list, direct_pixels, sizeof direct_pixels, &dw, &dh, &dstride));
    ink direct = classify(direct_pixels, dw, dh, dstride);
    OT_EXPECT(direct.red > 0);
    OT_EXPECT(direct.green > 0);
    OT_EXPECT(direct.blue > 0);

    uint8_t *bytes = nullptr;
    size_t size = 0;
    optics_gfx_display_list_serialize_opts sopts = OPTICS_GFX_DISPLAY_LIST_SERIALIZE_OPTS_INIT;
    sopts.out_data = &bytes;
    sopts.out_size = &size;
    OT_REQUIRE(optics_gfx_display_list_serialize(list, &sopts) == OPTICS_GFX_OK);
    OT_EXPECT(bytes != nullptr && size > 0);

    optics_gfx_display_list *loaded = nullptr;
    optics_gfx_display_list_deserialize_opts dopts = OPTICS_GFX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
    dopts.data = bytes;
    dopts.size = size;
    dopts.out_list = &loaded;
    OT_REQUIRE(optics_gfx_display_list_deserialize(&dopts) == OPTICS_GFX_OK);
    OT_EXPECT(loaded != nullptr);
    OT_EXPECT(optics_gfx_display_list_command_count(loaded) > 0);

    uint8_t loaded_pixels[CANVAS_EXTENT * CANVAS_EXTENT * 4];
    uint32_t lw = 0, lh = 0, lstride = 0;
    OT_REQUIRE(replay(loaded, loaded_pixels, sizeof loaded_pixels, &lw, &lh, &lstride));
    OT_EXPECT(lw == dw && lh == dh && lstride == dstride);
    OT_EXPECT_MEM_EQ(loaded_pixels, direct_pixels, (size_t)dstride * dh);

    optics_gfx_display_list_serialized_free(bytes);
    optics_gfx_display_list_release(loaded);
    optics_ui_snapshot_release(snapshot);
    optics_ui_release(ui);
    /* The runtime-icon registry owns its parsed document for the process
     * lifetime, so the count is intentionally not zero here. */
}

int main(void) {
    document_layer();
    ui_to_pixels();
    return OT_REPORT();
}
