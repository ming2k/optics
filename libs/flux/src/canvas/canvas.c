/*
 * Canvas — lifecycle, state stack, and public draw entry points.
 *
 * Heavy geometry work (flattening, tessellation, stroking) lives in
 * geometry_*.c; pipeline caching and draw submission live in renderer.c.
 */
#include "backend.h"
#include "internal.h"

#include <flux/canvas_cpu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/* Allocation shims: a GPU canvas uses the device's tracked allocator; a
 * headless CPU canvas (device == NULL) falls back to the C allocator. Both
 * return zeroed memory. Shared by the GPU and CPU constructors. */
void *flux_canvas_alloc(flux_device *d, size_t n) {
    if (d)
        return flux_internal_alloc(d, n);
    return calloc(1, n);
}
void flux_canvas_free(flux_device *d, void *p) {
    if (d)
        flux_internal_free(d, p);
    else
        free(p);
}

bool flux_canvas_alloc_scratch(flux_canvas *c) {
    flux_device *d = c->device;
    c->scratch_pts = flux_canvas_alloc(d, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_pts));
    c->scratch_verts =
        flux_canvas_alloc(d, FLUX_CANVAS_PATH_SCRATCH_CAP * 3 * sizeof(*c->scratch_verts));
    c->scratch_contours =
        flux_canvas_alloc(d, FLUX_CANVAS_MAX_CONTOURS * sizeof(*c->scratch_contours));
    c->scratch_lnk_prev =
        flux_canvas_alloc(d, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_lnk_prev));
    c->scratch_lnk_next =
        flux_canvas_alloc(d, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_lnk_next));
    c->scratch_frames =
        flux_canvas_alloc(d, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_frames));
    return c->scratch_pts && c->scratch_verts && c->scratch_contours && c->scratch_lnk_prev &&
           c->scratch_lnk_next && c->scratch_frames;
}

void flux_canvas_free_scratch(flux_canvas *c) {
    flux_device *d = c->device;
    flux_canvas_free(d, c->scratch_pts);
    flux_canvas_free(d, c->scratch_verts);
    flux_canvas_free(d, c->scratch_contours);
    flux_canvas_free(d, c->scratch_lnk_prev);
    flux_canvas_free(d, c->scratch_lnk_next);
    flux_canvas_free(d, c->scratch_frames);
}

flux_result flux_canvas_create(const flux_canvas_desc *desc, flux_canvas **out) {
    if (!desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_CANVAS_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_CANVAS_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    /* Backend selection (Skia SkSurface-style factory). AUTO => GPU when a
     * surface is given, else the software backend. */
    flux_canvas_backend_kind kind = desc->backend;
    if (kind == FLUX_CANVAS_BACKEND_AUTO)
        kind = desc->surface ? FLUX_CANVAS_BACKEND_GPU : FLUX_CANVAS_BACKEND_CPU;
    if (kind == FLUX_CANVAS_BACKEND_CPU)
        return flux_canvas_create_cpu(desc->width, desc->height, desc->scale, out);

    if (!desc->surface) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "GPU canvas requires a surface");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_device *device = desc->surface->device;
    flux_canvas *c = flux_internal_alloc(device, sizeof(*c));
    if (!c)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&c->ref_count, 1u);
    c->device = flux_device_retain(device);
    c->surface = flux_surface_retain(desc->surface);
    c->backend = flux_canvas_backend_vk();

    /* Content scale (device-pixel ratio); the base transform applies it. */
    c->content_scale = (desc->scale > 0.0f) ? desc->scale : 1.0f;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);
    c->state_top = 0;

    /* Backend-private GPU state: pipeline-cache warm, colour/stencil formats,
     * and the owned MSAA/stencil targets. Kept entirely inside the backend so
     * struct flux_canvas carries no Vulkan types. */
    flux_result br = c->backend->canvas_init(c->backend, c);
    if (br != FLUX_OK) {
        flux_surface_release(c->surface);
        flux_device_release(c->device);
        flux_internal_free(device, c);
        return br;
    }

    if (!flux_canvas_alloc_scratch(c)) {
        flux_canvas_free_scratch(c);
        c->backend->canvas_destroy(c->backend, c);
        flux_surface_release(c->surface);
        flux_device_release(c->device);
        flux_internal_free(device, c);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    *out = c;
    return FLUX_OK;
}

