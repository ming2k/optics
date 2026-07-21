/*
 * CPU (software) implementation of the canvas rendering backend.
 *
 * Renders the same triangle-batch stream the Vulkan backend consumes, but
 * rasterizes on the host into a premultiplied-RGBA framebuffer — no GPU,
 * device, or surface required. It reproduces the canvas fragment shaders:
 *   - solid            (canvas_solid.frag):    interpolated vertex colour
 *   - linear/radial    (canvas_gradient.frag): per-pixel gradient lookup
 *   - rounded-rect SDF  (canvas_sdf.frag):      analytic distance-field coverage
 * Edge anti-aliasing comes from 4x supersampled triangle coverage (the GPU
 * path uses 4x MSAA); blending is premultiplied SRC_OVER, matching the Vulkan
 * pipeline's blend state.
 *
 * Unsupported (they need GPU-resident textures): image draws. Glyph draws
 * ARE supported on a device-less canvas via a host-resident R8 coverage
 * atlas (ADR-0019): the caller sets flux_glyph_run_desc::host_coverage
 * instead of `atlas`, and the CPU rasteriser samples coverage directly
 * from it. Only image draws remain unsupported on CPU. */
#include "backend.h"

#include <flux/canvas_cpu.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Supersampling factor per axis (SS*SS samples/pixel; 4x ~= the GPU's 4x MSAA).
 * Rendering happens in a width*SS x height*SS sample buffer and is box-filtered
 * to the output on readback. Blending per sample (not per-triangle averaging)
 * is what keeps shared triangle edges seam-free. */
#define FLUX_CPU_SS 2

typedef struct flux_cpu_canvas {
    uint32_t width, height; /* logical (output) size */
    uint32_t sw, sh;        /* sample-buffer size = width*SS, height*SS */
    float *fb;              /* premultiplied RGBA, sw*sh*4, row-major */
    uint8_t *rgba8;         /* cached downsampled 8-bit view (width*height*4) */
} flux_cpu_canvas;

static inline flux_cpu_canvas *cpu(flux_canvas *c) {
    return (flux_cpu_canvas *)c->backend_data;
}

typedef struct vec4f {
    float r, g, b, a;
} vec4f;

