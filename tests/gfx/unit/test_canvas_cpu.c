/*
 * Headless CPU-canvas rendering test. No Vulkan device or window: it exercises
 * the software backend (flux_canvas_backend_cpu) end to end — create, record,
 * read back pixels — across the solid, SDF rounded-rect, and gradient paths.
 */
#include "optics_test.h"
#include <math.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/canvas_helpers.h>
#include <optics/gfx/math.h>
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
    optics_gfx_canvas *c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);
    OT_EXPECT(c != nullptr);

    optics_gfx_color black = optics_gfx_color_rgba_premul(0, 0, 0, 255);
    optics_gfx_color red = optics_gfx_color_rgba_premul(255, 0, 0, 255);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){16, 16, 32, 32}, red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
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
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_color white = optics_gfx_color_rgba_premul(255, 255, 255, 255);
    optics_gfx_canvas_fill_rrect(c, (optics_gfx_rect){0, 0, 64, 64}, 20.0f, white);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* centre → opaque white */
    OT_EXPECT(p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 250);
    px(fb, stride, 1, 1, p); /* rounded corner → transparent */
    OT_EXPECT(p[3] < 20);

    /* ---- Clip rect confines a fill ---- */
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 32, 64});
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){0, 0, 64, 64}, red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 10, 32, p); /* inside clip → red */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 50, 32, p); /* outside clip → untouched black */
    OT_EXPECT(p[0] < 5 && p[3] > 250);

    /* ---- Shared triangle edge is covered once, not blended twice ---- */
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &(optics_gfx_color){}}) ==
              OPTICS_GFX_OK);
    optics_gfx_color half_red = optics_gfx_color_rgba_premul(255, 0, 0, 128);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){16, 16, 32, 32}, half_red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* exactly on the quad's shared diagonal */
    OT_EXPECT(p[0] >= 126 && p[0] <= 130);
    OT_EXPECT(p[3] >= 126 && p[3] <= 130);
    px(fb, stride, 30, 32, p); /* ordinary interior sample has the same alpha */
    OT_EXPECT(p[0] >= 126 && p[0] <= 130);
    OT_EXPECT(p[3] >= 126 && p[3] <= 130);

    /* ---- Fixed SRC_OVER SDF does not inherit a preceding SRC paint ---- */
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_brush replace_red = optics_gfx_brush_solid(red);
    replace_red.blend = OPTICS_GFX_BLEND_SRC;
    optics_gfx_canvas_fill_rect(c, (optics_gfx_rect){0, 0, W, H}, &replace_red);
    optics_gfx_color half_black = optics_gfx_color_rgba_premul(0, 0, 0, 128);
    optics_gfx_canvas_stroke_rrect(c, (optics_gfx_rect){16, 16, 32, 32}, 0.0f, half_black, 2.0f);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 16, 32, p); /* antialiased point on the left stroke */
    OT_EXPECT(p[0] > 100 && p[0] < 200);
    OT_EXPECT(p[1] < 5 && p[2] < 5 && p[3] > 250);

    optics_gfx_canvas_release(c);

    /* ---- Clip follows the current HiDPI transform and cannot expand ---- */
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 2.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 16, 32});
    /* A later, larger clip intersects the first instead of replacing it. */
    optics_gfx_canvas_clip_rect(c, (optics_gfx_rect){0, 0, 32, 32});
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){0, 0, 32, 32}, red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 24, 32, p); /* logical x=12: inside transformed clip */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 40, 32, p); /* logical x=20: outside transformed clip */
    OT_EXPECT(p[0] < 5 && p[3] > 250);
    optics_gfx_canvas_release(c);

    /* ---- Linear gradient ---- */
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = W,
                                                           .height = H},
                                 &c) == OPTICS_GFX_OK);
    optics_gfx_gradient_stop stops[2] = {
        {0.0f, optics_gfx_color_rgba_premul(255, 0, 0, 255)},
        {1.0f, optics_gfx_color_rgba_premul(0, 0, 255, 255)},
    };
    optics_gfx_brush g = optics_gfx_brush_linear_gradient(
        (optics_gfx_point){0, 0}, (optics_gfx_point){(float)W, 0}, stops, 2);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_rect(c, (optics_gfx_rect){0, 0, (float)W, (float)H}, &g);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 2, 32, p); /* left → red end */
    OT_EXPECT(p[0] > 200 && p[2] < 60);
    px(fb, stride, 61, 32, p); /* right → blue end */
    OT_EXPECT(p[2] > 200 && p[0] < 60);
    optics_gfx_canvas_release(c);

    /* Dimensions that overflow the supersample buffer are rejected before any
     * allocation or wrapped size calculation. */
    c = nullptr;
    OT_EXPECT(
        optics_gfx_canvas_create(&(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                           .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                           .scale = 1.0f,
                                                           .width = UINT32_MAX,
                                                           .height = 1},
                                 &c) == OPTICS_GFX_ERROR_OUT_OF_RANGE);
    OT_EXPECT(c == nullptr);

    /* ---- Unified factory + pass API (Skia SkSurface-style) ---- */
    optics_gfx_canvas_desc d = OPTICS_GFX_CANVAS_DESC_INIT;
    d.backend = OPTICS_GFX_CANVAS_BACKEND_CPU;
    d.width = W;
    d.height = H;
    d.scale = 1.0f;
    OT_EXPECT(optics_gfx_canvas_create(&d, &c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_begin(
                  c, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                    .frame = nullptr,
                                                    .clear_color = &black}) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){0, 0, (float)W, (float)H}, red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    OT_EXPECT(fb != nullptr && w == W && h == H);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    /* Descriptor LOAD preserves the previous pass. */
    optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    optics_gfx_color blue = optics_gfx_color_rgba_premul(0, 0, 255, 255);
    pd.clear_color = &blue;
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    /* Tagged pNext extensions keep ordinary CPU operations valid. */
    pd.clear_color = &black;
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_rect_color(c, (optics_gfx_rect){8, 8, 16, 16}, red);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_ERROR_INVALID_STATE);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
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
    optics_gfx_arena arena;
    OT_EXPECT(optics_gfx_arena_init(&arena, 4096, nullptr) == OPTICS_GFX_OK);
    optics_gfx_path *eo_path = nullptr;
    OT_EXPECT(optics_gfx_path_create(&eo_path, &arena) == OPTICS_GFX_OK);
    optics_gfx_path_add_rect(eo_path, (optics_gfx_rect){8, 8, 24, 24});
    optics_gfx_path_add_rect(eo_path, (optics_gfx_rect){14, 14, 12, 12});
    optics_gfx_brush eo = optics_gfx_brush_solid(white);
    pd.clear_color = &black;
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_path(c, eo_path, OPTICS_GFX_FILL_EVEN_ODD, &eo);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 11, 20, p); /* between the squares: painted */
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 20, 20, p); /* inside the inner square: the hole */
    OT_EXPECT(p[0] < 5 && p[3] > 250);
    /* Non-zero paints the same path solid, inner square included. */
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    optics_gfx_canvas_fill_path(c, eo_path, OPTICS_GFX_FILL_NON_ZERO, &eo);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 20, 20, p);
    OT_EXPECT(p[0] > 250 && p[3] > 250);
    /* The sticky error was consumed by end; the next pass starts clean. */
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    optics_gfx_arena_deinit(&arena);
    pd.next = nullptr;

    /* Restore the blue baseline used by the partial-clear preservation check. */
    pd.clear_color = &blue;
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);

    /* A damage scope constrains both the attachment clear and the initial draw
     * scissor. Pixels outside it retain the previous blue pass. */
    const optics_gfx_rect dirty = {16, 20, 24, 12};
    pd.clear_color = &red;
    pd.damage = (optics_gfx_damage){.rects = &dirty, .count = 1};
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_canvas_end(c) == OPTICS_GFX_OK);
    fb = optics_gfx_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 20, 24, p);
    OT_EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 8, 24, p);
    OT_EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    /* A damage rect entirely outside the framebuffer names no pixel at all, so
     * the pass is rejected rather than silently painting nothing. */
    const optics_gfx_rect outside = {W + 4, 0, 2, 4};
    pd.damage = (optics_gfx_damage){.rects = &outside, .count = 1};
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    /* A count with no array is a malformed descriptor, not "the whole extent". */
    pd.damage = (optics_gfx_damage){.rects = nullptr, .count = 3};
    pd.frame = nullptr;
    OT_EXPECT(optics_gfx_canvas_begin(c, &pd) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_canvas_release(c);

    /* ---- Analytic path fill/stroke (>2048 pts), sloped AA, EVEN_ODD, and blends ---- */
    {
        const uint32_t CW = 96, CH = 96;
        optics_gfx_canvas *ac = nullptr;
        OT_EXPECT(optics_gfx_canvas_create(
                      &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC,
                                                .backend = OPTICS_GFX_CANVAS_BACKEND_CPU,
                                                .scale = 1.0f,
                                                .width = CW,
                                                .height = CH},
                      &ac) == OPTICS_GFX_OK);
        optics_gfx_arena arena;
        OT_EXPECT(optics_gfx_arena_init(&arena, 256u * 1024u, nullptr) == OPTICS_GFX_OK);

        /* 1 & 2. >2048-vertex circle fill and >2048-vertex ring stroke. */
        optics_gfx_path *many = nullptr;
        OT_EXPECT(optics_gfx_path_create(&many, &arena) == OPTICS_GFX_OK);
        const uint32_t N = 2600;
        for (uint32_t i = 0; i < N; ++i) {
            float ang = (float)i * (6.28318530718f / (float)N);
            float x = 48.0f + 30.0f * cosf(ang);
            float y = 48.0f + 30.0f * sinf(ang);
            if (i == 0)
                optics_gfx_path_move_to(many, x, y);
            else
                optics_gfx_path_line_to(many, x, y);
        }
        optics_gfx_path_close(many);

        optics_gfx_set_diagnostic(&(optics_gfx_error_info){.code = OPTICS_GFX_OK});
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_geometry fg = {.kind = OPTICS_GFX_GEOM_PATH,
                                  .path = {.path = many, .fill_rule = OPTICS_GFX_FILL_NON_ZERO}};
        optics_gfx_brush rb = optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 0, 0, 255));
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &fg, .brush = &rb});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        optics_gfx_error_info err = {};
        optics_gfx_get_last_error(&err);
        OT_EXPECT(err.code == OPTICS_GFX_OK);

        uint32_t aw = 0, ah = 0, ast = 0;
        const uint8_t *afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        OT_EXPECT(afb != nullptr);
        uint8_t cp[4];
        px(afb, ast, 48, 48, cp); /* inside 2600-pt fill */
        OT_EXPECT(cp[0] > 250 && cp[1] < 5 && cp[2] < 5);
        px(afb, ast, 4, 4, cp); /* outside */
        OT_EXPECT(cp[0] < 5 && cp[3] > 250);

        optics_gfx_set_diagnostic(&(optics_gfx_error_info){.code = OPTICS_GFX_OK});
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_geometry sg = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = many}};
        sg.stroke.width = 4.0f;
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &sg, .brush = &rb});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        optics_gfx_get_last_error(&err);
        OT_EXPECT(err.code == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 78, 48, cp); /* on ring stroke at (48+30, 48) */
        OT_EXPECT(cp[0] > 240);
        px(afb, ast, 48, 48, cp); /* center is hollow */
        OT_EXPECT(cp[0] < 5);

        /* 3. Sloped stroke anti-aliasing: >16 distinct red grey levels. */
        optics_gfx_path *diag = nullptr;
        OT_EXPECT(optics_gfx_path_create(&diag, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_move_to(diag, 8.0f, 12.0f);
        optics_gfx_path_line_to(diag, 88.0f, 80.0f);
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_geometry dg = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = diag}};
        dg.stroke.width = 2.0f;
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &dg, .brush = &rb});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
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
        optics_gfx_path *star = nullptr;
        OT_EXPECT(optics_gfx_path_create(&star, &arena) == OPTICS_GFX_OK);
        const int order[5] = {0, 2, 4, 1, 3};
        for (int i = 0; i < 5; ++i) {
            float a = -1.5707963f + (float)order[i] * (6.28318530718f / 5.0f);
            float sx = 48.0f + 36.0f * cosf(a);
            float sy = 48.0f + 36.0f * sinf(a);
            if (i == 0)
                optics_gfx_path_move_to(star, sx, sy);
            else
                optics_gfx_path_line_to(star, sx, sy);
        }
        optics_gfx_path_close(star);

        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_geometry star_eo = {
            .kind = OPTICS_GFX_GEOM_PATH,
            .path = {.path = star, .fill_rule = OPTICS_GFX_FILL_EVEN_ODD}};
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &star_eo, .brush = &rb});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 48, 48, cp); /* center of star hollow under EVEN_ODD */
        OT_EXPECT(cp[0] < 5);

        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &black}) == OPTICS_GFX_OK);
        optics_gfx_geometry star_nz = {
            .kind = OPTICS_GFX_GEOM_PATH,
            .path = {.path = star, .fill_rule = OPTICS_GFX_FILL_NON_ZERO}};
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &star_nz, .brush = &rb});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 48, 48, cp); /* center of star filled under NON_ZERO */
        OT_EXPECT(cp[0] > 250);

        /* 5. PLUS, MULTIPLY, and SRC blend modes on analytic path fills. */
        optics_gfx_color yellow = optics_gfx_color_rgba_premul(180, 180, 0, 255);
        optics_gfx_path *sq = nullptr;
        OT_EXPECT(optics_gfx_path_create(&sq, &arena) == OPTICS_GFX_OK);
        optics_gfx_path_add_rect(sq, (optics_gfx_rect){10.0f, 10.0f, 30.0f, 30.0f});
        optics_gfx_geometry sqg = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = sq}};

        /* PLUS blue over yellow -> whiteish/bright RGB */
        optics_gfx_brush b_plus =
            optics_gfx_brush_solid(optics_gfx_color_rgba_premul(0, 0, 255, 255));
        b_plus.blend = OPTICS_GFX_BLEND_PLUS;
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &yellow}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_plus});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] > 170 && cp[1] > 170 && cp[2] > 250);

        /* MULTIPLY red over yellow -> keeps R, zeroes G */
        optics_gfx_brush b_mul =
            optics_gfx_brush_solid(optics_gfx_color_rgba_premul(255, 0, 0, 255));
        b_mul.blend = OPTICS_GFX_BLEND_MULTIPLY;
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &yellow}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_mul});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] > 170 && cp[0] < 190 && cp[1] < 5 && cp[2] < 5);

        /* SRC semi-transparent cyan over yellow -> overwrites yellow cleanly */
        optics_gfx_brush b_src =
            optics_gfx_brush_solid(optics_gfx_color_rgba_premul(0, 128, 128, 128));
        b_src.blend = OPTICS_GFX_BLEND_SRC;
        OT_EXPECT(optics_gfx_canvas_begin(
                      ac, &(optics_gfx_canvas_pass_desc){.type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                         .clear_color = &yellow}) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_geometry(
            ac, &(optics_gfx_canvas_draw_geometry_opts){.geom = &sqg, .brush = &b_src});
        OT_EXPECT(optics_gfx_canvas_end(ac) == OPTICS_GFX_OK);
        afb = optics_gfx_canvas_cpu_pixels(ac, &aw, &ah, &ast);
        px(afb, ast, 25, 25, cp);
        OT_EXPECT(cp[0] < 5 && cp[1] > 50 && cp[1] == cp[2] && cp[3] > 110 && cp[3] < 145);

        optics_gfx_arena_deinit(&arena);
        optics_gfx_canvas_release(ac);
    }

    return OT_REPORT();
}