void flux_canvas_destroy(flux_canvas *c) {
    if (!c)
        return;
    /* Recorded segments retain images; release them before the backend
     * (and its image teardown) goes away. */
    canvas_record_pool_destroy(c);
    /* Backend owns all GPU resources (and any in-flight-frame wait). */
    c->backend->canvas_destroy(c->backend, c);

    flux_device *dev = c->device;
    flux_canvas_free_scratch(c);
    if (c->surface)
        flux_surface_release(c->surface);
    flux_canvas_free(dev, c);
    if (dev)
        flux_device_release(dev);
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

static flux_result canvas_begin_pass_impl(flux_canvas *c, flux_frame *f, flux_image *target,
                                          const flux_canvas_pass_desc *desc) {
    if (!c || !desc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_CANVAS_PASS_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_CANVAS_PASS_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->antialias != FLUX_CANVAS_ANTIALIAS_AUTO &&
        desc->antialias != FLUX_CANVAS_ANTIALIAS_NONE &&
        desc->antialias != FLUX_CANVAS_ANTIALIAS_MSAA_4X) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "invalid Canvas antialias mode");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->antialias == FLUX_CANVAS_ANTIALIAS_MSAA_4X && !desc->clear_color) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "4x MSAA Canvas pass requires a clear colour");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (c->recording) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "Canvas pass begun twice without end");
        return FLUX_ERROR_INVALID_STATE;
    }
    /* A GPU canvas records into a frame's command buffer; a CPU canvas has
     * none. `c->device != NULL` distinguishes them. */
    if (c->device && !f) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "GPU canvas requires an open frame");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    c->frame = f;
    c->state_top = 0;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);

    flux_result r = c->backend->begin_pass(c->backend, c, f, target, desc->clear_color,
                                           desc->antialias);
    if (r != FLUX_OK) {
        c->frame = nullptr;
        return r;
    }
    c->recording = true;
    return FLUX_OK;
}

flux_result flux_canvas_begin_pass(flux_canvas *c, flux_frame *f,
                                   const flux_canvas_pass_desc *desc) {
    return canvas_begin_pass_impl(c, f, nullptr, desc);
}

flux_result flux_canvas_begin_frame(flux_canvas *c, flux_frame *f, const flux_color *clear) {
    flux_canvas_pass_desc desc = FLUX_CANVAS_PASS_DESC_INIT;
    desc.clear_color = clear;
    return flux_canvas_begin_pass(c, f, &desc);
}

void flux_canvas_end_frame(flux_canvas *c) {
    if (!c || !c->recording)
        return;
    c->backend->end_pass(c->backend, c);
    c->frame = nullptr;
    c->recording = false;
}

/* GPU-spelled compatibility wrappers. */
flux_result flux_canvas_begin(flux_canvas *c, flux_frame *f, const flux_color *clear) {
    if (c && !f) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_canvas_begin requires a frame");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    return flux_canvas_begin_frame(c, f, clear);
}

void flux_canvas_end(flux_canvas *c) {
    flux_canvas_end_frame(c);
}

const uint8_t *flux_canvas_read_pixels(flux_canvas *c, uint32_t *width, uint32_t *height,
                                       uint32_t *stride) {
    if (!c || !c->backend->read_pixels)
        return nullptr;
    return c->backend->read_pixels(c->backend, c, width, height, stride);
}

/* ------------------------------------------------------------------ */
/*  Render-target capture (ADR-0017)                                  */
/* ------------------------------------------------------------------ */

flux_result flux_canvas_begin_target(flux_canvas *c, flux_frame *f, flux_image *target,
                                     const flux_color *clear) {
    flux_canvas_pass_desc desc = FLUX_CANVAS_PASS_DESC_INIT;
    desc.clear_color = clear;
    return flux_canvas_begin_target_pass(c, f, target, &desc);
}

