/* Public brush, stroke, and geometry validation contracts. */
#include "optics_test.h"
#include <math.h>
#include <optics/gfx.h>

static optics_gfx_result encode_one(const optics_gfx_geometry *g, const optics_gfx_brush *b,
                                    bool use_slot) {
    optics_gfx_encoder *enc = nullptr;
    OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
    optics_gfx_encoder_draw_geometry(enc,
                                     &(optics_gfx_encoder_draw_geometry_opts){
                                         .geom = g, .brush = b, .slot = 0, .use_slot = use_slot});
    optics_gfx_result st = optics_gfx_encoder_status(enc);
    optics_gfx_encoder_destroy(enc);
    return st;
}

int main(void) {
    optics_gfx_brush brush = optics_gfx_brush_solid(0xFF000000u);
    OT_EXPECT(brush.kind == OPTICS_GFX_BRUSH_SOLID);
    OT_EXPECT(brush.solid.color == 0xFF000000u);
    OT_EXPECT(brush.blend == OPTICS_GFX_BLEND_SRC_OVER);
    OT_EXPECT(brush.opacity == 1.0f);

    optics_gfx_geometry line = optics_gfx_geom_line(0, 0, 1, 1);
    OT_EXPECT(line.stroke.width == 1.0f);
    OT_EXPECT(line.stroke.cap == OPTICS_GFX_CAP_BUTT);
    OT_EXPECT(line.stroke.join == OPTICS_GFX_JOIN_MITER);

    /* RRect validation rejects negative/NaN radii and out-of-range curvature. */
    optics_gfx_geometry ok_rr = optics_gfx_geom_rrect(
        (optics_gfx_rect){0, 0, 20, 20}, optics_gfx_corner_radii_all(0, 4, 8, 2), 0.6f);
    OT_EXPECT(encode_one(&ok_rr, &brush, false) == OPTICS_GFX_OK);

    optics_gfx_geometry neg_r = optics_gfx_geom_rrect(
        (optics_gfx_rect){0, 0, 20, 20}, optics_gfx_corner_radii_all(0, -1, 0, 0), 0.0f);
    OT_EXPECT(encode_one(&neg_r, &brush, false) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_geometry nan_r = optics_gfx_geom_rrect(
        (optics_gfx_rect){0, 0, 20, 20}, optics_gfx_corner_radii_all(NAN, 0, 0, 0), 0.0f);
    OT_EXPECT(encode_one(&nan_r, &brush, false) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_geometry bad_curv = optics_gfx_geom_rrect(
        (optics_gfx_rect){0, 0, 20, 20}, optics_gfx_corner_radii_uniform(4.0f), 1.5f);
    OT_EXPECT(encode_one(&bad_curv, &brush, false) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    /* Image brush clip_radii and clip_curvature are validated too. */
    optics_gfx_geometry rect = optics_gfx_geom_rect((optics_gfx_rect){0, 0, 20, 20});
    optics_gfx_brush img_ok = {
        .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
        .blend = OPTICS_GFX_BLEND_SRC_OVER,
        .opacity = 1.0f,
        .image =
            {
                .clip_rect = {2, 2, 16, 16},
                .clip_radii = optics_gfx_corner_radii_all(0, 4, 4, 0),
                .clip_curvature = 0.5f,
                .tint = 0xFFFFFFFFu,
            },
    };
    OT_EXPECT(encode_one(&rect, &img_ok, true) == OPTICS_GFX_OK);

    optics_gfx_brush img_bad_clip = img_ok;
    img_bad_clip.image.clip_radii.br = -2.0f;
    OT_EXPECT(encode_one(&rect, &img_bad_clip, true) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_brush img_bad_curv = img_ok;
    img_bad_curv.image.clip_curvature = -0.1f;
    OT_EXPECT(encode_one(&rect, &img_bad_curv, true) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    return OT_REPORT();
}
