/*
 * Headless CPU-canvas rendering test. No Vulkan device or window: it exercises
 * the software backend (flux_canvas_backend_cpu) end to end — create, record,
 * read back pixels — across the solid, SDF rounded-rect, and gradient paths.
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/canvas_cpu.h>
#include <flux/math.h>
#include <stddef.h>

/* The no-stencil option is a pNext extension specifically so an application
 * compiled with the pre-extension descriptor remains ABI-compatible with a
 * newer libflux. Keep this executable assertion alongside the API tests. */
typedef struct legacy_flux_canvas_pass_desc {
    flux_struct_type type;
    const void *next;
    const flux_color *clear_color;
    flux_canvas_antialias antialias;
    int32_t render_offset_x;
    int32_t render_offset_y;
    uint32_t render_width;
    uint32_t render_height;
} legacy_flux_canvas_pass_desc;

_Static_assert(sizeof(flux_canvas_pass_desc) == sizeof(legacy_flux_canvas_pass_desc),
               "flux_canvas_pass_desc ABI changed");
#define ASSERT_PASS_DESC_FIELD(field)                                                           \
    _Static_assert(offsetof(flux_canvas_pass_desc, field) ==                                    \
                       offsetof(legacy_flux_canvas_pass_desc, field),                           \
                   "flux_canvas_pass_desc field layout changed: " #field)
ASSERT_PASS_DESC_FIELD(type);
ASSERT_PASS_DESC_FIELD(next);
ASSERT_PASS_DESC_FIELD(clear_color);
ASSERT_PASS_DESC_FIELD(antialias);
ASSERT_PASS_DESC_FIELD(render_offset_x);
ASSERT_PASS_DESC_FIELD(render_offset_y);
ASSERT_PASS_DESC_FIELD(render_width);
ASSERT_PASS_DESC_FIELD(render_height);
#undef ASSERT_PASS_DESC_FIELD

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
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    EXPECT(c != nullptr);

    flux_color black = flux_color_rgba_premul(0, 0, 0, 255);
    flux_color red = flux_color_rgba_premul(255, 0, 0, 255);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){16, 16, 32, 32}, red);
    flux_canvas_cpu_end(c);

    uint32_t w = 0, h = 0, stride = 0;
    const uint8_t *fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr);
    EXPECT(w == W && h == H && stride == W * 4);

    uint8_t p[4];
    px(fb, stride, 32, 32, p); /* inside the rect → red */
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 3, 3, p); /* outside → cleared black */
    EXPECT(p[0] < 5 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 40, 40, p); /* still inside */
    EXPECT(p[0] > 250 && p[3] > 250);

    /* ---- SDF rounded rect on a transparent clear: corners are cut ---- */
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_color white = flux_color_rgba_premul(255, 255, 255, 255);
    flux_canvas_fill_rrect(c, (flux_rect){0, 0, 64, 64}, 20.0f, white);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* centre → opaque white */
    EXPECT(p[0] > 250 && p[1] > 250 && p[2] > 250 && p[3] > 250);
    px(fb, stride, 1, 1, p); /* rounded corner → transparent */
    EXPECT(p[3] < 20);

    /* ---- Clip rect confines a fill ---- */
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 32, 64});
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, 64, 64}, red);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 10, 32, p); /* inside clip → red */
    EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 50, 32, p); /* outside clip → untouched black */
    EXPECT(p[0] < 5 && p[3] > 250);

    /* ---- Shared triangle edge is covered once, not blended twice ---- */
    EXPECT(flux_canvas_cpu_begin(c, nullptr) == FLUX_OK);
    flux_color half_red = flux_color_rgba_premul(255, 0, 0, 128);
    flux_canvas_fill_rect_color(c, (flux_rect){16, 16, 32, 32}, half_red);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p); /* exactly on the quad's shared diagonal */
    EXPECT(p[0] >= 126 && p[0] <= 130);
    EXPECT(p[3] >= 126 && p[3] <= 130);
    px(fb, stride, 30, 32, p); /* ordinary interior sample has the same alpha */
    EXPECT(p[0] >= 126 && p[0] <= 130);
    EXPECT(p[3] >= 126 && p[3] <= 130);

    /* ---- Fixed SRC_OVER SDF does not inherit a preceding SRC paint ---- */
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_paint replace_red = flux_paint_solid(red);
    replace_red.blend = FLUX_BLEND_SRC;
    flux_canvas_fill_rect(c, (flux_rect){0, 0, W, H}, &replace_red);
    flux_color half_black = flux_color_rgba_premul(0, 0, 0, 128);
    flux_canvas_stroke_rrect(c, (flux_rect){16, 16, 32, 32}, 0.0f, half_black, 2.0f);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 16, 32, p); /* antialiased point on the left stroke */
    EXPECT(p[0] > 100 && p[0] < 200);
    EXPECT(p[1] < 5 && p[2] < 5 && p[3] > 250);

    flux_canvas_destroy(c);

    /* ---- Clip follows the current HiDPI transform and cannot expand ---- */
    EXPECT(flux_canvas_create_cpu(W, H, 2.0f, &c) == FLUX_OK);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 16, 32});
    /* A later, larger clip intersects the first instead of replacing it. */
    flux_canvas_clip_rect(c, (flux_rect){0, 0, 32, 32});
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, 32, 32}, red);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 24, 32, p); /* logical x=12: inside transformed clip */
    EXPECT(p[0] > 250 && p[3] > 250);
    px(fb, stride, 40, 32, p); /* logical x=20: outside transformed clip */
    EXPECT(p[0] < 5 && p[3] > 250);
    flux_canvas_destroy(c);

    /* ---- Linear gradient ---- */
    EXPECT(flux_canvas_create_cpu(W, H, 1.0f, &c) == FLUX_OK);
    flux_gradient_stop stops[2] = {
        {0.0f, flux_color_rgba_premul(255, 0, 0, 255)},
        {1.0f, flux_color_rgba_premul(0, 0, 255, 255)},
    };
    flux_paint g =
        flux_paint_linear_gradient((flux_point){0, 0}, (flux_point){(float)W, 0}, stops, 2);
    EXPECT(flux_canvas_cpu_begin(c, &black) == FLUX_OK);
    flux_canvas_fill_rect(c, (flux_rect){0, 0, (float)W, (float)H}, &g);
    flux_canvas_cpu_end(c);
    fb = flux_canvas_cpu_pixels(c, &w, &h, &stride);
    px(fb, stride, 2, 32, p); /* left → red end */
    EXPECT(p[0] > 200 && p[2] < 60);
    px(fb, stride, 61, 32, p); /* right → blue end */
    EXPECT(p[2] > 200 && p[0] < 60);
    flux_canvas_destroy(c);

    /* Dimensions that overflow the supersample buffer are rejected before any
     * allocation or wrapped size calculation. */
    c = nullptr;
    EXPECT(flux_canvas_create_cpu(UINT32_MAX, 1, 1.0f, &c) == FLUX_ERROR_OUT_OF_RANGE);
    EXPECT(c == nullptr);

    /* ---- Unified factory + pass API (Skia SkSurface-style) ---- */
    flux_canvas_desc d = FLUX_CANVAS_DESC_INIT;
    d.backend = FLUX_CANVAS_BACKEND_CPU;
    d.width = W;
    d.height = H;
    d.scale = 1.0f;
    EXPECT(flux_canvas_create(&d, &c) == FLUX_OK);
    EXPECT(flux_canvas_begin_frame(c, nullptr, &black) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){0, 0, (float)W, (float)H}, red);
    flux_canvas_end_frame(c);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    EXPECT(fb != nullptr && w == W && h == H);
    px(fb, stride, 32, 32, p);
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    /* Descriptor LOAD preserves the previous pass, while explicit NONE
     * permits a clear without asking the backend for multisampling. */
    flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
    pd.antialias = FLUX_CANVAS_ANTIALIAS_NONE;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_end_frame(c);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    flux_color blue = flux_color_rgba_premul(0, 0, 255, 255);
    pd.clear_color = &blue;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_end_frame(c);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 32, 32, p);
    EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    /* An explicitly no-stencil pass keeps ordinary CPU solid/image-style
     * operations valid, matching the Vulkan pass contract. */
    flux_canvas_no_stencil_desc no_stencil = FLUX_CANVAS_NO_STENCIL_DESC_INIT;
    no_stencil.enabled = true;
    pd.next = &no_stencil;
    pd.clear_color = &black;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_fill_rect_color(c, (flux_rect){8, 8, 16, 16}, red);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_STATE);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 12, 12, p);
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);

    /* EVEN_ODD cannot be represented by the nonzero ear-clip path. In a
     * strict no-stencil pass it must diagnose and drop the draw, never
     * silently substitute different fill semantics. */
    flux_arena arena;
    EXPECT(flux_arena_init(&arena, 4096, nullptr) == FLUX_OK);
    flux_path *eo_path = nullptr;
    EXPECT(flux_path_create(&eo_path, &arena) == FLUX_OK);
    flux_path_add_rect(eo_path, (flux_rect){8, 8, 16, 16});
    flux_paint eo = flux_paint_solid(white);
    eo.fill_rule = FLUX_FILL_EVEN_ODD;
    pd.clear_color = &black;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_fill_path(c, eo_path, &eo);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_ERROR_INVALID_STATE);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 12, 12, p);
    EXPECT(p[0] < 5 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    /* The sticky error was consumed by end; the next pass starts clean. */
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    EXPECT(flux_canvas_end_frame_checked(c) == FLUX_OK);
    flux_arena_destroy(&arena);
    pd.next = nullptr;

    /* Restore the blue baseline used by the partial-clear preservation check. */
    pd.clear_color = &blue;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_end_frame(c);

    /* A dirty render area constrains both the attachment clear and the
     * initial draw scissor. Pixels outside it retain the previous blue pass. */
    pd.clear_color = &red;
    pd.render_offset_x = 16;
    pd.render_offset_y = 20;
    pd.render_width = 24;
    pd.render_height = 12;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_OK);
    flux_canvas_end_frame(c);
    fb = flux_canvas_read_pixels(c, &w, &h, &stride);
    px(fb, stride, 20, 24, p);
    EXPECT(p[0] > 250 && p[1] < 5 && p[2] < 5 && p[3] > 250);
    px(fb, stride, 8, 24, p);
    EXPECT(p[0] < 5 && p[1] < 5 && p[2] > 250 && p[3] > 250);

    pd.render_offset_x = 63;
    pd.render_width = 2;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_ERROR_INVALID_ARGUMENT);

    pd.clear_color = nullptr;
    pd.antialias = FLUX_CANVAS_ANTIALIAS_MSAA_4X;
    pd.render_offset_x = 0;
    pd.render_offset_y = 0;
    pd.render_width = 0;
    pd.render_height = 0;
    EXPECT(flux_canvas_begin_pass(c, nullptr, &pd) == FLUX_ERROR_INVALID_ARGUMENT);
    flux_canvas_destroy(c);

    TEST_SUMMARY();
}