flux_result flux_canvas_begin_target_pass(flux_canvas *c, flux_frame *f, flux_image *target,
                                          const flux_canvas_pass_desc *desc) {
    if (!c || !f || !target)
        return FLUX_ERROR_INVALID_ARGUMENT;
    return canvas_begin_pass_impl(c, f, target, desc);
}

void flux_canvas_end_target(flux_canvas *c) {
    if (!c || !c->target_pass)
        return;
    /* end_pass emits the trailing COLOR_ATTACHMENT -> SHADER_READ transition
     * so a following flux_effect_blur / draw_image needs no caller sync. */
    c->backend->end_pass(c->backend, c);
    c->target = nullptr;
    c->target_pass = false;
    c->frame = nullptr;
    c->recording = false;
    /* The capture session is closed; a subsequent canvas_begin or
     * begin_target opens a fresh session on a (possibly different) frame. */
}

/* ------------------------------------------------------------------ */
/*  State stack                                                       */
/* ------------------------------------------------------------------ */

void flux_canvas_save(flux_canvas *c) {
    if (!c)
        return;
    if (c->state_top + 1 >= FLUX_CANVAS_MAX_STATES) {
        /* Stack overflow: previously this was a silent no-op, leaving
         * the caller's save/restore pairing stranded with no diagnostic.
         * Surface the failure through both the device logger (visible
         * in any flux-log-equipped app) and the thread-local error
         * slot, so the caller can detect via flux_get_last_error. */
        if (c->device && c->device->log) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "flux_canvas_save: state stack overflow (depth > %d); "
                     "save was a no-op — check for unbalanced save/restore",
                     FLUX_CANVAS_MAX_STATES);
            c->device->log(FLUX_LOG_ERROR, "flux_canvas_save", 0, "%s", buf, c->device->log_user);
        }
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "flux_canvas_save: state stack overflow");
        return;
    }
    c->states[c->state_top + 1] = c->states[c->state_top];
    c->state_top++;
}

void flux_canvas_restore(flux_canvas *c) {
    if (!c || c->state_top == 0)
        return;
    c->state_top--;
    c->backend->set_scissor(c->backend, c, c->states[c->state_top].scissor);
}

void flux_canvas_translate(flux_canvas *c, float x, float y) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_translate(x, y));
}

void flux_canvas_scale(flux_canvas *c, float sx, float sy) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_scale(sx, sy));
}

void flux_canvas_rotate(flux_canvas *c, float radians) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_rotate(radians));
}

void flux_canvas_set_scale(flux_canvas *c, float scale) {
    if (!c)
        return;
    c->content_scale = (scale > 0.0f) ? scale : 1.0f;
    /* If set between begin/end, refresh the base transform so the change
     * takes effect this frame (callers usually set it before begin). */
    if (c->recording && c->state_top == 0)
        c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);
}

float flux_canvas_get_scale(const flux_canvas *c) {
    if (!c)
        return 1.0f;
    /* The *effective* scale: the active transform's pixel scale, so callers
     * (e.g. flux_text) rasterise to match whether the scale comes from the
     * content-scale base transform or a manual flux_canvas_scale on top. */
    return flux_canvas_mat3x2_pixel_scale(c->states[c->state_top].transform);
}

void flux_canvas_transform(flux_canvas *c, flux_mat3x2 m) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, m);
}

