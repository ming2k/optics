/*
 * CPU (software) implementation of the canvas rendering backend.
 *
 * Rasterizes the same high-level draw stream the Vulkan backend consumes
 * directly into a 1x premultiplied-RGBA float framebuffer — no GPU, device,
 * surface, supersampling, or stencil buffer required:
 *   - integer rect batches (canvas_solid/gradient): exact per-pixel coverage
 *   - rounded-rect/circle SDF (canvas_sdf):          analytic distance field
 *   - arbitrary paths & strokes (canvas_coverage):   1x analytic sub-band coverage
 * All four separable blend modes (SRC_OVER, SRC, PLUS, MULTIPLY) match Vulkan.
 *
 * ADR-0069: the framebuffer holds premultiplied linear-light values in the
 * scRGB working space, exactly like the GPU's RGBA16F intermediate. Packed
 * sRGB colours (vertex colours, gradient stops, clear, glyph tints) decode
 * at the edges via optics_gfx_color_to_linear — the same curve the shaders'
 * optics_gfx_decode_premul_srgb applies — and the 8-bit readback performs the
 * output transform once (un-premultiply, sRGB-encode, re-premultiply). The
 * CPU backend deliberately does NOT dither: it is the deterministic oracle;
 * the GPU's ±1 LSB TPDF dither is absorbed by test tolerances.
 *
 * Unsupported (they need GPU-resident textures): image draws. Glyph draws
 * ARE supported on a device-less canvas via a host-resident R8 coverage
 * atlas (ADR-0019): the caller sets optics_gfx_glyph_run_desc::host_coverage
 * instead of `atlas`, and the CPU rasteriser samples coverage directly
 * from it. */
#include "backend.h"

#include <math.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/canvas_cpu.h>
#include <optics/gfx/sdf.h>
#include <stdlib.h>
#include <string.h>

#define OPTICS_GFX_CPU_MAX_LAYERS 8

typedef struct optics_gfx_cpu_layer {
    float *fb;
    float opacity;
} optics_gfx_cpu_layer;

typedef struct optics_gfx_cpu_canvas {
    struct optics_gfx_cpu_canvas *owner;
    struct optics_gfx_cpu_canvas *target_scratch;
    uint32_t width, height; /* output pixel size (1x, no supersampling) */
    /* Cached single-channel coverage plane (optics_gfx_canvas_cpu_coverage):
     * width*height bytes, 1x analytic coverage alpha quantised to 8 bits. */
    uint8_t *coverage;
    float *fb;      /* premultiplied RGBA, width*height*4, row-major (active layer or base) */
    float *base_fb; /* root destination framebuffer */
    uint8_t *rgba8; /* cached 8-bit view (width*height*4) */
    /* Reusable coverage plane for analytic fill (ADR-0119). Sized to the clip
     * on demand and kept for the canvas's lifetime. */
    float *cover_scratch;
    size_t cover_scratch_n;

    /* Isolated layer compositing stack (ADR-0091 Opacity Groups) */
    optics_gfx_cpu_layer layers[OPTICS_GFX_CPU_MAX_LAYERS];
    uint32_t layer_top;
} optics_gfx_cpu_canvas;

static inline optics_gfx_cpu_canvas *cpu(optics_gfx_canvas *c) {
    return (optics_gfx_cpu_canvas *)c->backend_data;
}

typedef struct vec4f {
    float r, g, b, a;
} vec4f;

/* Packed premultiplied sRGB -> premultiplied linear (working space).
 * Same semantics as the GPU vertex shader's optics_gfx_decode_premul_srgb. */