static inline vec4f unpack_premul(flux_color c) {
    uint8_t r, g, b, a;
    flux_color_unpack(c, &r, &g, &b, &a);
    return (vec4f){r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* ------------------------------------------------------------------ */
/*  Fragment shaders (host ports)                                     */
/* ------------------------------------------------------------------ */

static vec4f frag_gradient(const flux_canvas_push *pc, float px, float py) {
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

static vec4f frag_sdf(const flux_canvas_push *pc, vec4f vcol, float px, float py) {
    float cx = pc->image_dst[0], cy = pc->image_dst[1];
    float hx = pc->image_dst[2], hy = pc->image_dst[3];
    float radius = pc->image_src[0], stroke_hw = pc->image_src[1];

    float qx = fabsf(px - cx) - (hx - radius);
    float qy = fabsf(py - cy) - (hy - radius);
    float mx = qx > 0.0f ? qx : 0.0f;
    float my = qy > 0.0f ? qy : 0.0f;
    float d = fminf(fmaxf(qx, qy), 0.0f) + sqrtf(mx * mx + my * my) - radius;
    if (stroke_hw > 0.0f)
        d = fabsf(d) - stroke_hw;

    /* Screen-space |grad(d)| ~= 1 px for the canvas transform, so fwidth ~= 1. */
    float cov = clampf(0.5f - d, 0.0f, 1.0f);
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

/* Rasterize one triangle into the sample buffer, one blended sample per
 * hi-res texel. Vertex positions are in canvas (output) space; they are scaled
 * by SS here. Fragment shaders evaluate at the equivalent canvas coordinate.
 * `blend` selects the compositing mode (ADR: canvas blend modes); the GPU
 * backend implements the same set via fixed-function VkBlendFactor. */
static void raster_tri(flux_cpu_canvas *v, flux_recti clip, canvas_pipe_id id,
                       const flux_canvas_push *pc, flux_blend_mode blend,
                       const flux_canvas_vertex *a, const flux_canvas_vertex *b,
                       const flux_canvas_vertex *c) {
    const float ss = (float)FLUX_CPU_SS;
    const flux_canvas_vertex *va = a;
    const flux_canvas_vertex *vb = b;
    const flux_canvas_vertex *vc = c;
    float ax = va->pos[0] * ss, ay = va->pos[1] * ss;
    float bx = vb->pos[0] * ss, by = vb->pos[1] * ss;
    float cx = vc->pos[0] * ss, cy = vc->pos[1] * ss;
    float area = edge(ax, ay, bx, by, cx, cy);
    if (fabsf(area) < 1e-7f)
        return;
    if (area < 0.0f) {
        const flux_canvas_vertex *tmp_v = vb;
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

    /* Clip is in output pixels; scale to sample space. */
    int cx0 = clip.x * FLUX_CPU_SS, cy0 = clip.y * FLUX_CPU_SS;
    int cx1 = (clip.x + (int)clip.w) * FLUX_CPU_SS, cy1 = (clip.y + (int)clip.h) * FLUX_CPU_SS;

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
    if (maxx > (int)v->sw)
        maxx = (int)v->sw;
    if (maxy > (int)v->sh)
        maxy = (int)v->sh;

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

            /* Fragment shaders operate in canvas (output) coordinates. */
            float px = fx / ss, py = fy / ss;
            vec4f frag;
            switch (id) {
            case CANVAS_PIPE_GRADIENT:
            case CANVAS_PIPE_COVER_GRADIENT:
                frag = frag_gradient(pc, px, py);
                break;
            case CANVAS_PIPE_SDF: {
                vec4f vc = {w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g,
                            w0 * ca.b + w1 * cb.b + w2 * cc.b, w0 * ca.a + w1 * cb.a + w2 * cc.a};
                frag = frag_sdf(pc, vc, px, py);
                break;
            }
            case CANVAS_PIPE_STENCIL_WRITE:
            case CANVAS_PIPE_STENCIL_WRITE_EO:
                continue; /* colour-write masked off; CPU has no stencil buffer */
            case CANVAS_PIPE_IMAGE:
                return; /* textured image: unsupported on CPU */
            default:    /* SOLID / COVER_SOLID */
                frag =
                    (vec4f){w0 * ca.r + w1 * cb.r + w2 * cc.r, w0 * ca.g + w1 * cb.g + w2 * cc.g,
                            w0 * ca.b + w1 * cb.b + w2 * cc.b, w0 * ca.a + w1 * cb.a + w2 * cc.a};
                break;
            }

            float *dst = &v->fb[((size_t)sy * v->sw + sx) * 4];
            /* Premultiplied compositing. SRC_OVER is the default; the other
             * three modes mirror the VkBlendFactor choices in renderer.c so
             * CPU and GPU output stay pixel-equivalent for the canvas test
             * suite. STENCIL_WRITE is colour-write-masked off above. */
            switch (blend) {
            case FLUX_BLEND_SRC:
                dst[0] = frag.r;
                dst[1] = frag.g;
                dst[2] = frag.b;
                dst[3] = frag.a;
                break;
            case FLUX_BLEND_PLUS:
                dst[0] = frag.r + dst[0];
                dst[1] = frag.g + dst[1];
                dst[2] = frag.b + dst[2];
                dst[3] = frag.a + dst[3];
                break;
            case FLUX_BLEND_MULTIPLY: {
                /* dst' = src*dst + (1-src.a)*dst (premultiplied). */
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
static void raster_glyph_quad(flux_cpu_canvas *v, flux_recti clip, const flux_canvas_vertex *verts,
                              const uint8_t *atlas, uint32_t aw, uint32_t ah) {
    const float ss = (float)FLUX_CPU_SS;
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

    int cx0 = clip.x * FLUX_CPU_SS, cy0 = clip.y * FLUX_CPU_SS;
    int cx1 = (clip.x + (int)clip.w) * FLUX_CPU_SS, cy1 = (clip.y + (int)clip.h) * FLUX_CPU_SS;

    int sx0 = (int)floorf(minx * ss);
    int sy0 = (int)floorf(miny * ss);
    int sx1 = (int)ceilf(maxx * ss);
    int sy1 = (int)ceilf(maxy * ss);
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
    if (sx1 > (int)v->sw)
        sx1 = (int)v->sw;
    if (sy1 > (int)v->sh)
        sy1 = (int)v->sh;

    vec4f tint = unpack_premul(verts[0].color);

    /* Triangle A = 0,1,2 ; Triangle B = 3,4,5. Precompute SS-space verts. */
    const flux_canvas_vertex *ta[3] = {&verts[0], &verts[1], &verts[2]};
    const flux_canvas_vertex *tb[3] = {&verts[3], &verts[4], &verts[5]};
    float tax[3], tay[3], tbx[3], tby[3];
    for (int i = 0; i < 3; i++) {
        tax[i] = ta[i]->pos[0] * ss;
        tay[i] = ta[i]->pos[1] * ss;
        tbx[i] = tb[i]->pos[0] * ss;
        tby[i] = tb[i]->pos[1] * ss;
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

            /* premultiplied tint × coverage, SRC_OVER (one sample). */
            float *dst = &v->fb[((size_t)sy * v->sw + sx) * 4];
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

static flux_result cpu_canvas_init(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    if (c->fb_width == 0 || c->fb_height == 0 || c->fb_width > UINT32_MAX / FLUX_CPU_SS ||
        c->fb_height > UINT32_MAX / FLUX_CPU_SS) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "CPU canvas dimensions overflow sample buffer");
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    uint32_t sw = c->fb_width * FLUX_CPU_SS;
    uint32_t sh = c->fb_height * FLUX_CPU_SS;
    if ((size_t)sw > SIZE_MAX / (size_t)sh / 4u / sizeof(float)) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "CPU canvas framebuffer size overflow");
        return FLUX_ERROR_OUT_OF_RANGE;
    }

    flux_cpu_canvas *v = calloc(1, sizeof(*v));
    if (!v)
        return FLUX_ERROR_OUT_OF_MEMORY;
    v->width = c->fb_width;
    v->height = c->fb_height;
    v->sw = sw;
    v->sh = sh;
    v->fb = calloc((size_t)v->sw * v->sh * 4, sizeof(float));
    if (!v->fb) {
        free(v);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    c->backend_data = v;
    return FLUX_OK;
}

static void cpu_canvas_destroy(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    flux_cpu_canvas *v = cpu(c);
    if (!v)
        return;
    free(v->fb);
    free(v->rgba8);
    free(v);
    c->backend_data = nullptr;
}

static flux_result cpu_begin_pass(const flux_canvas_backend *self, flux_canvas *c, flux_frame *f,
                                  flux_image *target, const flux_color *clear) {
    (void)self;
    (void)f;
    if (target) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "CPU canvas has no offscreen target (v1)");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_cpu_canvas *v = cpu(c);

    vec4f cc = {0, 0, 0, 0};
    if (clear)
        cc = unpack_premul(*clear);
    size_t n = (size_t)v->sw * v->sh;
    for (size_t i = 0; i < n; ++i) {
        v->fb[i * 4 + 0] = cc.r;
        v->fb[i * 4 + 1] = cc.g;
        v->fb[i * 4 + 2] = cc.b;
        v->fb[i * 4 + 3] = cc.a;
    }

    c->pass_active = true;
    c->target_pass = false;
    c->target = nullptr;
    c->stencil_available = false; /* fills fall back to ear-clip only */
    c->fb_width = v->width;
    c->fb_height = v->height;
    c->states[0].scissor = (flux_recti){0, 0, v->width, v->height};
    return FLUX_OK;
}

static void cpu_end_pass(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    c->pass_active = false; /* pixels are already resolved in the framebuffer */
}

static void cpu_set_scissor(const flux_canvas_backend *self, flux_canvas *c, flux_recti clip) {
    (void)self;
    (void)c;
    (void)clip; /* submit reads c->states[top].scissor directly */
}

static bool cpu_bind_program(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id) {
    (void)self;
    (void)c;
    (void)id;
    return true; /* no pipeline objects; submit switches on id */
}

static void cpu_submit(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id,
                       const flux_canvas_push *push, const flux_canvas_vertex *verts,
                       uint32_t vertex_count) {
    (void)self;
    if (!c->recording || vertex_count < 3)
        return;

    flux_cpu_canvas *v = cpu(c);
    flux_recti clip = c->states[c->state_top].scissor;

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
        raster_tri(v, clip, id, push, c->pending_blend, &verts[i], &verts[i + 1],
                   &verts[i + 2]);
}

static const uint8_t *cpu_read_pixels(const flux_canvas_backend *self, flux_canvas *c,
                                      uint32_t *width, uint32_t *height, uint32_t *stride) {
    (void)self;
    flux_cpu_canvas *v = cpu(c);
    if (!v->rgba8) {
        if ((size_t)v->width > SIZE_MAX / (size_t)v->height / 4u)
            return nullptr;
        v->rgba8 = malloc((size_t)v->width * v->height * 4);
        if (!v->rgba8)
            return nullptr;
    }
    /* Box-downsample the SS x SS sample block per output pixel. */
    const float norm = 1.0f / (float)(FLUX_CPU_SS * FLUX_CPU_SS);
    for (uint32_t y = 0; y < v->height; ++y) {
        for (uint32_t x = 0; x < v->width; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (uint32_t sy = 0; sy < FLUX_CPU_SS; ++sy) {
                for (uint32_t sx = 0; sx < FLUX_CPU_SS; ++sx) {
                    const float *s =
                        &v->fb[(((size_t)(y * FLUX_CPU_SS + sy)) * v->sw + (x * FLUX_CPU_SS + sx)) *
                               4];
                    acc[0] += s[0];
                    acc[1] += s[1];
                    acc[2] += s[2];
                    acc[3] += s[3];
                }
            }
            uint8_t *o = &v->rgba8[((size_t)y * v->width + x) * 4];
            for (int k = 0; k < 4; ++k)
                o[k] = (uint8_t)(clampf(acc[k] * norm, 0.0f, 1.0f) * 255.0f + 0.5f);
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

static const flux_canvas_backend cpu_backend = {
    .name = "cpu",
    .canvas_init = cpu_canvas_init,
    .canvas_destroy = cpu_canvas_destroy,
    .begin_pass = cpu_begin_pass,
    .end_pass = cpu_end_pass,
    .set_scissor = cpu_set_scissor,
    .bind_program = cpu_bind_program,
    .submit = cpu_submit,
    .read_pixels = cpu_read_pixels,
};

const flux_canvas_backend *flux_canvas_backend_cpu(void) {
    return &cpu_backend;
}

/* ------------------------------------------------------------------ */
/*  Public headless-canvas API (flux/canvas_cpu.h)                    */
/* ------------------------------------------------------------------ */

flux_result flux_canvas_create_cpu(uint32_t width, uint32_t height, float scale,
                                   flux_canvas **out) {
    if (!out || width == 0 || height == 0)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;

    flux_canvas *c = calloc(1, sizeof(*c));
    if (!c)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&c->ref_count, 1u);
    c->device = nullptr;
    c->surface = nullptr;
    c->backend = flux_canvas_backend_cpu();
    c->content_scale = (scale > 0.0f) ? scale : 1.0f;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);
    c->state_top = 0;
    c->fb_width = width;
    c->fb_height = height;

    flux_result r = c->backend->canvas_init(c->backend, c);
    if (r != FLUX_OK) {
        free(c);
        return r;
    }
    if (!flux_canvas_alloc_scratch(c)) {
        flux_canvas_free_scratch(c);
        c->backend->canvas_destroy(c->backend, c);
        free(c);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    *out = c;
    return FLUX_OK;
}

/* CPU-spelled convenience wrappers over the unified pass/readback API. */
flux_result flux_canvas_cpu_begin(flux_canvas *c, const flux_color *clear) {
    if (!c || c->backend != flux_canvas_backend_cpu())
        return FLUX_ERROR_INVALID_ARGUMENT;
    return flux_canvas_begin_frame(c, nullptr, clear);
}

void flux_canvas_cpu_end(flux_canvas *c) {
    flux_canvas_end_frame(c);
}

const uint8_t *flux_canvas_cpu_pixels(const flux_canvas *c, uint32_t *width, uint32_t *height,
                                      uint32_t *stride) {
    return flux_canvas_read_pixels((flux_canvas *)c, width, height, stride);
}