void flux_canvas_clip_rect(flux_canvas *c, flux_rect r) {
    if (!c)
        return;

    flux_canvas_state *state = &c->states[c->state_top];
    flux_rect transformed =
        flux_mat3x2_transform_rect(state->transform, r);
    flux_recti current = state->scissor;
    int64_t current_left = current.x;
    int64_t current_top = current.y;
    int64_t current_right = current_left + (int64_t)current.w;
    int64_t current_bottom = current_top + (int64_t)current.h;
    flux_recti sc = {current.x, current.y, 0, 0};

    if (isfinite(transformed.x) && isfinite(transformed.y)
        && isfinite(transformed.w) && isfinite(transformed.h)
        && transformed.w > 0.0f && transformed.h > 0.0f) {
        double left_d = floor((double)transformed.x);
        double top_d = floor((double)transformed.y);
        double right_d =
            ceil((double)transformed.x + (double)transformed.w);
        double bottom_d =
            ceil((double)transformed.y + (double)transformed.h);
        int64_t left = current_left;
        int64_t top = current_top;
        int64_t right = current_right;
        int64_t bottom = current_bottom;

        if (left_d > (double)left)
            left = left_d < (double)current_right
                       ? (int64_t)left_d
                       : current_right;
        if (top_d > (double)top)
            top = top_d < (double)current_bottom
                      ? (int64_t)top_d
                      : current_bottom;
        if (right_d < (double)right)
            right = right_d > (double)current_left
                        ? (int64_t)right_d
                        : current_left;
        if (bottom_d < (double)bottom)
            bottom = bottom_d > (double)current_top
                         ? (int64_t)bottom_d
                         : current_top;

        if (right > left && bottom > top) {
            sc.x = (int32_t)left;
            sc.y = (int32_t)top;
            sc.w = (uint32_t)(right - left);
            sc.h = (uint32_t)(bottom - top);
        }
    }

    state->scissor = sc;
    c->backend->set_scissor(c->backend, c, sc);
}

/* ------------------------------------------------------------------ */
/*  Public draws                                                      */
/* ------------------------------------------------------------------ */

void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_paint *paint) {
    if (!c || !c->recording)
        return;
    flux_mat3x2 tx = c->states[c->state_top].transform;
    flux_color vc = paint ? paint->color : 0xFF000000u;

    flux_point p0 = {r.x, r.y};
    flux_point p1 = {r.x + r.w, r.y};
    flux_point p2 = {r.x + r.w, r.y + r.h};
    flux_point p3 = {r.x, r.y + r.h};

    flux_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, vc);
    push_vertex(&v[1], p1, tx, vc);
    push_vertex(&v[2], p2, tx, vc);
    push_vertex(&v[3], p0, tx, vc);
    push_vertex(&v[4], p2, tx, vc);
    push_vertex(&v[5], p3, tx, vc);
    submit_triangles(c, paint, v, 6);
}

void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color) {
    flux_paint p = flux_paint_default();
    p.color = color;
    flux_canvas_fill_rect(c, r, &p);
}

/* Pack a normalised UV pair into the vertex `_pad` field as unorm16x2.
 * canvas_solid.vert expands it into v_uv for both image quads and glyphs. */
static uint32_t pack_uv(float u, float v) {
    uint32_t pu = (uint32_t)(fminf(fmaxf(u, 0.0f), 1.0f) * 65535.0f + 0.5f);
    uint32_t pv = (uint32_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 65535.0f + 0.5f);
    return pu | (pv << 16);
}

static void *frame_retain_image(void *resource) {
    return flux_image_retain(resource);
}

static void frame_release_image(void *resource) {
    flux_image_release(resource);
}

bool canvas_track_foreign_image(flux_canvas *c, flux_image *img) {
    if (!c || !img || !img->imported_memory)
        return true;
    return flux_frame_track_foreign_image(c->frame, img->image, img, frame_retain_image,
                                          frame_release_image, &img->foreign_owned);
}