static inline vec4f unpack_premul(optics_gfx_color c) {
    optics_gfx_vec4 l = optics_gfx_color_to_linear(c);
    return (vec4f){l.x, l.y, l.z, l.w};
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ------------------------------------------------------------------ */
/*  Fragment shaders (host ports)                                     */
/* ------------------------------------------------------------------ */

static vec4f frag_gradient(const optics_gfx_canvas_push *pc, float px, float py) {
    float t = 0.0f;
    if (pc->kind == 1u) { /* linear */
        float ax = pc->grad_to[0] - pc->grad_from[0];
        float ay = pc->grad_to[1] - pc->grad_from[1];
        float len2 = ax * ax + ay * ay;
        if (len2 > 0.0f)
            t = ((px - pc->grad_from[0]) * ax + (py - pc->grad_from[1]) * ay) / len2;
    } else if (pc->kind == 2u) { /* radial */
        if (pc->grad_radius > 0.0f) {
            float dx = px - pc->grad_from[0], dy = py - pc->grad_from[1];
            t = sqrtf(dx * dx + dy * dy) / pc->grad_radius;
        }
    }
    t = clampf(t, 0.0f, 1.0f);

    uint32_t n = pc->num_stops;
    if (n == 0u)
        return (vec4f){0, 0, 0, 0};
    if (t <= pc->stops[0].t)
        return unpack_premul(pc->stops[0].color);
    if (t >= pc->stops[n - 1u].t)
        return unpack_premul(pc->stops[n - 1u].color);
    for (uint32_t i = 1u; i < n; ++i) {
        float t1 = pc->stops[i].t;
        if (t <= t1) {
            float t0 = pc->stops[i - 1u].t;
            float u = (t1 > t0) ? (t - t0) / (t1 - t0) : 0.0f;
            vec4f c0 = unpack_premul(pc->stops[i - 1u].color);
            vec4f c1 = unpack_premul(pc->stops[i].color);
            return (vec4f){c0.r + (c1.r - c0.r) * u, c0.g + (c1.g - c0.g) * u,
                           c0.b + (c1.b - c0.b) * u, c0.a + (c1.a - c0.a) * u};
        }
    }
    return unpack_premul(pc->stops[n - 1u].color);
}

static vec4f frag_sdf(const optics_gfx_canvas_push *pc, vec4f vcol, float px, float py) {
    float cx = pc->image_dst[0], cy = pc->image_dst[1];
    float hx = pc->image_dst[2], hy = pc->image_dst[3];
    float radius = pc->image_src[0], stroke_hw = pc->image_src[1];
    float curvature = pc->image_src[2];
    float aa = pc->image_src[3]; /* shape units per screen pixel */

    /* Map the fragment back into shape space with the inverse linear part,
     * then evaluate the shared field there (ADR-0118). The mapping is exact
     * for any similarity, which is all `canvas_sdf_similarity` admits. */
    float rx = px - cx, ry = py - cy;
    float i00 = pc->grad_from[0], i01 = pc->grad_from[1];
    float i10 = pc->grad_to[0], i11 = pc->grad_to[1];
    optics_gfx_point p = {i00 * rx + i01 * ry, i10 * rx + i11 * ry};

    /* The *same* primitives the canvas_sdf.frag pipeline evaluates, so host and
     * device coverage agree by construction rather than by tolerance. This host
     * port once carried its own copy of the formula; test_canvas_squircle's
     * dispatch test now compares the two pipelines end to end. */
    float d = optics_gfx_sdf_squircle(p, hx, hy, radius, curvature);
    d = optics_gfx_sdf_ring(d, stroke_hw);
    float cov = optics_gfx_sdf_coverage(d, aa);
    return (vec4f){vcol.r * cov, vcol.g * cov, vcol.b * cov, vcol.a * cov};
}

/* ------------------------------------------------------------------ */
/*  Rasterizer                                                        */
/* ------------------------------------------------------------------ */

static inline float edge(float ax, float ay, float bx, float by, float px, float py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Match Vulkan's top-left fill convention in the canvas's y-down coordinate
 * system. For a positive-area triangle, top edges run left-to-right and left
 * edges run bottom-to-top. Exactly one of two oppositely directed shared edges
 * is therefore inclusive. */
static inline bool edge_is_top_left(float ax, float ay, float bx, float by) {
    float dy = by - ay;
    float dx = bx - ax;
    return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

/* Blend a premultiplied linear fragment into a framebuffer pixel. Matches the
 * fixed-function VkBlendFactor presets used by the Vulkan pipelines. */
static inline void cpu_blend_pixel(float *dst, vec4f frag, optics_gfx_blend_mode blend) {
    switch (blend) {
    case OPTICS_GFX_BLEND_SRC:
        dst[0] = frag.r;
        dst[1] = frag.g;
        dst[2] = frag.b;
        dst[3] = frag.a;
        break;
    case OPTICS_GFX_BLEND_PLUS:
        dst[0] = frag.r + dst[0];
        dst[1] = frag.g + dst[1];
        dst[2] = frag.b + dst[2];
        dst[3] = frag.a + dst[3];
        break;
    case OPTICS_GFX_BLEND_MULTIPLY: {
        float inva = 1.0f - frag.a;
        dst[0] = frag.r * dst[0] + dst[0] * inva;
        dst[1] = frag.g * dst[1] + dst[1] * inva;
        dst[2] = frag.b * dst[2] + dst[2] * inva;
        dst[3] = frag.a * dst[3] + dst[3] * inva;
        break;
    }
    default: /* SRC_OVER */
    {
        float inv = 1.0f - frag.a;
        dst[0] = frag.r + dst[0] * inv;
        dst[1] = frag.g + dst[1] * inv;
        dst[2] = frag.b + dst[2] * inv;
        dst[3] = frag.a + dst[3] * inv;
        break;
    }
    }
}

static void raster_tri(optics_gfx_cpu_canvas *v, optics_gfx_recti clip, canvas_pipe_id id,
                       const optics_gfx_canvas_push *pc, optics_gfx_blend_mode blend,
                       const optics_gfx_canvas_vertex *a, const optics_gfx_canvas_vertex *b,
                       const optics_gfx_canvas_vertex *c) {
    const optics_gfx_canvas_vertex *va = a;
    const optics_gfx_canvas_vertex *vb = b;
    const optics_gfx_canvas_vertex *vc = c;
    float ax = va->pos[0], ay = va->pos[1];
    float bx = vb->pos[0], by = vb->pos[1];
    float cx = vc->pos[0], cy = vc->pos[1];
    float area = edge(ax, ay, bx, by, cx, cy);
    if (fabsf(area) < 1e-7f)
        return;
    if (area < 0.0f) {
        const optics_gfx_canvas_vertex *tmp_v = vb;
        vb = vc;
        vc = tmp_v;
        float tmp = bx;
        bx = cx;
        cx = tmp;
        tmp = by;
        by = cy;
        cy = tmp;
        area = -area;
    }
    float inv_area = 1.0f / area;
    vec4f ca = unpack_premul(va->color), cb = unpack_premul(vb->color),
          cc = unpack_premul(vc->color);

    bool edge0_inclusive = edge_is_top_left(bx, by, cx, cy);
    bool edge1_inclusive = edge_is_top_left(cx, cy, ax, ay);
    bool edge2_inclusive = edge_is_top_left(ax, ay, bx, by);

    int cx0 = clip.x, cy0 = clip.y;
    int cx1 = clip.x + (int)clip.w, cy1 = clip.y + (int)clip.h;

    int minx = (int)floorf(fminf(ax, fminf(bx, cx)));
    int maxx = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int miny = (int)floorf(fminf(ay, fminf(by, cy)));
    int maxy = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (minx < cx0)
        minx = cx0;
    if (miny < cy0)
        miny = cy0;
    if (maxx > cx1)
        maxx = cx1;
    if (maxy > cy1)
        maxy = cy1;
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx > (int)v->width)
        maxx = (int)v->width;
    if (maxy > (int)v->height)
        maxy = (int)v->height;

    for (int sy = miny; sy < maxy; ++sy) {
        for (int sx = minx; sx < maxx; ++sx) {
            float fx = (float)sx + 0.5f;
            float fy = (float)sy + 0.5f;
            float e0 = edge(bx, by, cx, cy, fx, fy);
            float e1 = edge(cx, cy, ax, ay, fx, fy);
            float e2 = edge(ax, ay, bx, by, fx, fy);
            if (e0 < 0.0f || (e0 == 0.0f && !edge0_inclusive) || e1 < 0.0f ||
                (e1 == 0.0f && !edge1_inclusive) || e2 < 0.0f || (e2 == 0.0f && !edge2_inclusive))
                continue;
            float w0 = e0 * inv_area;
            float w1 = e1 * inv_area;
            float w2 = e2 * inv_area;

            vec4f frag;
            switch (id) {
            case CANVAS_PIPE_GRADIENT:
                frag = frag_gradient(pc, fx, fy);
                break;
            case CANVAS_PIPE_SDF: {
                vec4f vc = {w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g,
                            w0 * ca.b + w1 * cb.b + w2 * cc.b, w0 * ca.a + w1 * cb.a + w2 * cc.a};
                frag = frag_sdf(pc, vc, fx, fy);
                break;
            }
            case CANVAS_PIPE_IMAGE:
                return; /* textured image: unsupported on CPU */
            default:    /* CANVAS_PIPE_SOLID */
                frag =
                    (vec4f){w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g,
                            w0 * ca.b + w1 * cb.b + w2 * cc.b, w0 * ca.a + w1 * cb.a + w2 * cc.a};
                break;
            }

            float *dst = &v->fb[((size_t)sy * v->width + sx) * 4];
            cpu_blend_pixel(dst, frag, blend);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Glyph run blit (host atlas — ADR-0019)                             */
/* ------------------------------------------------------------------ */

/* Unpack the UV that draw_glyph_run stored as unorm16x2 in vertex::_pad. */
static inline void unpack_uv(uint32_t pad, float *u, float *v) {
    *u = (float)(pad & 0xFFFFu) / 65535.0f;
    *v = (float)(pad >> 16) / 65535.0f;
}

/* Sample the host R8 coverage atlas at normalised (u,v) in [0,1] with
 * NEAREST filtering, matching the GPU path's atlas sampler (txt_engine_init
 * creates a NEAREST sampler for crisp blits). The atlas is rasterised at the
 * device pixel grid, so bilinear here would only blur already-aligned ink;
 * edge anti-aliasing comes from the 2x2 framebuffer supersampling in
 * raster_glyph_quad, not from filtering the coverage map. CLAMP_TO_EDGE. */
static inline float sample_cov(const uint8_t *atlas, uint32_t aw, uint32_t ah, float u, float v) {
    int x = (int)(u * (float)aw);
    int y = (int)(v * (float)ah);
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x > (int)aw - 1)
        x = (int)aw - 1;
    if (y > (int)ah - 1)
        y = (int)ah - 1;
    return atlas[(size_t)y * aw + x] * (1.0f / 255.0f);
}

/* Rasterise one glyph quad (two triangles, indices 0..5 share color + an
 * axis-aligned screen rect) by sampling the host R8 atlas at each supersample
 * and premultiplied SRC_OVER blending into the float framebuffer. Each sample
 * is blended independently, so supersampled edges anti-alias and overlapping
 * runs compose correctly — the same model raster_tri uses for fills. */
static void raster_glyph_quad(optics_gfx_cpu_canvas *v, optics_gfx_recti clip,
                              const optics_gfx_canvas_vertex *verts, const uint8_t *atlas,
                              uint32_t aw, uint32_t ah) {
    /* The quad's six vertices form an axis-aligned rect (draw_glyph_run
     * emits p0,p1,p2,p3 = TL,TR,BR,BL). Use min/max over all six for
     * robustness against winding. Positions are already in output pixels. */
    float minx = verts[0].pos[0], maxx = minx;
    float miny = verts[0].pos[1], maxy = miny;
    for (int i = 1; i < 6; i++) {
        float x = verts[i].pos[0], y = verts[i].pos[1];
        if (x < minx)
            minx = x;
        if (x > maxx)
            maxx = x;
        if (y < miny)
            miny = y;
        if (y > maxy)
            maxy = y;
    }

    int cx0 = clip.x, cy0 = clip.y;
    int cx1 = clip.x + (int)clip.w, cy1 = clip.y + (int)clip.h;

    int sx0 = (int)floorf(minx);
    int sy0 = (int)floorf(miny);
    int sx1 = (int)ceilf(maxx);
    int sy1 = (int)ceilf(maxy);
    if (sx0 < cx0)
        sx0 = cx0;
    if (sy0 < cy0)
        sy0 = cy0;
    if (sx1 > cx1)
        sx1 = cx1;
    if (sy1 > cy1)
        sy1 = cy1;
    if (sx0 < 0)
        sx0 = 0;
    if (sy0 < 0)
        sy0 = 0;
    if (sx1 > (int)v->width)
        sx1 = (int)v->width;
    if (sy1 > (int)v->height)
        sy1 = (int)v->height;

    vec4f tint = unpack_premul(verts[0].color);

    const optics_gfx_canvas_vertex *ta[3] = {&verts[0], &verts[1], &verts[2]};
    const optics_gfx_canvas_vertex *tb[3] = {&verts[3], &verts[4], &verts[5]};
    float tax[3], tay[3], tbx[3], tby[3];
    for (int i = 0; i < 3; i++) {
        tax[i] = ta[i]->pos[0];
        tay[i] = ta[i]->pos[1];
        tbx[i] = tb[i]->pos[0];
        tby[i] = tb[i]->pos[1];
    }
    float area_a = edge(tax[0], tay[0], tax[1], tay[1], tax[2], tay[2]);
    float area_b = edge(tbx[0], tby[0], tbx[1], tby[1], tbx[2], tby[2]);
    bool use_a = fabsf(area_a) > 1e-7f;
    bool use_b = fabsf(area_b) > 1e-7f;
    if (!use_a && !use_b)
        return;

    for (int sy = sy0; sy < sy1; ++sy) {
        for (int sx = sx0; sx < sx1; ++sx) {
            float fx = (float)sx + 0.5f;
            float fy = (float)sy + 0.5f;
            /* Find which triangle covers this sample and its barycentric UV. */
            float w0 = 0, w1 = 0, w2 = 0;
            float u = 0.0f, vv = 0.0f;
            bool hit = false;
            if (use_a) {
                float ia = 1.0f / area_a;
                w0 = edge(tax[1], tay[1], tax[2], tay[2], fx, fy) * ia;
                w1 = edge(tax[2], tay[2], tax[0], tay[0], fx, fy) * ia;
                w2 = edge(tax[0], tay[0], tax[1], tay[1], fx, fy) * ia;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    float u0, v0, u1, v1, u2, v2;
                    unpack_uv(ta[0]->_pad, &u0, &v0);
                    unpack_uv(ta[1]->_pad, &u1, &v1);
                    unpack_uv(ta[2]->_pad, &u2, &v2);
                    u = w0 * u0 + w1 * u1 + w2 * u2;
                    vv = w0 * v0 + w1 * v1 + w2 * v2;
                    hit = true;
                }
            }
            if (!hit && use_b) {
                float ib = 1.0f / area_b;
                w0 = edge(tbx[1], tby[1], tbx[2], tby[2], fx, fy) * ib;
                w1 = edge(tbx[2], tby[2], tbx[0], tby[0], fx, fy) * ib;
                w2 = edge(tbx[0], tby[0], tbx[1], tby[1], fx, fy) * ib;
                if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                    float u3, v3, u4, v4, u5, v5;
                    unpack_uv(tb[0]->_pad, &u3, &v3);
                    unpack_uv(tb[1]->_pad, &u4, &v4);
                    unpack_uv(tb[2]->_pad, &u5, &v5);
                    u = w0 * u3 + w1 * u4 + w2 * u5;
                    vv = w0 * v3 + w1 * v4 + w2 * v5;
                    hit = true;
                }
            }
            if (!hit)
                continue;

            float cov = sample_cov(atlas, aw, ah, u, vv);
            if (cov <= 0.0f)
                continue;

            /* premultiplied tint × coverage, SRC_OVER. */
            float *dst = &v->fb[((size_t)sy * v->width + sx) * 4];
            float pr = tint.r * cov;
            float pg = tint.g * cov;
            float pb = tint.b * cov;
            float pa = tint.a * cov;
            float inv = 1.0f - pa;
            dst[0] = pr + dst[0] * inv;
            dst[1] = pg + dst[1] * inv;
            dst[2] = pb + dst[2] * inv;
            dst[3] = pa + dst[3] * inv;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Vtable                                                            */
/* ------------------------------------------------------------------ */

static optics_gfx_result cpu_canvas_init(const optics_gfx_canvas_backend *self,
                                         optics_gfx_canvas *c) {
    (void)self;
    if (c->fb_width == 0 || c->fb_height == 0 ||
        (uint64_t)c->fb_width > (uint64_t)UINT32_MAX / (4u * sizeof(float)) ||
        (uint64_t)c->fb_height >
            (uint64_t)UINT32_MAX / (4u * sizeof(float)) / (uint64_t)c->fb_width) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "CPU canvas framebuffer size overflow");
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    }

    optics_gfx_cpu_canvas *v = calloc(1, sizeof(*v));
    if (!v)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    v->width = c->fb_width;
    v->height = c->fb_height;
    v->fb = calloc((size_t)v->width * v->height * 4, sizeof(float));
    if (!v->fb) {
        free(v);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    v->base_fb = v->fb;
    v->layer_top = 0;
    c->backend_data = v;
    return OPTICS_GFX_OK;
}

static void cpu_canvas_destroy(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (!v)
        return;
    if (v->target_scratch) {
        c->backend_data = v->target_scratch;
        cpu_canvas_destroy(self, c);
        c->backend_data = v;
    }
    for (uint32_t i = 0; i < v->layer_top; ++i) {
        free(v->layers[i].fb);
    }
    free(v->base_fb);
    free(v->rgba8);
    free(v->cover_scratch);
    free(v->coverage);
    free(v);
    c->backend_data = nullptr;
}

static optics_gfx_result cpu_save_layer(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                        const optics_gfx_rect *bounds, float opacity) {
    (void)self;
    (void)bounds;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (!v)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (v->layer_top >= OPTICS_GFX_CPU_MAX_LAYERS)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;

    size_t pixels = (size_t)v->width * v->height;
    float *layer_fb = calloc(pixels * 4, sizeof(float));
    if (!layer_fb)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    v->layers[v->layer_top].fb = layer_fb;
    v->layers[v->layer_top].opacity = opacity;
    v->layer_top++;
    v->fb = layer_fb;
    return OPTICS_GFX_OK;
}

static void cpu_restore_layer(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (!v || v->layer_top == 0)
        return;

    v->layer_top--;
    float *layer_fb = v->layers[v->layer_top].fb;
    float opacity = v->layers[v->layer_top].opacity;
    if (opacity < 0.0f)
        opacity = 0.0f;
    if (opacity > 1.0f)
        opacity = 1.0f;

    float *parent_fb = (v->layer_top > 0) ? v->layers[v->layer_top - 1].fb : v->base_fb;

    size_t pixels = (size_t)v->width * v->height;
    for (size_t i = 0; i < pixels; ++i) {
        float *l = &layer_fb[i * 4];
        float la = l[3] * opacity;
        if (la <= 0.0f)
            continue;
        float lr = l[0] * opacity;
        float lg = l[1] * opacity;
        float lb = l[2] * opacity;
        float *p = &parent_fb[i * 4];
        float inv_la = 1.0f - la;
        p[0] = lr + p[0] * inv_la;
        p[1] = lg + p[1] * inv_la;
        p[2] = lb + p[2] * inv_la;
        p[3] = la + p[3] * inv_la;
    }

    free(layer_fb);
    v->layers[v->layer_top].fb = nullptr;
    v->fb = parent_fb;
}

static const uint8_t *cpu_read_pixels(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                      uint32_t *width, uint32_t *height, uint32_t *stride);

/* RGBA8 targets are the authoritative storage between passes. Load every
 * sample from that target, preserving untouched pixels across dirty passes. */
static void cpu_load_target(optics_gfx_cpu_canvas *v, const optics_gfx_target *target) {
    for (uint32_t y = 0; y < v->height; ++y) {
        for (uint32_t x = 0; x < v->width; ++x) {
            const uint8_t *px = target->cpu_buffer + y * target->cpu_stride + x * 4;
            vec4f color = unpack_premul(((uint32_t)px[3] << 24) | ((uint32_t)px[0] << 16) |
                                        ((uint32_t)px[1] << 8) | px[2]);
            float *dst = v->fb + ((size_t)y * v->width + x) * 4;
            dst[0] = color.r;
            dst[1] = color.g;
            dst[2] = color.b;
            dst[3] = color.a;
        }
    }
}

static optics_gfx_result cpu_begin_pass(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                        optics_gfx_frame *f, optics_gfx_image *target,
                                        const canvas_pass_config *config) {
    (void)self;
    (void)f;
    if (target) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "CPU canvas has no offscreen target (v1)");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    optics_gfx_cpu_canvas *root = cpu(c);
    optics_gfx_cpu_canvas *v = root;
    optics_gfx_target *host = c->bound_cpu_target;
    uint32_t width = host ? host->width : root->width;
    uint32_t height = host ? host->height : root->height;
    optics_gfx_recti area;
    if (!canvas_pass_render_bounds(config, width, height, &area)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "Canvas damage scope names no pixel of the target");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* ADR-0149: mirror the GPU path's per-draw scissor so the two backends
     * produce the same pixels for the same descriptor. Kept on the canvas, not
     * on this stack frame, because submit runs later. */
    c->pass_damage = *config;

    if (host) {
        v = root->target_scratch;
        if (v && (v->width != width || v->height != height)) {
            c->backend_data = v;
            cpu_canvas_destroy(self, c);
            c->backend_data = root;
            root->target_scratch = nullptr;
            v = nullptr;
        }
        if (!v) {
            c->fb_width = width;
            c->fb_height = height;
            optics_gfx_result result = cpu_canvas_init(self, c);
            if (result != OPTICS_GFX_OK) {
                c->backend_data = root;
                return result;
            }
            v = cpu(c);
            v->rgba8 = malloc((size_t)width * height * 4);
            if (!v->rgba8) {
                cpu_canvas_destroy(self, c);
                c->backend_data = root;
                return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
            }
            v->owner = root;
            root->target_scratch = v;
        }
        c->backend_data = v;
        cpu_load_target(v, host);
    }
    if (config->clear_color) {
        vec4f cc = unpack_premul(*config->clear_color);
        /* ADR-0149: clear only the damage rectangles, exactly as the Vulkan
         * path restricts its clear. Pixels between two damage bands belong to
         * other widgets and must survive this pass. */
        optics_gfx_recti clears[OPTICS_GFX_DAMAGE_CAPACITY];
        const uint32_t clear_count =
            canvas_pass_areas(config, width, height, clears, OPTICS_GFX_DAMAGE_CAPACITY);
        for (uint32_t a = 0; a < clear_count; a++) {
            const uint32_t x0 = (uint32_t)clears[a].x;
            const uint32_t y0 = (uint32_t)clears[a].y;
            const uint32_t x1 = (uint32_t)clears[a].x + clears[a].w;
            const uint32_t y1 = (uint32_t)clears[a].y + clears[a].h;
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = x0; x < x1; ++x) {
                    size_t i = (size_t)y * v->width + x;
                    v->fb[i * 4 + 0] = cc.r;
                    v->fb[i * 4 + 1] = cc.g;
                    v->fb[i * 4 + 2] = cc.b;
                    v->fb[i * 4 + 3] = cc.a;
                }
            }
        }
    }

    c->pass_active = true;
    c->target_pass = false;
    c->target = nullptr;
    c->fb_width = v->width;
    c->fb_height = v->height;
    c->states[0].scissor = area;
    return OPTICS_GFX_OK;
}

static void cpu_end_pass(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (c->bound_cpu_target) {
        const uint8_t *pixels = cpu_read_pixels(self, c, nullptr, nullptr, nullptr);
        optics_gfx_target *target = c->bound_cpu_target;
        /* ADR-0149: write back only the damage rectangles. The scratch buffer
         * holds the target's whole contents (loaded by cpu_load_target), so a
         * full copy-back would still be correct — but copying only the damaged
         * region is what proves the pass honoured its scope, and it keeps a
         * regional update cheap on a host target as well. */
        optics_gfx_recti written[OPTICS_GFX_DAMAGE_CAPACITY];
        const uint32_t written_count = canvas_pass_areas(&c->pass_damage, v->width, v->height,
                                                         written, OPTICS_GFX_DAMAGE_CAPACITY);
        for (uint32_t a = 0; a < written_count; a++) {
            const uint32_t row_bytes = written[a].w * 4u;
            for (uint32_t y = written[a].y; y < written[a].y + written[a].h; ++y) {
                memcpy(target->cpu_buffer + y * target->cpu_stride + (size_t)written[a].x * 4u,
                       pixels + (size_t)y * v->width * 4 + (size_t)written[a].x * 4u, row_bytes);
            }
        }
        c->backend_data = v->owner;
    }
    c->pass_active = false;
}

static void cpu_set_scissor(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                            optics_gfx_recti clip) {
    (void)self;
    (void)c;
    (void)clip; /* submit reads c->states[top].scissor directly */
}

static bool cpu_bind_program(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                             canvas_pipe_id id) {
    (void)self;
    (void)c;
    (void)id;
    return true; /* no pipeline objects; submit switches on id */
}

static void cpu_submit(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                       canvas_pipe_id id, const optics_gfx_canvas_push *push,
                       const optics_gfx_canvas_vertex *verts, uint32_t vertex_count) {
    (void)self;
    if (!c->recording || vertex_count < 3)
        return;

    optics_gfx_cpu_canvas *v = cpu(c);
    optics_gfx_recti clip;
    /* ADR-0149: intersect the draw's logical clip with the pass's damage scope,
     * exactly as the Vulkan backend does. A draw that touches no damage is
     * dropped rather than rasterised. */
    if (!canvas_damage_select(&c->pass_damage, c->states[c->state_top].scissor, &clip)) {
        c->dropped_draws++;
        return;
    }

    /* Glyph runs sample a host R8 atlas on the CPU backend (ADR-0019). Each
     * quad is six vertices (two tris); blit them directly. Image draws still
     * have no host source, so they stay unsupported. */
    if (id == CANVAS_PIPE_GLYPH) {
        const uint8_t *atlas = c->pending_host_atlas;
        uint32_t aw = c->pending_host_atlas_w;
        uint32_t ah = c->pending_host_atlas_h;
        if (!atlas || aw == 0 || ah == 0)
            return;
        for (uint32_t i = 0; i + 6 <= vertex_count; i += 6)
            raster_glyph_quad(v, clip, &verts[i], atlas, aw, ah);
        return;
    }
    if (id == CANVAS_PIPE_IMAGE)
        return; /* textured image draws unsupported on CPU */

    for (uint32_t i = 0; i + 3 <= vertex_count; i += 3)
        raster_tri(v, clip, id, push, c->pending_blend, &verts[i], &verts[i + 1], &verts[i + 2]);
}

static const uint8_t *cpu_read_pixels(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                      uint32_t *width, uint32_t *height, uint32_t *stride) {
    (void)self;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (!v->rgba8) {
        if ((size_t)v->width > SIZE_MAX / (size_t)v->height / 4u)
            return nullptr;
        v->rgba8 = malloc((size_t)v->width * v->height * 4);
        if (!v->rgba8)
            return nullptr;
    }
    /* ADR-0069 output transform at 1x pixel resolution: un-premultiply,
     * sRGB-encode, re-premultiply, quantise. No dither — deterministic oracle. */
    for (uint32_t y = 0; y < v->height; ++y) {
        for (uint32_t x = 0; x < v->width; ++x) {
            const float *s = &v->fb[((size_t)y * v->width + x) * 4];
            uint8_t *o = &v->rgba8[((size_t)y * v->width + x) * 4];
            float a = s[3];
            float inv_a = a > 0.0f ? 1.0f / a : 0.0f;
            o[3] = (uint8_t)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
            for (int k = 0; k < 3; ++k) {
                float straight = s[k] * inv_a; /* a == 0 -> black */
                float enc = optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, straight);
                o[k] = (uint8_t)(clampf(enc * a, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
        }
    }
    if (width)
        *width = v->width;
    if (height)
        *height = v->height;
    if (stride)
        *stride = v->width * 4;
    return v->rgba8;
}

/* Single-channel coverage plane: alpha quantised directly to 8 bits at 1x.
 * The resolve matches cpu_read_pixels' alpha channel exactly, so a mask read
 * here and a premultiplied pixel read there agree on coverage. */
static const uint8_t *cpu_read_coverage(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                        uint32_t *width, uint32_t *height, uint32_t *stride) {
    (void)self;
    optics_gfx_cpu_canvas *v = cpu(c);
    if (!v || !v->fb || v->width == 0 || v->height == 0)
        return nullptr;
    if (!v->coverage) {
        if ((size_t)v->width > SIZE_MAX / (size_t)v->height)
            return nullptr;
        v->coverage = malloc((size_t)v->width * v->height);
        if (!v->coverage)
            return nullptr;
    }
    for (uint32_t y = 0; y < v->height; ++y) {
        for (uint32_t x = 0; x < v->width; ++x) {
            float a = v->fb[((size_t)y * v->width + x) * 4 + 3];
            v->coverage[(size_t)y * v->width + x] =
                (uint8_t)(clampf(a, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
    if (width)
        *width = v->width;
    if (height)
        *height = v->height;
    if (stride)
        *stride = v->width;
    return v->coverage;
}

/* Analytic coverage fill (ADR-0119): rasterize the contours at 1x pixel
 * resolution and composite the (already premultiplied) colour directly. */
static optics_gfx_result cpu_submit_cover(const optics_gfx_canvas_backend *self,
                                          optics_gfx_canvas *c, const optics_gfx_point *pts,
                                          const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                          optics_gfx_fill_rule rule, const canvas_paint *paint,
                                          optics_gfx_recti clip) {
    (void)self;
    optics_gfx_cpu_canvas *v = c ? c->backend_data : nullptr;
    if (!v || !v->fb || !pts || !cons)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (clip.w <= 0 || clip.h <= 0 || ncons == 0)
        return OPTICS_GFX_OK;

    if (paint->kind != CANVAS_PAINT_SOLID && paint->kind != CANVAS_PAINT_LINEAR_GRADIENT &&
        paint->kind != CANVAS_PAINT_RADIAL_GRADIENT)
        return OPTICS_GFX_ERROR_UNSUPPORTED;

    /* Narrow the coverage sweep to the geometry bbox intersected with clip. */
    float min_x = 1e30f, min_y = 1e30f, max_x = -1e30f, max_y = -1e30f;
    bool have_pt = false;
    for (uint32_t ci = 0; ci < ncons; ++ci) {
        for (uint32_t k = 0; k < cons[ci].count; ++k) {
            optics_gfx_point pt = pts[cons[ci].start + k];
            if (!isfinite(pt.x) || !isfinite(pt.y))
                continue;
            if (!have_pt || pt.x < min_x)
                min_x = pt.x;
            if (!have_pt || pt.x > max_x)
                max_x = pt.x;
            if (!have_pt || pt.y < min_y)
                min_y = pt.y;
            if (!have_pt || pt.y > max_y)
                max_y = pt.y;
            have_pt = true;
        }
    }
    if (!have_pt)
        return OPTICS_GFX_OK;
    int x0 = (int)floorf(min_x), y0 = (int)floorf(min_y);
    int x1 = (int)ceilf(max_x), y1 = (int)ceilf(max_y);
    int cmin_x = clip.x > 0 ? clip.x : 0;
    int cmin_y = clip.y > 0 ? clip.y : 0;
    int cmax_x = clip.x + (int)clip.w;
    int cmax_y = clip.y + (int)clip.h;
    if (cmax_x > (int)v->width)
        cmax_x = (int)v->width;
    if (cmax_y > (int)v->height)
        cmax_y = (int)v->height;
    if (x0 < cmin_x)
        x0 = cmin_x;
    if (y0 < cmin_y)
        y0 = cmin_y;
    if (x1 > cmax_x)
        x1 = cmax_x;
    if (y1 > cmax_y)
        y1 = cmax_y;
    if (x1 <= x0 || y1 <= y0)
        return OPTICS_GFX_OK;

    uint32_t w = (uint32_t)(x1 - x0), h = (uint32_t)(y1 - y0);
    size_t need = (size_t)w * h;
    if (need > v->cover_scratch_n) {
        float *grown = realloc(v->cover_scratch, need * sizeof(float));
        if (!grown)
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        v->cover_scratch = grown;
        v->cover_scratch_n = need;
    }
    float *cov = v->cover_scratch;
    optics_gfx_result rc =
        canvas_coverage_rasterize(pts, cons, ncons, rule, (float)x0, (float)y0, w, h, cov);
    if (rc != OPTICS_GFX_OK)
        return rc;

    optics_gfx_canvas_push pc;
    build_push(c, paint, &pc);
    vec4f solid_col = unpack_premul(paint->color);

    for (uint32_t y = 0; y < h; ++y) {
        int py = y0 + (int)y;
        for (uint32_t x = 0; x < w; ++x) {
            float a = cov[(size_t)y * w + x];
            if (a <= 0.0f)
                continue;
            int px = x0 + (int)x;
            float px_c = (float)px + 0.5f;
            float py_c = (float)py + 0.5f;
            vec4f src =
                (paint->kind == CANVAS_PAINT_SOLID) ? solid_col : frag_gradient(&pc, px_c, py_c);
            vec4f frag = {src.r * a, src.g * a, src.b * a, src.a * a};
            float *dst = &v->fb[((size_t)py * v->width + px) * 4];
            cpu_blend_pixel(dst, frag, paint->blend);
        }
    }
    return OPTICS_GFX_OK;
}

static const optics_gfx_canvas_backend cpu_backend = {
    .name = "cpu",
    .canvas_init = cpu_canvas_init,
    .canvas_destroy = cpu_canvas_destroy,
    .begin_pass = cpu_begin_pass,
    .end_pass = cpu_end_pass,
    .set_scissor = cpu_set_scissor,
    .bind_program = cpu_bind_program,
    .submit = cpu_submit,
    .submit_cover = cpu_submit_cover,
    .read_pixels = cpu_read_pixels,
    .read_coverage = cpu_read_coverage,
    .save_layer = cpu_save_layer,
    .restore_layer = cpu_restore_layer,
};

const optics_gfx_canvas_backend *optics_gfx_canvas_backend_cpu(void) {
    return &cpu_backend;
}

/* ------------------------------------------------------------------ */
/*  Public headless-canvas API (gfx/canvas_cpu.h)                    */
/* ------------------------------------------------------------------ */

optics_gfx_result optics_gfx_canvas_cpu_create(uint32_t width, uint32_t height, float scale,
                                               optics_gfx_canvas **out) {
    if (!out || width == 0 || height == 0) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "width/height must be non-zero");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    optics_gfx_canvas *c = calloc(1, sizeof(*c));
    if (!c)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&c->ref_count, 1u);
    c->device = nullptr;
    c->surface = nullptr;
    c->backend = optics_gfx_canvas_backend_cpu();
    c->content_scale = (scale > 0.0f) ? scale : 1.0f;
    c->states[0].transform = optics_gfx_mat3x2_scale(c->content_scale, c->content_scale);
    c->state_top = 0;
    c->plan_node = UINT32_MAX;
    c->fb_width = width;
    c->fb_height = height;

    optics_gfx_result r = c->backend->canvas_init(c->backend, c);
    if (r != OPTICS_GFX_OK) {
        free(c);
        return r;
    }
    if (!optics_gfx_canvas_alloc_scratch(c)) {
        optics_gfx_canvas_free_scratch(c);
        c->backend->canvas_destroy(c->backend, c);
        free(c);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    *out = c;
    return OPTICS_GFX_OK;
}

const uint8_t *optics_gfx_canvas_cpu_pixels(const optics_gfx_canvas *c, uint32_t *width,
                                            uint32_t *height, uint32_t *stride) {
    return optics_gfx_canvas_read_pixels((optics_gfx_canvas *)c, width, height, stride);
}

const uint8_t *optics_gfx_canvas_cpu_coverage(const optics_gfx_canvas *c, uint32_t *width,
                                              uint32_t *height, uint32_t *stride) {
    if (!c)
        return nullptr;
    return c->backend->read_coverage(c->backend, (optics_gfx_canvas *)c, width, height, stride);
}
