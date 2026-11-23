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
 * (optics_gfx_color_space_transform_matrix / optics_gfx_transfer_encode) rather than
 * hard-coded, so the test tracks the parametric model, not a driver.
 * Tolerances absorb the ±1 LSB TPDF output dither and 8-bit quantisation.
 */
#include "../icc_testkit.h"
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

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

static optics_gfx_result render_frame(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                                      optics_gfx_color clear,
                                      void (*draw)(optics_gfx_canvas *, void *), void *user) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;
    r = optics_gfx_canvas_begin(
        canvas, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                               .frame = frame,
                                               .clear_color = &clear});
    if (r != OPTICS_GFX_OK)
        return r;
    if (draw)
        draw(canvas, user);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

static void draw_half_white(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                      optics_gfx_color_rgba_premul(255, 255, 255, 128));
}

static void draw_red(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                      optics_gfx_color_rgba(255, 0, 0, 255));
}

static void draw_black_white_gradient(optics_gfx_canvas *canvas, void *user) {
    (void)user;
    optics_gfx_gradient_stop stops[2] = {
        {0.0f, optics_gfx_color_rgba(0, 0, 0, 255)},
        {1.0f, optics_gfx_color_rgba(255, 255, 255, 255)},
    };
    optics_gfx_brush g = optics_gfx_brush_linear_gradient(
        (optics_gfx_point){0, 0}, (optics_gfx_point){(float)W, 0}, stops, 2);
    optics_gfx_canvas_fill_rect(canvas, (optics_gfx_rect){0, 0, (float)W, (float)H}, &g);
}