static void draw_image_with_sampler_handle(flux_canvas *c, flux_image *img, flux_sampler *sampler,
                                           flux_bindless_handle sh, flux_rect dst, flux_rect src,
                                           flux_color tint, uint32_t kind, float radius) {
    /* Image draws need a GPU-resident texture (img->bindless): unsupported on
     * a headless CPU canvas. */
    if (!c->device || sh == FLUX_BINDLESS_INVALID)
        return;
    if (!canvas_track_foreign_image(c, img))
        return;

    flux_mat3x2 tx = c->states[c->state_top].transform;
    flux_point p0 = {dst.x, dst.y};
    flux_point p1 = {dst.x + dst.w, dst.y};
    flux_point p2 = {dst.x + dst.w, dst.y + dst.h};
    flux_point p3 = {dst.x, dst.y + dst.h};

    /* UVs live on the vertices instead of being reconstructed from the
     * destination's screen-space AABB. That keeps sampling correct under
     * every affine canvas transform, including rotation and skew. The colour
     * carries a premultiplied tint for plain images and coverage glyphs. */
    flux_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, tint);
    push_vertex(&v[1], p1, tx, tint);
    push_vertex(&v[2], p2, tx, tint);
    push_vertex(&v[3], p0, tx, tint);
    push_vertex(&v[4], p2, tx, tint);
    push_vertex(&v[5], p3, tx, tint);
    v[0]._pad = pack_uv(0.0f, 0.0f);
    v[1]._pad = pack_uv(1.0f, 0.0f);
    v[2]._pad = pack_uv(1.0f, 1.0f);
    v[3]._pad = pack_uv(0.0f, 0.0f);
    v[4]._pad = pack_uv(1.0f, 1.0f);
    v[5]._pad = pack_uv(0.0f, 1.0f);

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    pc.kind = kind;
    pc.image_handle = img->bindless;
    pc.sampler_handle = sh;
    pc.image_src[0] = src.x;
    pc.image_src[1] = src.y;
    pc.image_src[2] = src.w;
    pc.image_src[3] = src.h;
    if (kind == 5u) {
        float scale = flux_canvas_mat3x2_pixel_scale(tx);
        flux_point center = {dst.x + dst.w * 0.5f, dst.y + dst.h * 0.5f};
        flux_point transformed = flux_mat3x2_transform_point(tx, center);
        pc.image_dst[0] = transformed.x;
        pc.image_dst[1] = transformed.y;
        pc.image_dst[2] = dst.w * 0.5f * scale;
        pc.image_dst[3] = dst.h * 0.5f * scale;
        pc.grad_radius = fminf(fmaxf(radius * scale, 0.0f),
                               fminf(pc.image_dst[2], pc.image_dst[3]));
    }

    canvas_record_retain_image(c, img);
    canvas_record_retain_sampler(c, sampler);
    canvas_emit(c, CANVAS_PIPE_IMAGE, &pc, v, 6);
}

/* ------------------------------------------------------------------ */
/*  SDF rounded rectangles / circles                                  */
/* ------------------------------------------------------------------ */

/* Draw a rounded rect (or, with stroke_hw > 0, its ring border) through the
 * SDF pipeline: resolution-independent analytic AA. The shape is evaluated in
 * screen-pixel space (translation + uniform scale; rotation is not modelled,
 * which suits axis-aligned UI). */
static void draw_sdf_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color,
                           float stroke_hw) {
    if (!c || !c->recording || r.w <= 0.0f || r.h <= 0.0f)
        return;

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float s = flux_canvas_mat3x2_pixel_scale(tx);

    flux_point center = {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    flux_point cp = flux_mat3x2_transform_point(tx, center);
    float hx = r.w * 0.5f * s;
    float hy = r.h * 0.5f * s;
    float rad = radius * s;
    float max_r = fminf(hx, hy);
    if (rad > max_r)
        rad = max_r;
    if (rad < 0.0f)
        rad = 0.0f;
    float hw = stroke_hw > 0.0f ? stroke_hw * s : 0.0f;

    /* Quad covers the bbox plus the AA fringe and (when stroking) the ring's
     * outer half-width. Built directly in screen space — the vertex shader
     * only maps pixels to NDC. */
    float m = 1.5f + hw;
    float x0 = cp.x - hx - m, y0 = cp.y - hy - m;
    float x1 = cp.x + hx + m, y1 = cp.y + hy + m;

    flux_canvas_vertex v[6];
    const flux_point quad[6] = {
        {x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1},
    };
    for (int i = 0; i < 6; ++i) {
        v[i].pos[0] = quad[i].x;
        v[i].pos[1] = quad[i].y;
        v[i].color = color;
        v[i]._pad = 0;
    }

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    /* SDF params share the image_dst / image_src push slots (screen pixels). */
    pc.image_dst[0] = cp.x;
    pc.image_dst[1] = cp.y;
    pc.image_dst[2] = hx;
    pc.image_dst[3] = hy;
    pc.image_src[0] = rad;
    pc.image_src[1] = hw;
    pc.image_src[2] = 0.0f;
    pc.image_src[3] = 0.0f;

    canvas_emit(c, CANVAS_PIPE_SDF, &pc, v, 6);
}

void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color) {
    draw_sdf_rrect(c, r, radius, color, 0.0f);
}

