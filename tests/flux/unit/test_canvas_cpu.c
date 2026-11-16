/*
 * Headless CPU-canvas rendering test. No Vulkan device or window: it exercises
 * the software backend (flux_canvas_backend_cpu) end to end — create, record,
 * read back pixels — across the solid, SDF rounded-rect, and gradient paths.
 */
#include "optics_test.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/canvas_helpers.h>
#include <flux/math.h>
#include <math.h>
#include <stddef.h>

/* Fetch an RGBA8 pixel from a premultiplied framebuffer. */
static void px(const uint8_t *fb, uint32_t stride, uint32_t x, uint32_t y, uint8_t out[4]) {
    const uint8_t *p = fb + (size_t)y * stride + (size_t)x * 4;
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
}

int main(void) {
    const uint32_t W = 64, H = 64;

    /* ---- Solid fill on an opaque black clear ---- */
    flux_canvas *c = nullptr;
    OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                     .backend = FLUX_CANVAS_BACKEND_CPU,
                                                     .scale = 1.0f,
                                                     .width = W,
                                                     .height = H},
                                 &c) == FLUX_OK);
    OT_EXPECT(c != nullptr);

    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    flux_color red = flux_color_rgba_premul(255, 0, 0, 255);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){16, 16, 32, 32}, red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr);
    OT_EXPECT(w == W && h == H && stride == W * 4);

    uint8_t p[4];
    px(fb, stride, 32, 32, p); /* inside the rect → red */
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 3, 3, p); /* outside → cleared black */
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 40, 40, p); /* still inside */
    OT_EXPECT(p[0] > 250 && p[3] > 250);

    /* ---- SDF rounded rect on a transparent clear: corners are cut ---- */
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    flux_canvas_fill_rrect(c, (flux_rect){0, 0, 64, 64}, 20.0f, white);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* centre → opaque white */
    OT_EXPECT(p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 250);
    px(fb, stride, 1, 1, p); /* rounded corner → transparent */
    OT_EXPECT(p[3] < 20);

    /* ---- Clip rect confines a fill ---- */
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 32, 64});
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, 64, 64}, red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 10, 32, p); /* inside clip → red */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 50, 32, p); /* outside clip → untouched black */
    OT_EXPECT(p[0] < 5 && p[3] > 250);

    /* ---- Shared triangle edge is covered once, not blended twice ---- */
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &(flux_color){}}) ==
              FLUX_OK);
    flux_color half_red = flux_color_rgba_premul(255, 0, 0, 128);
    flux_canvas_fill_rect_color(c, (flux_rect){16, 16, 32, 32}, half_red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* exactly on the quad's shared diagonal */
    OT_EXPECT(p[0] >= 126 && p[0] <= 130);
    OT_EXPECT(p[3] >= 126 && p[3] <= 130);
    px(fb, stride, 30, 32, p); /* ordinary interior sample has the same alpha */
    OT_EXPECT(p[0] >= 126 && p[0] <= 130);
    OT_EXPECT(p[3] >= 126 && p[3] <= 130);

    /* ---- Fixed SRC_OVER SDF does not inherit a preceding SRC paint ---- */
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_brush replace_red = flux_brush_solid(red);
    replace_red.blend = FLUX_BLEND_SRC;
    flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &replace_red);
    flux_color half_black = flux_color_rgba_premul(0, 0, 0, 128);
    flux_canvas_stroke_rrect(c, (flux_rect){16, 16, 32, 32}, 0.0f, half_black, 2.0f);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 16, 32, p); /* antialiased point on the left stroke */
    OT_EXPECT(p[0] > 100 && p[0] < 200);
    OT_EXPECT(p[1] < 5 && p[2] < 5 && p[3] > 250);

    flux_canvas_release(c);

    /* ---- Clip follows the current HiDPI transform and cannot expand ---- */
    OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                     .backend = FLUX_CANVAS_BACKEND_CPU,
                                                     .scale = 2.0f,
                                                     .width = W,
                                                     .height = H},
                                 &c) == FLUX_OK);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 16, 32});
    /* A later, larger clip intersects the first instead of replacing it. */
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 32, 32});
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, 32, 32}, red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 24, 32, p); /* logical x=12: inside transformed clip */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 40, 32, p); /* logical x=20: outside transformed clip */
    OT_EXPECT(p[0] < 5 && p[3] > 250);
    flux_canvas_release(c);

    /* ---- Linear gradient ---- */
    OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                     .backend = FLUX_CANVAS_BACKEND_CPU,
                                                     .scale = 1.0f,
                                                     .width = W,
                                                     .height = H},
                                 &c) == FLUX_OK);
    flux_gradient_stop stops[2] = {
        {0.0f, flux_color_rgba_premul(255, 0, 0, 255)},
        {1.0f, flux_color_rgba_premul(0, 0, 255, 255)},
    };
    flux_brush g =
        flux_brush_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 2);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_canvas_fill_rect(c, (flux_rect){0, 0, (float)W, (float)H}, &g);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 2, 32, p); /* left → red end */
    OT_EXPECT(p[0] > 200 && p[2] < 60);
    px(fb, stride, 61, 32, p); /* right → blue end */
    OT_EXPECT(p[2] > 200 && p[0] < 60);
    flux_canvas_release(c);

    /* Dimensions that overflow the supersample buffer are rejected before any
     * allocation or wrapped size calculation. */
    c = nullptr;
    OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                     .backend = FLUX_CANVAS_BACKEND_CPU,
                                                     .scale = 1.0f,
                                                     .width = UINT32_MAX,
                                                     .height = 1},
                                 &c) == FLUX_ERROR_OUT_OF_RANGE);
    OT_EXPECT(c == nullptr);

    /* ---- Unified factory + pass API (Skia SkSurface-style) ---- */
    flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
    d.backend = FLUX_CANVAS_BACKEND_CPU;
    d.width = W;
    d.height = H;
    d.scale = 1.0f;
    OT_EXPECT(flux_canvas_create(&d, &c) == FLUX_OK);
    OT_EXPECT(flux_canvas_begin(c, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                            .frame = nullptr,
                                                            .clear_color = &black}) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, (float)W, (float)H}, red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    /* Descriptor LOAD preserves the previous pass. */
    flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    flux_color blue = flux_color_rgba_premul(0, 0, 255, 255);
    pd.clear_color = &blue;
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    /* Tagged pNext extensions keep ordinary CPU operations valid. */
    pd.clear_color = &black;
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){8, 8, 16, 16}, red);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_ERROR_INVALID_STATE);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 12, 12, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    /* EVEN_ODD used to be unrepresentable here: the ear-clip path computes
     * non-zero winding, so a no-stencil pass had to diagnose and drop the draw.
     * Since ADR-0119 S2 a solid SRC_OVER fill is answered by analytic coverage,
     * whose winding is accumulated from sorted crossings — so even-odd works
     * with no stencil attachment at all, and this asserts the capability rather
     * than the old refusal.
     *
     * Two nested same-winding squares is the case that separates the rules:
     * non-zero fills both, even-odd leaves the inner one empty. */
    flux_arena arena;
    OT_EXPECT(flux_arena_init(&arena, 4096, nullptr) == FLUX_OK);
    flux_path *eo_path = nullptr;
    OT_EXPECT(flux_path_create(&eo_path, &arena) == FLUX_OK);
    flux_path_add_rect(eo_path, (flux_rect){8, 8, 24, 24});
    flux_path_add_rect(eo_path, (flux_rect){14, 14, 12, 12});
    flux_brush eo = flux_brush_solid(white);
    pd.clear_color = &black;
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    flux_canvas_fill_path(c, eo_path, FLUX_FILL_EVEN_ODD, &eo);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 11, 20, p); /* between the squares: painted */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 20, 20, p); /* inside the inner square: the hole */
    OT_EXPECT(p[0] < 5 && p[3] > 250);
    /* Non-zero paints the same path solid, inner square included. */
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    flux_canvas_fill_path(c, eo_path, FLUX_FILL_NON_ZERO, &eo);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 20, 20, p);
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    /* The sticky error was consumed by end; the next pass starts clean. */
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    flux_arena_deinit(&arena);
    pd.next = nullptr;

    /* Restore the blue baseline used by the partial-clear preservation check. */
    pd.clear_color = &blue;
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);

    /* A damage scope constrains both the attachment clear and the initial draw
     * scissor. Pixels outside it retain the previous blue pass. */
    const flux_rect dirty = {16, 20, 24, 12};
    pd.clear_color = &red;
    pd.damage = (flux_damage){.rects = &dirty, .count = 1};
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_OK);
    OT_EXPECT(flux_canvas_end(c) == FLUX_OK);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 20, 24, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 8, 24, p);
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    /* A damage rect entirely outside the framebuffer names no pixel at all, so
     * the pass is rejected rather than silently painting nothing. */
    const flux_rect outside = {W + 4, 0, 2, 4};
    pd.damage = (flux_damage){.rects = &outside, .count = 1};
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_ERROR_INVALID_ARGUMENT);

    /* A count with no array is a malformed descriptor, not "the whole extent". */
    pd.damage = (flux_damage){.rects = nullptr, .count = 3};
    pd.frame = nullptr;
    OT_EXPECT(flux_canvas_begin(c, &pd) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_canvas_release(c);

    /* ---- Analytic path fill/stroke (>2048 pts), sloped AA, EVEN_ODD, and blends ---- */
    {
        const uint32_t CW = 96, CH = 96;
        flux_canvas *ac = nullptr;
        OT_EXPECT(flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC,
                                                         .backend = FLUX_CANVAS_BACKEND_CPU,
                                                         .scale = 1.0f,
                                                         .width = CW,
                                                         .height = CH},
                                     &ac) == FLUX_OK);
        flux_arena arena;
        OT_EXPECT(flux_arena_init(&arena, 256u * 1024u, nullptr) == FLUX_OK);

        /* 1 & 2. >2048-vertex circle fill and >2048-vertex ring stroke. */
        flux_path *many = nullptr;
        OT_EXPECT(flux_path_create(&many, &arena) == FLUX_OK);
        const uint32_t N = 2600;
        for (uint32_t i = 0; i < N; ++i) {
            float ang = (float)i * (6.28318530718f / (float)N);
            float x = 48.0f + 30.0f * cosf(ang);
            float y = 48.0f + 30.0f * sinf(ang);
            if (i == 0)
                flux_path_move_to(many, x, y);
            else
                flux_path_line_to(many, x, y);
        }
        flux_path_close(many);

        flux_set_diagnostic(&(flux_error_info){.code = FLUX_OK});
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
                  FLUX_OK);
        flux_geometry fg = {.kind = FLUX_GEOM_PATH,
                            .path = {.path = many, .fill_rule = FLUX_FILL_NON_ZERO}};
        flux_brush rb = flux_brush_solid(flux_color_rgba_premul(255, 0, 0, 255));
        flux_canvas_draw_geometry(ac, &(flux_canvas_draw_geometry_opts){.geom = &fg, .brush = &rb});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        flux_error_info err = {};
        flux_get_last_error(&err);
        OT_EXPECT(err.code == FLUX_OK);

        uint32_t aw = 0, ah = 0, ast = 0;
        const uint8_t *afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        OT_EXPECT(afb != nullptr);
        uint8_t cp[4];
        px(afb, ast, 48, 48, cp); /* inside 2600-pt fill */
        OT_EXPECT(cp[0] > 250 && cp[1] < 5 && cp[2] < 5);
        px(afb, ast, 4, 4, cp); /* outside */
        OT_EXPECT(cp[0] < 5 && cp[3] > 250);

        flux_set_diagnostic(&(flux_error_info){.code = FLUX_OK});
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
                  FLUX_OK);
        flux_geometry sg = {.kind = FLUX_GEOM_PATH, .path = {.path = many}};
        sg.stroke.width = 4.0f;
        flux_canvas_draw_geometry(ac, &(flux_canvas_draw_geometry_opts){.geom = &sg, .brush = &rb});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        flux_get_last_error(&err);
        OT_EXPECT(err.code == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 78, 48, cp); /* on ring stroke at (48+30, 48) */
        OT_EXPECT(cp[0] > 240);
        px(afb, ast, 48, 48, cp); /* center is hollow */
        OT_EXPECT(cp[0] < 5);

        /* 3. Sloped stroke anti-aliasing: >16 distinct red grey levels. */
        flux_path *diag = nullptr;
        OT_EXPECT(flux_path_create(&diag, &arena) == FLUX_OK);
        flux_path_move_to(diag, 8.0f, 12.0f);
        flux_path_line_to(diag, 88.0f, 80.0f);
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
                  FLUX_OK);
        flux_geometry dg = {.kind = FLUX_GEOM_PATH, .path = {.path = diag}};
        dg.stroke.width = 2.0f;
        flux_canvas_draw_geometry(ac, &(flux_canvas_draw_geometry_opts){.geom = &dg, .brush = &rb});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        int seen_lvl[256] = {};
        for (uint32_t yy = 0; yy < CH; ++yy) {
            for (uint32_t xx = 0; xx < CW; ++xx) {
                uint8_t rch = afb[(size_t)yy * ast + (size_t)xx * 4];
                seen_lvl[rch] = 1;
            }
        }
        int partial = 0;
        for (int lvl = 1; lvl < 255; ++lvl)
            partial += seen_lvl[lvl];
        OT_EXPECT(partial > 16);

        /* 4. Self-intersecting 5-point star: EVEN_ODD vs NON_ZERO center winding. */
        flux_path *star = nullptr;
        OT_EXPECT(flux_path_create(&star, &arena) == FLUX_OK);
        const int order[5] = {0, 2, 4, 1, 3};
        for (int i = 0; i < 5; ++i) {
            float a = -1.5707963f + (float)order[i] * (6.28318530718f / 5.0f);
            float sx = 48.0f + 36.0f * cosf(a);
            float sy = 48.0f + 36.0f * sinf(a);
            if (i == 0)
                flux_path_move_to(star, sx, sy);
            else
                flux_path_line_to(star, sx, sy);
        }
        flux_path_close(star);

        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
                  FLUX_OK);
        flux_geometry star_eo = {.kind = FLUX_GEOM_PATH,
                                 .path = {.path = star, .fill_rule = FLUX_FILL_EVEN_ODD}};
        flux_canvas_draw_geometry(
            ac, &(flux_canvas_draw_geometry_opts){.geom = &star_eo, .brush = &rb});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 48, 48, cp); /* center of star hollow under EVEN_ODD */
        OT_EXPECT(cp[0] < 5);

        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &black}) ==
                  FLUX_OK);
        flux_geometry star_nz = {.kind = FLUX_GEOM_PATH,
                                 .path = {.path = star, .fill_rule = FLUX_FILL_NON_ZERO}};
        flux_canvas_draw_geometry(
            ac, &(flux_canvas_draw_geometry_opts){.geom = &star_nz, .brush = &rb});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 48, 48, cp); /* center of star filled under NON_ZERO */
        OT_EXPECT(cp[0] > 250);

        /* 5. PLUS, MULTIPLY, and SRC blend modes on analytic path fills. */
        flux_color yellow = flux_color_rgba_premul(180, 180, 0, 255);
        flux_path *sq = nullptr;
        OT_EXPECT(flux_path_create(&sq, &arena) == FLUX_OK);
        flux_path_add_rect(sq, (flux_rect){10.0f, 10.0f, 30.0f, 30.0f});
        flux_geometry sqg = {.kind = FLUX_GEOM_PATH, .path = {.path = sq}};

        /* PLUS blue over yellow -> whiteish/bright RGB */
        flux_brush b_plus = flux_brush_solid(flux_color_rgba_premul(0, 0, 255, 255));
        b_plus.blend = FLUX_BLEND_PLUS;
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &yellow}) ==
                  FLUX_OK);
        flux_canvas_draw_geometry(
            ac, &(flux_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_plus});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] > 170 && cp[1] > 170 && cp[2] > 250);

        /* MULTIPLY red over yellow -> keeps R, zeroes G */
        flux_brush b_mul = flux_brush_solid(flux_color_rgba_premul(255, 0, 0, 255));
        b_mul.blend = FLUX_BLEND_MULTIPLY;
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &yellow}) ==
                  FLUX_OK);
        flux_canvas_draw_geometry(ac,
                                  &(flux_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_mul});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] > 170 && cp[0] < 190 && cp[1] < 5 && cp[2] < 5);

        /* SRC semi-transparent cyan over yellow -> overwrites yellow cleanly */
        flux_brush b_src = flux_brush_solid(flux_color_rgba_premul(0, 128, 128, 128));
        b_src.blend = FLUX_BLEND_SRC;
        OT_EXPECT(flux_canvas_begin(ac, &(flux_canvas_pass_desc){.type = FLUX_TYPE_CANVAS_PASS_DESC,
                                                                 .clear_color = &yellow}) ==
                  FLUX_OK);
        flux_canvas_draw_geometry(ac,
                                  &(flux_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_src});
        OT_EXPECT(flux_canvas_end(ac) == FLUX_OK);
        afb = flux_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] < 5 && cp[1] > 50 && cp[1] == cp[2] && cp[3] > 110 && cp[3] < 145);

        flux_arena_deinit(&arena);
        flux_canvas_release(ac);
    }

    return OT_REPORT();
}