/*
 * Colour management (ADR-0069, Phase 2): the canvas renders into an
 * RGBA16F linear working space (scRGB) and applies an explicit output
 * transform at the end of every pass. These cases pin the observable
 * contract through offscreen-surface readback:
 *
 *   1. blending happens in linear light — 50% white over black reads
 *      back as sRGB-encode(0.5) ≈ 188, not the gamma-space 128;
 *   2. a Display P3 offscreen surface is negotiated via the surface
 *      color-space desc and content is transformed into it at output;
 *   3. a LOAD pass seeds the working-space intermediate from the
 *      destination's existing pixels, so a partial redraw preserves
 *      the rest of the frame;
 *   4. gradient stops interpolate in linear light — a black→white
 *      ramp's midpoint is ~187, not 128.
 *
 * Expected values are computed from the public math API
 * (flux_color_space_transform_matrix / flux_transfer_encode) rather than
 * hard-coded, so the test tracks the parametric model, not a driver.
 * Tolerances absorb the ±1 LSB TPDF output dither and 8-bit quantisation.
 */
#include "../icc_testkit.h"
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u
#define BYTES (W * H * 4u)

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

static bool near_ch(uint8_t got, int want, int tol) {
    int d = (int)got - want;
    return d >= -tol && d <= tol;
}

static flux_result render_frame(flux_surface *s, flux_canvas *canvas, flux_color clear,
                                void (*draw)(flux_canvas *, void *), void *user) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;
    r = flux_canvas_begin_frame(canvas, frame, &clear);
    if (r != FLUX_OK)
        return r;
    if (draw)
        draw(canvas, user);
    flux_canvas_end_frame(canvas);
    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    return flux_frame_present(frame);
}

static void draw_half_white(flux_canvas *canvas, void *user) {
    (void)user;
    flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)W, (float)H},
                                flux_color_rgba_premul(255, 255, 255, 128));
}

static void draw_red(flux_canvas *canvas, void *user) {
    (void)user;
    flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)W, (float)H},
                                flux_color_rgba(255, 0, 0, 255));
}

static void draw_black_white_gradient(flux_canvas *canvas, void *user) {
    (void)user;
    flux_gradient_stop stops[2] = {
        {0.0f, flux_color_rgba(0, 0, 0, 255)},
        {1.0f, flux_color_rgba(255, 255, 255, 255)},
    };
    flux_paint g =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 2);
    flux_canvas_fill_rect(canvas, (flux_rect){0, 0, (float)W, (float)H}, &g);
}