void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color,
                              float width) {
    if (width <= 0.0f)
        width = 1.0f;
    draw_sdf_rrect(c, r, radius, color, width * 0.5f);
}

/* Whole-image sub-rect: sample the entire texture across dst. */
static const flux_rect FLUX_SRC_WHOLE = {0.0f, 0.0f, 1.0f, 1.0f};

void flux_canvas_draw_image(flux_canvas *c, flux_image *img, flux_rect dst,
                            const flux_paint *paint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    flux_color tint = paint ? paint->color : flux_color_rgba_premul(255, 255, 255, 255);
    draw_image_with_sampler_handle(c, img, NULL, sh, dst, FLUX_SRC_WHOLE, tint, 3u, 0.0f);
}

void flux_canvas_draw_image_rrect(flux_canvas *c, flux_image *img, flux_rect dst, float radius,
                                  const flux_paint *paint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    flux_color tint = paint ? paint->color : flux_color_rgba_premul(255, 255, 255, 255);
    draw_image_with_sampler_handle(c, img, NULL, sh, dst, FLUX_SRC_WHOLE, tint, 5u, radius);
}

void flux_canvas_draw_image_sub(flux_canvas *c, flux_image *img, flux_rect dst, flux_rect src) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, NULL, sh, dst, src,
                                   flux_color_rgba_premul(255, 255, 255, 255), 3u, 0.0f);
}

void flux_canvas_draw_image_sampled(flux_canvas *c, flux_image *img, flux_sampler *sampler,
                                    flux_rect dst, const flux_paint *paint) {
    if (!c || !c->recording || !img || !sampler)
        return;
    flux_bindless_handle sh = flux_sampler_bindless_handle(sampler);
    flux_color tint = paint ? paint->color : flux_color_rgba_premul(255, 255, 255, 255);
    draw_image_with_sampler_handle(c, img, sampler, sh, dst, FLUX_SRC_WHOLE, tint, 3u, 0.0f);
}

void flux_canvas_draw_image_coverage(flux_canvas *c, flux_image *img, flux_rect dst,
                                     flux_color tint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, NULL, sh, dst, FLUX_SRC_WHOLE, tint, 4u, 0.0f);
}

void flux_canvas_draw_image_coverage_sub(flux_canvas *c, flux_image *img, flux_rect dst,
                                         flux_rect src, flux_color tint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, NULL, sh, dst, src, tint, 4u, 0.0f);
}

/* ------------------------------------------------------------------ */
/*  Glyph runs (ADR-0010)                                             */
/* ------------------------------------------------------------------ */