static void draw_image_full(optics_gfx_canvas *canvas, void *user) {
    optics_gfx_canvas_draw_image(canvas, user, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                 nullptr);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_color_management: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    }
    optics_gfx_canvas *canvas = nullptr;
    {
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
    }

    static uint8_t px[BYTES];

    /* --- 1. Linear-light blending: 50% white over black -> ~188 --- */
    {
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(s, canvas, black, draw_half_white, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        /* sRGB-encode(0.502) x 255 = 187.6; the pre-ADR-0069 gamma-space
         * blend produced 128 — assert both the value and that the old
         * behaviour is gone. */
        OT_EXPECT(near_ch(centre[0], 188, 3));
        OT_EXPECT(centre[0] > 170);
        OT_EXPECT(near_ch(centre[3], 255, 1));
    }

    /* --- 2. Display P3 offscreen surface: pure red transforms at output --- */
    {
        optics_gfx_color_space p3_spaces[] = {
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_DISPLAY_P3};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = p3_spaces;
        csd.space_count = 1;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *p3s = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &p3s) == OPTICS_GFX_OK);
        optics_gfx_canvas *p3c = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = p3s;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &p3c) == OPTICS_GFX_OK);

        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(p3s, &info);
        OT_EXPECT(info.color_space.primaries == OPTICS_GFX_PRIMARIES_DISPLAY_P3);
        OT_EXPECT(info.color_space.transfer == OPTICS_GFX_TRANSFER_SRGB);

        /* Expected: working-space red (1,0,0 linear BT.709) through the
         * scRGB -> P3 primaries matrix, then sRGB-encoded. */
        optics_gfx_mat3 to_p3;
        OT_EXPECT(optics_gfx_color_space_transform_matrix(
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB,
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_DISPLAY_P3, &to_p3));
        optics_gfx_vec3 lin =
            optics_gfx_mat3_transform_vec3(to_p3, (optics_gfx_vec3){1.0f, 0.0f, 0.0f});
        int er =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.x) * 255.0f);
        int eg =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.y) * 255.0f);
        int eb =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.z) * 255.0f);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(p3s, p3c, black, draw_red, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(p3s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], er, 4));
        OT_EXPECT(near_ch(centre[1], eg, 4));
        OT_EXPECT(near_ch(centre[2], eb, 4));
        /* Sanity: P3 is wider than BT.709, so saturated sRGB red must land
         * INSIDE the gamut — red channel drops well below 255. */
        OT_EXPECT(centre[0] < 250 && centre[0] > 200);

        optics_gfx_canvas_release(p3c);
        optics_gfx_surface_release(p3s);
    }

    /* --- 3. LOAD pass seeds from the previous frame's pixels --- */
    {
        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(s, &info);
        /* Offscreen surfaces have one image per frame slot: paint every
         * slot red so the image the LOAD frame lands on holds red. */
        optics_gfx_color red = optics_gfx_color_rgba(255, 0, 0, 255);
        for (uint32_t i = 0; i < info.image_count; ++i)
            OT_EXPECT(render_frame(s, canvas, red, nullptr, nullptr) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        /* NULL clear colour => LOAD: the pass must seed the intermediate
         * with the destination's current pixels (ADR-0069 seed blit). */
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                      .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                      .frame = frame,
                                                      .clear_color = nullptr}) == OPTICS_GFX_OK);
        optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0, 0, (float)W / 2, (float)H},
                                          optics_gfx_color_rgba(0, 0, 255, 255));
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *left = px_at(px, W / 4, H / 2);
        const uint8_t *right = px_at(px, 3 * W / 4, H / 2);
        OT_EXPECT(near_ch(left[2], 255, 3) && left[0] < 5 && left[1] < 5);
        OT_EXPECT(near_ch(right[0], 255, 3) && right[1] < 5 && right[2] < 5);
    }

    /* --- 4. Gradient stops interpolate in linear light --- */
    {
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(s, canvas, black, draw_black_white_gradient, nullptr) ==
                  OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        /* x = W/2: t = 0.504, i.e. ~0.5 linear -> sRGB ~188. The
         * gamma-space interpolation this replaces produced 128. */
        const uint8_t *mid = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(mid[0], 187, 4));
        OT_EXPECT(mid[0] > 170);
        /* Linear light also lifts the dark end: one pixel in (t = 1.5/128,
         * 0.0117 linear) encodes to ~28, not the gamma-space ~3. The bright
         * end stays pinned and the ramp is monotone. */
        OT_EXPECT(near_ch(px_at(px, 1, H / 2)[0], 28, 4));
        OT_EXPECT(px_at(px, W - 2, H / 2)[0] >= 252);
        OT_EXPECT(px_at(px, W / 4, H / 2)[0] < mid[0]);
        OT_EXPECT(mid[0] < px_at(px, 3 * W / 4, H / 2)[0]);
    }

    /* --- 5. BT.2020 (gamma 2.4) offscreen surface --- */
    {
        optics_gfx_color_space spaces[] = {(optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_BT2020};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *bs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &bs) == OPTICS_GFX_OK);
        optics_gfx_canvas *bc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = bs;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &bc) == OPTICS_GFX_OK);

        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(bs, &info);
        OT_EXPECT(info.color_space.primaries == OPTICS_GFX_PRIMARIES_BT2020);
        OT_EXPECT(info.color_space.transfer == OPTICS_GFX_TRANSFER_GAMMA);

        optics_gfx_mat3 to_2020;
        OT_EXPECT(optics_gfx_color_space_transform_matrix(
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB,
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_BT2020, &to_2020));
        optics_gfx_vec3 lin =
            optics_gfx_mat3_transform_vec3(to_2020, (optics_gfx_vec3){1.0f, 0.0f, 0.0f});
        int er = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_GAMMA, 2.4f, lin.x) *
                             255.0f);
        int eg = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_GAMMA, 2.4f, lin.y) *
                             255.0f);
        int eb = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_GAMMA, 2.4f, lin.z) *
                             255.0f);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(bs, bc, black, draw_red, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(bs, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], er, 4));
        OT_EXPECT(near_ch(centre[1], eg, 4));
        OT_EXPECT(near_ch(centre[2], eb, 4));

        optics_gfx_canvas_release(bc);
        optics_gfx_surface_release(bs);
    }

    /* --- 6. scRGB-linear offscreen: the transfer stage is provably off --- */
    {
        optics_gfx_color_space spaces[] = {(optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *ls = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &ls) == OPTICS_GFX_OK);
        optics_gfx_canvas *lc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = ls;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &lc) == OPTICS_GFX_OK);

        /* 50% white over black: 0.502 linear written verbatim to 8-bit
         * lands on 128 — the number the sRGB case 1 must NOT produce. */
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(ls, lc, black, draw_half_white, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(ls, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], 128, 3));

        optics_gfx_canvas_release(lc);
        optics_gfx_surface_release(ls);
    }

    /* --- 9. Output override: sRGB container, P3 content (legacy display path) --- */
    {
        optics_gfx_color_space want_swapchain[] = {
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SRGB};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = want_swapchain;
        csd.space_count = 1;
        optics_gfx_surface_output_color_desc ocd = OPTICS_GFX_SURFACE_OUTPUT_COLOR_DESC_INIT;
        ocd.content_space = (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_DISPLAY_P3;
        csd.next = &ocd;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *os = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &os) == OPTICS_GFX_OK);
        optics_gfx_canvas *oc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = os;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &oc) == OPTICS_GFX_OK);

        /* The container stays sRGB; the content space is the override. */
        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(os, &info);
        OT_EXPECT(info.color_space.primaries == OPTICS_GFX_PRIMARIES_BT709);
        OT_EXPECT(info.content_space.primaries == OPTICS_GFX_PRIMARIES_DISPLAY_P3);

        /* Pixels come out in the override space — same expectation as
         * the negotiated-P3 case 2. */
        optics_gfx_mat3 to_p3;
        OT_EXPECT(optics_gfx_color_space_transform_matrix(
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB,
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_DISPLAY_P3, &to_p3));
        optics_gfx_vec3 lin =
            optics_gfx_mat3_transform_vec3(to_p3, (optics_gfx_vec3){1.0f, 0.0f, 0.0f});
        int er =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.x) * 255.0f);
        int eg =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.y) * 255.0f);
        int eb =
            (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, lin.z) * 255.0f);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(os, oc, black, draw_red, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(os, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], er, 4));
        OT_EXPECT(near_ch(centre[1], eg, 4));
        OT_EXPECT(near_ch(centre[2], eb, 4));

        optics_gfx_canvas_release(oc);
        optics_gfx_surface_release(os);
    }

    /* --- 7. P3-tagged image: params-block decode into the working space --- */
    {
        /* A mid-tone P3 colour: far enough from the gamut corners that
         * the P3 -> BT.709 matrix changes it measurably. */
        const uint8_t content[4] = {60, 180, 120, 255}; /* P3-encoded RGB */
        optics_gfx_color_space p3 = OPTICS_GFX_COLOR_SPACE_DISPLAY_P3;
        optics_gfx_image_color_space_desc csd = OPTICS_GFX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.space = &p3;
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = content;
        optics_gfx_image *img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &img) == OPTICS_GFX_OK);

        /* Expected: sRGB-decode the encoded value, P3 -> working matrix,
         * re-encode for the sRGB surface. */
        optics_gfx_mat3 to_work;
        OT_EXPECT(optics_gfx_color_space_transform_matrix(
            p3, (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB, &to_work));
        int want[3];
        optics_gfx_vec3 straight = {
            optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, content[0] / 255.0f),
            optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, content[1] / 255.0f),
            optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, content[2] / 255.0f)};
        optics_gfx_vec3 work = optics_gfx_mat3_transform_vec3(to_work, straight);
        want[0] = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, work.x) *
                              255.0f);
        want[1] = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, work.y) *
                              255.0f);
        want[2] = (int)lrintf(optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, work.z) *
                              255.0f);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, img) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], want[0], 4));
        OT_EXPECT(near_ch(centre[1], want[1], 4));
        OT_EXPECT(near_ch(centre[2], want[2], 4));
        /* The matrix must actually change the colour (else the tag is a
         * no-op): untagged sRGB interpretation would read back verbatim. */
        OT_EXPECT(centre[0] != content[0] || centre[1] != content[1] || centre[2] != content[2]);
        optics_gfx_image_release(img);
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
        optics_gfx_icc_profile *srgb_icc = nullptr;
        OT_EXPECT(optics_gfx_icc_profile_create(profile, size, &srgb_icc) == OPTICS_GFX_OK);

        const uint8_t teal[4] = {17, 101, 149, 255};
        optics_gfx_image_color_space_desc csd = OPTICS_GFX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.icc = srgb_icc;
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = teal;
        optics_gfx_image *tagged = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &tagged) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, tagged) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], 17, 3));
        OT_EXPECT(near_ch(centre[1], 101, 3));
        OT_EXPECT(near_ch(centre[2], 149, 3));
        optics_gfx_image_release(tagged);

        /* 8b. LUT-profile image (constant D50-white CLUT) draws white. */
        size_t len = icc_build_mft1_constant_white(pdata, &defs[0]);
        size = icc_build_profile(profile, "mntr", "XYZ ", pdata, len, defs, 1);
        optics_gfx_icc_profile *lut_icc = nullptr;
        OT_EXPECT(optics_gfx_icc_profile_create(profile, size, &lut_icc) == OPTICS_GFX_OK);
        csd.icc = lut_icc;
        optics_gfx_image *lut_img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &lut_img) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, lut_img) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(centre[0] > 245 && centre[1] > 245 && centre[2] > 245);
        optics_gfx_image_release(lut_img);

        /* 8c. Identity LUT image: a snapshot keeps its old pixels after the
         * original is updated, proving color state and pixels COW together. */
        len = icc_build_mab_identity(pdata, &defs[0], 3, 3);
        size = icc_build_profile(profile, "mntr", "XYZ ", pdata, len, defs, 1);
        optics_gfx_icc_profile *mab_icc = nullptr;
        OT_EXPECT(optics_gfx_icc_profile_create(profile, size, &mab_icc) == OPTICS_GFX_OK);
        const uint8_t red[4] = {255, 0, 0, 255};
        const uint8_t blue[4] = {0, 0, 255, 255};
        csd.icc = mab_icc;
        idesc.initial_data = red;
        optics_gfx_image *mab_img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &mab_img) == OPTICS_GFX_OK);
        optics_gfx_image *mab_snapshot = nullptr;
        OT_EXPECT(optics_gfx_image_snapshot(mab_img, &mab_snapshot) == OPTICS_GFX_OK);
        OT_EXPECT(
            optics_gfx_image_update_region(
                mab_img,
                &(optics_gfx_image_update_region_opts){
                    .x = 0, .y = 0, .width = 1, .height = 1, .data = blue, .bytes = sizeof blue}) ==
            OPTICS_GFX_OK);

        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, mab_snapshot) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(centre[0] > 200 && centre[2] < 80);

        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, mab_img) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        centre = px_at(px, W / 2, H / 2);
        /* The synthetic mAB profile swaps the B/G basis; the point is that
         * the new version differs from the snapshot's red. */
        OT_EXPECT(centre[1] > 200 && centre[0] < 80);

        optics_gfx_image_release(mab_snapshot);
        optics_gfx_image_release(mab_img);
        optics_gfx_icc_profile_release(mab_icc);

        optics_gfx_icc_profile_release(srgb_icc);
        optics_gfx_icc_profile_release(lut_icc);
    }

    /* --- 9. BT.2020 PQ tagged image rendered onto sRGB surface: 203 nits maps to sRGB 1.0 white
     * --- */
    {
        /* 203 nits in ST 2084 PQ is ~0.5807 encoded (148/255). */
        uint8_t pq_white_203nits = (uint8_t)lrintf(
            optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_PQ, 0.0f, 203.0f / 10000.0f) * 255.0f);
        const uint8_t pq_content[4] = {pq_white_203nits, pq_white_203nits, pq_white_203nits, 255};
        optics_gfx_color_space pq_space = OPTICS_GFX_COLOR_SPACE_BT2020_PQ;
        optics_gfx_image_color_space_desc csd = OPTICS_GFX_IMAGE_COLOR_SPACE_DESC_INIT;
        csd.space = &pq_space;
        optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
        idesc.next = &csd;
        idesc.width = 1;
        idesc.height = 1;
        idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        idesc.initial_data = pq_content;
        optics_gfx_image *pq_img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &idesc, &pq_img) == OPTICS_GFX_OK);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(s, canvas, black, draw_image_full, pq_img) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        /* Must decode 203 nits to ~1.0 in working space and output as ~255 white, not ~38. */
        OT_EXPECT(centre[0] >= 250 && centre[1] >= 250 && centre[2] >= 250);
        optics_gfx_image_release(pq_img);
    }

    /* --- 10. BT.2020 PQ offscreen: deep container + PQ output encode --- */
    {
        optics_gfx_color_space spaces[] = {
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_BT2020_PQ};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *pqs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &pqs) == OPTICS_GFX_OK);
        optics_gfx_canvas *pqc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = pqs;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &pqc) == OPTICS_GFX_OK);

        /* The 8-bit era rejected this outright; now the container must be
         * deep (10-bit preferred, 16F fallback) and the info reports HDR. */
        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(pqs, &info);
        OT_EXPECT(info.color_space.primaries == OPTICS_GFX_PRIMARIES_BT2020);
        OT_EXPECT(info.content_space.transfer == OPTICS_GFX_TRANSFER_PQ);
        OT_EXPECT(info.hdr);
        OT_EXPECT(info.format == OPTICS_GFX_FORMAT_RGB10A2_UNORM ||
                  info.format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT);

        /* 50% white over black = 0.502 working-space linear; the output
         * transform maps it through the BT.2408 SDR-white scale and PQ
         * encodes. Readback quantizes the deep container back to 8-bit. */
        const float lin = 128.0f / 255.0f;
        int want = (int)lrintf(
            optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_PQ, 0.0f, lin * (203.0f / 10000.0f)) *
            255.0f);
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(pqs, pqc, black, draw_half_white, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(pqs, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], want, 4));
        OT_EXPECT(near_ch(centre[0], centre[1], 2) && near_ch(centre[1], centre[2], 2));
        /* PQ ~0.51 here: plainly not the SDR sRGB (~188) or linear (128)
         * numbers — the encode stage provably ran. */
        OT_EXPECT(centre[0] > 100 && centre[0] < 160);

        optics_gfx_canvas_release(pqc);
        optics_gfx_surface_release(pqs);
    }

    /* --- 11. explicit offscreen format list: sRGB in a 16F container --- */
    {
        optics_gfx_color_space spaces[] = {(optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SRGB};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        optics_gfx_format formats[] = {OPTICS_GFX_FORMAT_RGBA16_SFLOAT};
        optics_gfx_surface_offscreen_format_desc fsd =
            OPTICS_GFX_SURFACE_OFFSCREEN_FORMAT_DESC_INIT;
        fsd.formats = formats;
        fsd.format_count = 1;
        csd.next = &fsd;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *fs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &fs) == OPTICS_GFX_OK);
        optics_gfx_canvas *fc = nullptr;
        optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
        cd.surface = fs;
        OT_EXPECT(optics_gfx_canvas_create(&cd, &fc) == OPTICS_GFX_OK);

        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(fs, &info);
        OT_EXPECT(info.format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT);
        OT_EXPECT(!info.hdr);

        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        OT_EXPECT(render_frame(fs, fc, black, draw_red, nullptr) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(fs, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(near_ch(centre[0], 255, 2) && centre[1] < 3 && centre[2] < 3);

        optics_gfx_canvas_release(fc);
        optics_gfx_surface_release(fs);
    }

    /* --- 12. an unsuitable explicit format list is rejected --- */
    {
        /* PQ content cannot live in an 8-bit container. */
        optics_gfx_color_space spaces[] = {
            (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_BT2020_PQ};
        optics_gfx_surface_color_space_desc csd = OPTICS_GFX_SURFACE_COLOR_SPACE_DESC_INIT;
        csd.spaces = spaces;
        csd.space_count = 1;
        optics_gfx_format formats[] = {OPTICS_GFX_FORMAT_BGRA8_UNORM};
        optics_gfx_surface_offscreen_format_desc fsd =
            OPTICS_GFX_SURFACE_OFFSCREEN_FORMAT_DESC_INIT;
        fsd.formats = formats;
        fsd.format_count = 1;
        csd.next = &fsd;
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.next = &csd;
        sd.width = W;
        sd.height = H;
        optics_gfx_surface *rs = nullptr;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &rs) == OPTICS_GFX_ERROR_UNSUPPORTED);
        OT_EXPECT(rs == nullptr);
    }

    /* --- 13. v2 and v4 Lab PCS profiles render the same gray --- */
    {
        uint8_t profile[16384];
        uint8_t pdata[8192];
        icc_tag_def defs[8];

        size_t len = icc_build_mft2_constant_lab(pdata, &defs[0], 50.0, 0.0, 0.0, true);
        size_t size = icc_build_profile(profile, "mntr", "Lab ", pdata, len, defs, 1);
        profile[8] = 0x02; /* ICC v2: the 0xFF00-full-scale Lab encoding */
        optics_gfx_icc_profile *lab_v2 = nullptr;
        OT_EXPECT(optics_gfx_icc_profile_create(profile, size, &lab_v2) == OPTICS_GFX_OK);

        len = icc_build_mft2_constant_lab(pdata, &defs[0], 50.0, 0.0, 0.0, false);
        size = icc_build_profile(profile, "mntr", "Lab ", pdata, len, defs, 1);
        optics_gfx_icc_profile *lab_v4 = nullptr;
        OT_EXPECT(optics_gfx_icc_profile_create(profile, size, &lab_v4) == OPTICS_GFX_OK);

        /* L* 50 neutral: working-space Y ~0.184, sRGB-encoded ~119. */
        uint8_t got[2][3];
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        const optics_gfx_icc_profile *profiles[2] = {lab_v2, lab_v4};
        for (int i = 0; i < 2; ++i) {
            optics_gfx_image_color_space_desc csd = OPTICS_GFX_IMAGE_COLOR_SPACE_DESC_INIT;
            csd.icc = profiles[i];
            optics_gfx_image_desc idesc = OPTICS_GFX_IMAGE_DESC_INIT;
            idesc.next = &csd;
            idesc.width = 1;
            idesc.height = 1;
            idesc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
            static const uint8_t any[4] = {77, 88, 99, 255}; /* CLUT is constant */
            idesc.initial_data = any;
            optics_gfx_image *img = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &idesc, &img) == OPTICS_GFX_OK);
            OT_EXPECT(render_frame(s, canvas, black, draw_image_full, img) == OPTICS_GFX_OK);
            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            memcpy(got[i], px_at(px, W / 2, H / 2), 3);
            optics_gfx_image_release(img);
        }
        for (int ch = 0; ch < 3; ++ch) {
            OT_EXPECT(near_ch(got[0][ch], 119, 5));
            OT_EXPECT(near_ch(got[0][ch], got[1][ch], 2));
        }
        /* Neutral stays neutral. */
        OT_EXPECT(near_ch(got[0][0], got[0][1], 3) && near_ch(got[0][1], got[0][2], 3));

        optics_gfx_icc_profile_release(lab_v2);
        optics_gfx_icc_profile_release(lab_v4);
    }

    /* --- 14. Repeated LOAD passes with overlapping damage rects are bit-exact idempotent --- */
    {
        optics_gfx_surface_info info;
        optics_gfx_surface_get_info(s, &info);
        optics_gfx_color dark_desktop = optics_gfx_color_rgba(26, 27, 30, 255);
        for (uint32_t i = 0; i < info.image_count; ++i)
            OT_EXPECT(render_frame(s, canvas, dark_desktop, nullptr, nullptr) == OPTICS_GFX_OK);

        static uint8_t baseline[BYTES];
        memset(baseline, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, baseline, BYTES) == OPTICS_GFX_OK);

        const optics_gfx_rect overlapping_damage[2] = {
            {0.0f, 0.0f, 64.0f, 64.0f},
            {32.0f, 32.0f, 64.0f, 64.0f},
        };
        for (uint32_t pass_idx = 0; pass_idx < 8; ++pass_idx) {
            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = nullptr,
                                                          .damage =
                                                              {
                                                                  .rects = overlapping_damage,
                                                                  .count = 2,
                                                              },
                                                      }) == OPTICS_GFX_OK);
            optics_gfx_canvas_fill_rect_color(canvas, (optics_gfx_rect){0.0f, 0.0f, 8.0f, 8.0f},
                                              optics_gfx_color_rgba(255, 255, 255, 255));
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        }

        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        /* Every pixel in the overlapping LOAD region (48, 48) outside the 8x8
         * white box must remain bit-for-bit identical to the initial solid fill
         * with zero dither grain accumulation. */
        const uint8_t *orig = px_at(baseline, 48, 48);
        const uint8_t *after = px_at(px, 48, 48);
        OT_EXPECT(orig[0] == 26 && orig[1] == 27 && orig[2] == 30 && orig[3] == 255);
        OT_EXPECT(memcmp(orig, after, 4) == 0);
    }

    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