static void draw_image_full(flux_canvas *canvas, void *user) {
    flux_canvas_draw_image(canvas, user, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_color_management: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    }
    flux_canvas *canvas = nullptr;
    {
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = s;
        EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
    }

    static uint8_t px[BYTES];

    /* --- 1. Linear-light blending: 50% white over black -> ~188 --- */
    {
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(s, canvas, black, draw_half_white, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        /* sRGB-encode(0.502) x 255 = 187.6; the pre-ADR-0069 gamma-space
         * blend produced 128 — assert both the value and that the old
         * behaviour is gone. */
        EXPECT(near_ch(centre[0], 188, 3));
        EXPECT(centre[0] > 170);
        EXPECT(near_ch(centre[3], 255, 1));
    }

    /* --- 2. Display P3 offscreen surface: pure red transforms at output --- */
    {
        flux_color_space p3_spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_DISPLAY_P3};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = p3_spaces;
        csd.space_count = 1;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *p3s = nullptr;
        EXPECT(flux_surface_create(d, &sd, &p3s) == FLUX_OK);
        flux_canvas *p3c = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = p3s;
        EXPECT(flux_canvas_create(&cd, &p3c) == FLUX_OK);

        flux_surface_info info;
        flux_surface_get_info(p3s, &info);
        EXPECT(info.color_space.primaries == FLUX_PRIMARIES_DISPLAY_P3);
        EXPECT(info.color_space.transfer == FLUX_TRANSFER_SRGB);

        /* Expected: working-space red (1,0,0 linear BT.709) through the
         * scRGB -> P3 primaries matrix, then sRGB-encoded. */
        flux_mat3 to_p3;
        EXPECT(flux_color_space_transform_matrix((flux_color_space)FLUX_COLOR_SPACE_SCRGB,
                                                 (flux_color_space)FLUX_COLOR_SPACE_DISPLAY_P3,
                                                 &to_p3));
        flux_vec3 lin = flux_mat3_transform_vec3(to_p3, (flux_vec3){1.0f, 0.0f, 0.0f});
        int er = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.x) * 255.0f);
        int eg = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.y) * 255.0f);
        int eb = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.z) * 255.0f);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(p3s, p3c, black, draw_red, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(p3s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], er, 4));
        EXPECT(near_ch(centre[1], eg, 4));
        EXPECT(near_ch(centre[2], eb, 4));
        /* Sanity: P3 is wider than BT.709, so saturated sRGB red must land
         * INSIDE the gamut — red channel drops well below 255. */
        EXPECT(centre[0] < 250 && centre[0] > 200);

        flux_canvas_destroy(p3c);
        flux_surface_release(p3s);
    }

    /* --- 3. LOAD pass seeds from the previous frame's pixels --- */
    {
        flux_surface_info info;
        flux_surface_get_info(s, &info);
        /* Offscreen surfaces have one image per frame slot: paint every
         * slot red so the image the LOAD frame lands on holds red. */
        flux_color red = flux_color_rgba(255, 0, 0, 255);
        for (uint32_t i = 0; i < info.image_count; ++i)
            EXPECT(render_frame(s, canvas, red, nullptr, nullptr) == FLUX_OK);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
        /* NULL clear colour => LOAD: the pass must seed the intermediate
         * with the destination's current pixels (ADR-0069 seed blit). */
        EXPECT(flux_canvas_begin_frame(canvas, frame, nullptr) == FLUX_OK);
        flux_canvas_fill_rect_color(canvas, (flux_rect){0, 0, (float)W / 2, (float)H},
                                    flux_color_rgba(0, 0, 255, 255));
        flux_canvas_end_frame(canvas);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *left = px_at(px, W / 4, H / 2);
        const uint8_t *right = px_at(px, 3 * W / 4, H / 2);
        EXPECT(near_ch(left[2], 255, 3) && left[0] < 5 && left[1] < 5);
        EXPECT(near_ch(right[0], 255, 3) && right[1] < 5 && right[2] < 5);
    }

    /* --- 4. Gradient stops interpolate in linear light --- */
    {
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(s, canvas, black, draw_black_white_gradient, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        /* x = W/2: t = 0.504, i.e. ~0.5 linear -> sRGB ~188. The
         * gamma-space interpolation this replaces produced 128. */
        const uint8_t *mid = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(mid[0], 187, 4));
        EXPECT(mid[0] > 170);
        /* Linear light also lifts the dark end: one pixel in (t = 1.5/128,
         * 0.0117 linear) encodes to ~28, not the gamma-space ~3. The bright
         * end stays pinned and the ramp is monotone. */
        EXPECT(near_ch(px_at(px, 1, H / 2)[0], 28, 4));
        EXPECT(px_at(px, W - 2, H / 2)[0] >= 252);
        EXPECT(px_at(px, W / 4, H / 2)[0] < mid[0]);
        EXPECT(mid[0] < px_at(px, 3 * W / 4, H / 2)[0]);
    }

    /* --- 5. BT.2020 (gamma 2.4) offscreen surface --- */
    {
        flux_color_space spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_BT2020};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *bs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &bs) == FLUX_OK);
        flux_canvas *bc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = bs;
        EXPECT(flux_canvas_create(&cd, &bc) == FLUX_OK);

        flux_surface_info info;
        flux_surface_get_info(bs, &info);
        EXPECT(info.color_space.primaries == FLUX_PRIMARIES_BT2020);
        EXPECT(info.color_space.transfer == FLUX_TRANSFER_GAMMA);

        flux_mat3 to_2020;
        EXPECT(flux_color_space_transform_matrix((flux_color_space)FLUX_COLOR_SPACE_SCRGB,
                                                 (flux_color_space)FLUX_COLOR_SPACE_BT2020,
                                                 &to_2020));
        flux_vec3 lin = flux_mat3_transform_vec3(to_2020, (flux_vec3){1.0f, 0.0f, 0.0f});
        int er = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_GAMMA, 2.4f, lin.x) * 255.0f);
        int eg = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_GAMMA, 2.4f, lin.y) * 255.0f);
        int eb = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_GAMMA, 2.4f, lin.z) * 255.0f);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(bs, bc, black, draw_red, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(bs, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], er, 4));
        EXPECT(near_ch(centre[1], eg, 4));
        EXPECT(near_ch(centre[2], eb, 4));

        flux_canvas_destroy(bc);
        flux_surface_release(bs);
    }

    /* --- 6. scRGB-linear offscreen: the transfer stage is provably off --- */
    {
        flux_color_space spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_SCRGB};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *ls = nullptr;
        EXPECT(flux_surface_create(d, &sd, &ls) == FLUX_OK);
        flux_canvas *lc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = ls;
        EXPECT(flux_canvas_create(&cd, &lc) == FLUX_OK);

        /* 50% white over black: 0.502 linear written verbatim to 8-bit
         * lands on 128 — the number the sRGB case 1 must NOT produce. */
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(ls, lc, black, draw_half_white, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(ls, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], 128, 3));

        flux_canvas_destroy(lc);
        flux_surface_release(ls);
    }

    /* --- 9. Output override: sRGB container, P3 content (legacy display path) --- */
    {
        flux_color_space want_swapchain[] = {(flux_color_space)FLUX_COLOR_SPACE_SRGB};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = want_swapchain;
        csd.space_count = 1;
        flux_surface_output_color_desc ocd = FLUX_SURFACE_OUTPUT_COLOR_DESC_INIT;
        ocd.content_space = (flux_color_space)FLUX_COLOR_SPACE_DISPLAY_P3;
        csd.next = &ocd;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *os = nullptr;
        EXPECT(flux_surface_create(d, &sd, &os) == FLUX_OK);
        flux_canvas *oc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = os;
        EXPECT(flux_canvas_create(&cd, &oc) == FLUX_OK);

        /* The container stays sRGB; the content space is the override. */
        flux_surface_info info;
        flux_surface_get_info(os, &info);
        EXPECT(info.color_space.primaries == FLUX_PRIMARIES_BT709);
        EXPECT(info.content_space.primaries == FLUX_PRIMARIES_DISPLAY_P3);

        /* Pixels come out in the override space — same expectation as
         * the negotiated-P3 case 2. */
        flux_mat3 to_p3;
        EXPECT(flux_color_space_transform_matrix((flux_color_space)FLUX_COLOR_SPACE_SCRGB,
                                                 (flux_color_space)FLUX_COLOR_SPACE_DISPLAY_P3,
                                                 &to_p3));
        flux_vec3 lin = flux_mat3_transform_vec3(to_p3, (flux_vec3){1.0f, 0.0f, 0.0f});
        int er = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.x) * 255.0f);
        int eg = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.y) * 255.0f);
        int eb = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, lin.z) * 255.0f);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(os, oc, black, draw_red, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(os, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], er, 4));
        EXPECT(near_ch(centre[1], eg, 4));
        EXPECT(near_ch(centre[2], eb, 4));

        flux_canvas_destroy(oc);
        flux_surface_release(os);
    }

    /* --- 7. P3-tagged image: params-block decode into the working space --- */
    {
        /* A mid-tone P3 colour: far enough from the gamut corners that
         * the P3 -> BT.709 matrix changes it measurably. */
        const uint8_t content[4] = {60, 180, 120, 255}; /* P3-encoded RGB */
        flux_color_space p3 = FLUX_COLOR_SPACE_DISPLAY_P3;
        flux_image_color_space_desc csd = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.space = &p3;
        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = FLUX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = content;
        flux_image *img = nullptr;
        EXPECT(flux_image_create(d, &idesc, &img) == FLUX_OK);

        /* Expected: sRGB-decode the encoded value, P3 -> working matrix,
         * re-encode for the sRGB surface. */
        flux_mat3 to_work;
        EXPECT(flux_color_space_transform_matrix(p3, (flux_color_space)FLUX_COLOR_SPACE_SCRGB,
                                                 &to_work));
        int want[3];
        flux_vec3 straight = {flux_transfer_decode(FLUX_TRANSFER_SRGB, 0.0f, content[0] / 255.0f),
                              flux_transfer_decode(FLUX_TRANSFER_SRGB, 0.0f, content[1] / 255.0f),
                              flux_transfer_decode(FLUX_TRANSFER_SRGB, 0.0f, content[2] / 255.0f)};
        flux_vec3 work = flux_mat3_transform_vec3(to_work, straight);
        want[0] = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, work.x) * 255.0f);
        want[1] = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, work.y) * 255.0f);
        want[2] = (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, work.z) * 255.0f);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(s, canvas, black, draw_image_full, img) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], want[0], 4));
        EXPECT(near_ch(centre[1], want[1], 4));
        EXPECT(near_ch(centre[2], want[2], 4));
        /* The matrix must actually change the colour (else the tag is a
         * no-op): untagged sRGB interpretation would read back verbatim. */
        EXPECT(centre[0] != content[0] || centre[1] != content[1] || centre[2] != content[2]);
        flux_image_release(img);
    }

    /* --- 8. ICC-tagged images: parametric extraction and LUT bake --- */
    {
        uint8_t profile[16384];
        uint8_t pdata[8192];
        icc_tag_def defs[8];
        size_t dlen = 0;

        /* 8a. sRGB-ICC-tagged teal renders like the untagged original. */
        uint32_t n = icc_build_matrix_tags(pdata, defs, 0, &dlen);
        size_t size = icc_build_profile(profile, "mntr", "XYZ ", pdata, dlen, defs, n);
        flux_icc_profile *srgb_icc = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &srgb_icc) == FLUX_OK);

        const uint8_t teal[4] = {17, 101, 149, 255};
        flux_image_color_space_desc csd = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.icc = srgb_icc;
        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = FLUX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = teal;
        flux_image *tagged = nullptr;
        EXPECT(flux_image_create(d, &idesc, &tagged) == FLUX_OK);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(s, canvas, black, draw_image_full, tagged) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], 17, 3));
        EXPECT(near_ch(centre[1], 101, 3));
        EXPECT(near_ch(centre[2], 149, 3));
        flux_image_release(tagged);

        /* 8b. LUT-profile image (constant D50-white CLUT) draws white. */
        size_t len = icc_build_mft1_constant_white(pdata, &defs[0]);
        size = icc_build_profile(profile, "mntr", "XYZ ", pdata, len, defs, 1);
        flux_icc_profile *lut_icc = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &lut_icc) == FLUX_OK);
        csd.icc = lut_icc;
        flux_image *lut_img = nullptr;
        EXPECT(flux_image_create(d, &idesc, &lut_img) == FLUX_OK);
        EXPECT(render_frame(s, canvas, black, draw_image_full, lut_img) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        centre = px_at(px, W / 2, H / 2);
        EXPECT(centre[0] > 245 && centre[1] > 245 && centre[2] > 245);
        flux_image_release(lut_img);

        flux_icc_profile_release(srgb_icc);
        flux_icc_profile_release(lut_icc);
    }

    /* --- 9. BT.2020 PQ tagged image rendered onto sRGB surface: 203 nits maps to sRGB 1.0 white
     * --- */
    {
        /* 203 nits in ST 2084 PQ is ~0.5807 encoded (148/255). */
        uint8_t pq_white_203nits = (uint8_t)lrintf(
            flux_transfer_encode(FLUX_TRANSFER_PQ, 0.0f, 203.0f / 10000.0f) * 255.0f);
        const uint8_t pq_content[4] = {pq_white_203nits, pq_white_203nits, pq_white_203nits, 255};
        flux_color_space pq_space = FLUX_COLOR_SPACE_BT2020_PQ;
        flux_image_color_space_desc csd = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.space = &pq_space;
        flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = FLUX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = pq_content;
        flux_image *pq_img = nullptr;
        EXPECT(flux_image_create(d, &idesc, &pq_img) == FLUX_OK);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(s, canvas, black, draw_image_full, pq_img) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        /* Must decode 203 nits to ~1.0 in working space and output as ~255 white, not ~38. */
        EXPECT(centre[0] >= 250 && centre[1] >= 250 && centre[2] >= 250);
        flux_image_release(pq_img);
    }

    /* --- 10. BT.2020 PQ offscreen: deep container + PQ output encode --- */
    {
        flux_color_space spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_BT2020_PQ};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *pqs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &pqs) == FLUX_OK);
        flux_canvas *pqc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = pqs;
        EXPECT(flux_canvas_create(&cd, &pqc) == FLUX_OK);

        /* The 8-bit era rejected this outright; now the container must be
         * deep (10-bit preferred, 16F fallback) and the info reports HDR. */
        flux_surface_info info;
        flux_surface_get_info(pqs, &info);
        EXPECT(info.color_space.primaries == FLUX_PRIMARIES_BT2020);
        EXPECT(info.content_space.transfer == FLUX_TRANSFER_PQ);
        EXPECT(info.hdr);
        EXPECT(info.format == FLUX_FORMAT_RGB10A2_UNORM ||
               info.format == FLUX_FORMAT_RGBA16_SFLOAT);

        /* 50% white over black = 0.502 working-space linear; the output
         * transform maps it through the BT.2408 SDR-white scale and PQ
         * encodes. Readback quantizes the deep container back to 8-bit. */
        const float lin = 128.0f / 255.0f;
        int want = (int)lrintf(
            flux_transfer_encode(FLUX_TRANSFER_PQ, 0.0f, lin * (203.0f / 10000.0f)) * 255.0f);
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(pqs, pqc, black, draw_half_white, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(pqs, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], want, 4));
        EXPECT(near_ch(centre[0], centre[1], 2) && near_ch(centre[1], centre[2], 2));
        /* PQ ~0.51 here: plainly not the SDR sRGB (~188) or linear (128)
         * numbers — the encode stage provably ran. */
        EXPECT(centre[0] > 100 && centre[0] < 160);

        flux_canvas_destroy(pqc);
        flux_surface_release(pqs);
    }

    /* --- 11. explicit offscreen format list: sRGB in a 16F container --- */
    {
        flux_color_space spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_SRGB};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        flux_format formats[] = {FLUX_FORMAT_RGBA16_SFLOAT};
        flux_surface_offscreen_format_desc fsd = FLUX_SURFACE_OFFSCREEN_FORMAT_DESC_INIT;
        fsd.formats = formats;
        fsd.format_count = 1;
        csd.next = &fsd;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *fs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &fs) == FLUX_OK);
        flux_canvas *fc = nullptr;
        flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
        cd.surface = fs;
        EXPECT(flux_canvas_create(&cd, &fc) == FLUX_OK);

        flux_surface_info info;
        flux_surface_get_info(fs, &info);
        EXPECT(info.format == FLUX_FORMAT_RGBA16_SFLOAT);
        EXPECT(!info.hdr);

        flux_color black = flux_color_rgba(0, 0, 0, 255);
        EXPECT(render_frame(fs, fc, black, draw_red, nullptr) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(fs, px, BYTES) == FLUX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(near_ch(centre[0], 255, 2) && centre[1] < 3 && centre[2] < 3);

        flux_canvas_destroy(fc);
        flux_surface_release(fs);
    }

    /* --- 12. an unsuitable explicit format list is rejected --- */
    {
        /* PQ content cannot live in an 8-bit container. */
        flux_color_space spaces[] = {(flux_color_space)FLUX_COLOR_SPACE_BT2020_PQ};
        flux_surface_color_space_desc csd = FLUX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        flux_format formats[] = {FLUX_FORMAT_BGRA8_UNORM};
        flux_surface_offscreen_format_desc fsd = FLUX_SURFACE_OFFSCREEN_FORMAT_DESC_INIT;
        fsd.formats = formats;
        fsd.format_count = 1;
        csd.next = &fsd;
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        flux_surface *rs = nullptr;
        EXPECT(flux_surface_create(d, &sd, &rs) == FLUX_ERROR_UNSUPPORTED);
        EXPECT(rs == nullptr);
    }

    /* --- 13. v2 and v4 Lab PCS profiles render the same gray --- */
    {
        uint8_t profile[16384];
        uint8_t pdata[8192];
        icc_tag_def defs[8];

        size_t len = icc_build_mft2_constant_lab(pdata, &defs[0], 50.0, 0.0, 0.0, true);
        size_t size = icc_build_profile(profile, "mntr", "Lab ", pdata, len, defs, 1);
        profile[8] = 0x02; /* ICC v2: the 0xFF00-full-scale Lab encoding */
        flux_icc_profile *lab_v2 = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &lab_v2) == FLUX_OK);

        len = icc_build_mft2_constant_lab(pdata, &defs[0], 50.0, 0.0, 0.0, false);
        size = icc_build_profile(profile, "mntr", "Lab ", pdata, len, defs, 1);
        flux_icc_profile *lab_v4 = nullptr;
        EXPECT(flux_icc_profile_create(profile, size, &lab_v4) == FLUX_OK);

        /* L* 50 neutral: working-space Y ~0.184, sRGB-encoded ~119. */
        uint8_t got[2][3];
        flux_color black = flux_color_rgba(0, 0, 0, 255);
        const flux_icc_profile *profiles[2] = {lab_v2, lab_v4};
        for (int i = 0; i < 2; ++i) {
            flux_image_color_space_desc csd = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
            csd.icc = profiles[i];
            flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
            idesc.next = &csd;
            idesc.width = 1;
            idesc.height = 1;
            idesc.format = FLUX_FORMAT_RGBA8_UNORM;
            static const uint8_t any[4] = {77, 88, 99, 255}; /* CLUT is constant */
            idesc.initial_data = any;
            flux_image *img = nullptr;
            EXPECT(flux_image_create(d, &idesc, &img) == FLUX_OK);
            EXPECT(render_frame(s, canvas, black, draw_image_full, img) == FLUX_OK);
            memset(px, 0xCD, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
            memcpy(got[i], px_at(px, W / 2, H / 2), 3);
            flux_image_release(img);
        }
        for (int ch = 0; ch < 3; ++ch) {
            EXPECT(near_ch(got[0][ch], 119, 5));
            EXPECT(near_ch(got[0][ch], got[1][ch], 2));
        }
        /* Neutral stays neutral. */
        EXPECT(near_ch(got[0][0], got[0][1], 3) && near_ch(got[0][1], got[0][2], 3));

        flux_icc_profile_release(lab_v2);
        flux_icc_profile_release(lab_v4);
    }

    flux_canvas_destroy(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