void flux_canvas_draw_glyph_run(flux_canvas *c, const flux_glyph_run_desc *desc) {
    if (!c || !c->recording || !desc)
        return;
    if (desc->type != FLUX_TYPE_GLYPH_RUN_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_GLYPH_RUN_DESC");
        return;
    }
    if (desc->quad_count == 0)
        return;
    if (!desc->quads) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "glyph run needs quads");
        return;
    }

    /* Two atlas sources, mutually exclusive (ADR-0019):
     *   - GPU:   `atlas` (flux_image, bindless). Requires a device.
     *   - Host:  `host_coverage` (R8 buffer). Device-less CPU canvas. */
    const bool host = (desc->host_coverage != NULL && desc->host_atlas_w > 0 && desc->host_atlas_h > 0);

    flux_bindless_handle sh = FLUX_BINDLESS_INVALID;
    uint32_t atlas_w = 0, atlas_h = 0;

    if (host) {
        if (c->device) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                      "host_coverage is for device-less canvases; use atlas on a GPU canvas");
            return;
        }
        atlas_w = desc->host_atlas_w;
        atlas_h = desc->host_atlas_h;
        c->pending_host_atlas = desc->host_coverage;
        c->pending_host_atlas_w = atlas_w;
        c->pending_host_atlas_h = atlas_h;
    } else {
        /* GPU path needs a device + a bindless atlas image. */
        if (!c->device || !desc->atlas)
            return;
        sh = desc->sampler ? flux_sampler_bindless_handle(desc->sampler)
                           : flux_device_default_sampler_handle(c->device);
        if (sh == FLUX_BINDLESS_INVALID || desc->atlas->bindless == FLUX_BINDLESS_INVALID)
            return;
        atlas_w = desc->atlas->width;
        atlas_h = desc->atlas->height;
        if (!canvas_track_foreign_image(c, desc->atlas))
            return;
    }

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float inv_w = 1.0f / (float)atlas_w;
    float inv_h = 1.0f / (float)atlas_h;

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    if (!host) {
        pc.image_handle = desc->atlas->bindless;
        pc.sampler_handle = sh;
    }

    /* Chunked so a run of any length works within the scratch vertex
     * buffer; each chunk is still one draw. */
    const uint32_t max_quads = (FLUX_CANVAS_PATH_SCRATCH_CAP * 3) / 6;
    for (uint32_t base = 0; base < desc->quad_count; base += max_quads) {
        uint32_t n = desc->quad_count - base;
        if (n > max_quads)
            n = max_quads;

        flux_canvas_vertex *verts = c->scratch_verts;
        uint32_t v_count = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const flux_glyph_quad *q = &desc->quads[base + i];
            float u0 = (float)q->ax * inv_w, v0 = (float)q->ay * inv_h;
            float u1 = (float)(q->ax + q->aw) * inv_w;
            float v1 = (float)(q->ay + q->ah) * inv_h;

            flux_point p0 = {q->sx, q->sy};
            flux_point p1 = {q->sx + q->sw, q->sy};
            flux_point p2 = {q->sx + q->sw, q->sy + q->sh};
            flux_point p3 = {q->sx, q->sy + q->sh};

            push_vertex(&verts[v_count], p0, tx, q->color);
            push_vertex(&verts[v_count + 1], p1, tx, q->color);
            push_vertex(&verts[v_count + 2], p2, tx, q->color);
            push_vertex(&verts[v_count + 3], p0, tx, q->color);
            push_vertex(&verts[v_count + 4], p2, tx, q->color);
            push_vertex(&verts[v_count + 5], p3, tx, q->color);
            verts[v_count]._pad = pack_uv(u0, v0);
            verts[v_count + 1]._pad = pack_uv(u1, v0);
            verts[v_count + 2]._pad = pack_uv(u1, v1);
            verts[v_count + 3]._pad = pack_uv(u0, v0);
            verts[v_count + 4]._pad = pack_uv(u1, v1);
            verts[v_count + 5]._pad = pack_uv(u0, v1);
            v_count += 6;
        }

        canvas_emit(c, CANVAS_PIPE_GLYPH, &pc, verts, v_count);
    }

    if (!host) {
        canvas_record_retain_image(c, desc->atlas);
        canvas_record_retain_sampler(c, desc->sampler);
    }

    if (host) {
        /* Drop the borrow so a subsequent non-glyph draw never sees a stale
         * host atlas pointer. */
        c->pending_host_atlas = NULL;
        c->pending_host_atlas_w = 0;
        c->pending_host_atlas_h = 0;
    }
}

uint64_t flux_canvas_dropped_draws(const flux_canvas *c) {
    return c ? c->dropped_draws : 0;
}

uint64_t flux_canvas_submit_calls(const flux_canvas *c) {
    return c ? c->submit_calls : 0;
}

uint64_t flux_canvas_recorded_draws(const flux_canvas *c) {
    return c ? c->recorded_draws : 0;
}
