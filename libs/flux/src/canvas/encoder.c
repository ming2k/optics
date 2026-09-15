/* Owned, relocatable command capture. Native payloads are private in-process
 * data, never a serialization format. Published lists contain no pointers into
 * recorder storage; views are reconstructed only for the duration of replay. */
#include "backend.h"
#include <flux/canvas_helpers.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef enum flux_cmd_kind : uint8_t {
    FLUX_CMD_DRAW_GEOM = 1,
    FLUX_CMD_DRAW_GLYPH_RUN,
    FLUX_CMD_SAVE,
    FLUX_CMD_RESTORE,
    FLUX_CMD_CLIP_RECT,
    FLUX_CMD_SAVE_LAYER,
    FLUX_CMD_TRANSLATE,
    FLUX_CMD_SCALE,
    FLUX_CMD_ROTATE,
    FLUX_CMD_TRANSFORM,
    FLUX_CMD_CHILD,
} flux_cmd_kind;

typedef struct flux_cmd_header {
    flux_cmd_kind kind;
    uint8_t pad[3];
    uint32_t size; /* Includes alignment padding. */
} flux_cmd_header;

typedef struct flux_cmd_draw_geom {
    flux_cmd_header header;
    flux_geometry geom;
    flux_brush brush;
    uint32_t path_seg_count;
    /* Trailing flux_path_segment[], addressed relative to this command. */
} flux_cmd_draw_geom;

typedef struct flux_cmd_glyph_run {
    flux_cmd_header header;
    flux_glyph_run_desc desc;
    uint32_t quad_count;
    size_t host_cov_bytes;
    /* Trailing flux_glyph_quad[], then coverage bytes. */
} flux_cmd_glyph_run;

typedef struct {
    flux_cmd_header header;
    flux_rect rect;
} flux_cmd_clip;
typedef struct {
    flux_cmd_header header;
    flux_rect bounds;
    float opacity;
} flux_cmd_save_layer;
typedef struct {
    flux_cmd_header header;
    flux_mat3x2 matrix;
} flux_cmd_transform;
typedef struct {
    flux_cmd_header header;
    float x, y;
} flux_cmd_vec2;
typedef struct {
    flux_cmd_header header;
    float value;
} flux_cmd_scalar;

typedef struct {
    flux_cmd_header header;
    flux_display_list *list;
} flux_cmd_child;

struct flux_display_list {
    atomic_uint refs;
    uint8_t *commands;
    size_t size;
    uint32_t count;
    uint32_t max_depth, tree_depth;
};

struct flux_encoder {
    uint8_t *buffer;
    size_t capacity, used, budget;
    uint32_t count, depth, max_depth, tree_depth, command_budget;
    bool finished;
    flux_result error;
};

void flux_encoder_fail(flux_encoder *enc, flux_result error) {
    if (enc && enc->error == FLUX_OK)
        enc->error = error;
}

flux_result flux_encoder_status(const flux_encoder *enc) {
    if (!enc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (enc->error != FLUX_OK)
        return enc->error;
    return enc->finished ? FLUX_ERROR_INVALID_STATE : FLUX_OK;
}

static bool encoder_ready(flux_encoder *enc) {
    if (!enc)
        return false;
    if (enc->finished)
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_STATE);
    return enc->error == FLUX_OK;
}

static void *encoder_push(flux_encoder *enc, flux_cmd_kind kind, size_t bytes) {
    if (!encoder_ready(enc))
        return nullptr;
    if (bytes > UINT32_MAX - 7u || enc->count >= enc->command_budget) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return nullptr;
    }
    bytes = (bytes + 7u) & ~(size_t)7u;
    if (bytes > enc->budget - enc->used) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
        return nullptr;
    }
    size_t required = enc->used + bytes;
    if (required > enc->capacity) {
        size_t cap = enc->capacity ? enc->capacity : 4096u;
        while (cap < required && cap <= enc->budget / 2u)
            cap *= 2u;
        if (cap < required || cap > enc->budget)
            cap = enc->budget;
        uint8_t *buffer = realloc(enc->buffer, cap);
        if (!buffer) {
            flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
            return nullptr;
        }
        enc->buffer = buffer;
        enc->capacity = cap;
    }
    flux_cmd_header *h = (void *)(enc->buffer + enc->used);
    memset(h, 0, bytes);
    h->kind = kind;
    h->size = (uint32_t)bytes;
    enc->used += bytes;
    enc->count++;
    return h;
}

static void commands_release(uint8_t *buffer, size_t size) {
    for (size_t off = 0; off < size;) {
        const flux_cmd_header *h = (const void *)(buffer + off);
        if (h->kind == FLUX_CMD_CHILD) {
            const flux_cmd_child *cmd = (const void *)h;
            flux_display_list_release(cmd->list);
        } else if (h->kind == FLUX_CMD_DRAW_GEOM) {
            const flux_cmd_draw_geom *cmd = (const void *)h;
            if (cmd->brush.kind == FLUX_BRUSH_IMAGE_PATTERN) {
                flux_image_release(cmd->brush.image.image);
                flux_sampler_release(cmd->brush.image.sampler);
            }
        } else if (h->kind == FLUX_CMD_DRAW_GLYPH_RUN) {
            const flux_cmd_glyph_run *cmd = (const void *)h;
            flux_image_release(cmd->desc.atlas);
            flux_sampler_release(cmd->desc.sampler);
        }
        off += h->size;
    }
    free(buffer);
}

flux_result flux_encoder_create(const flux_encoder_desc *desc, flux_encoder **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    flux_encoder *enc = calloc(1, sizeof(*enc));
    if (!enc)
        return FLUX_ERROR_OUT_OF_MEMORY;
    enc->budget = desc && desc->max_bytes ? desc->max_bytes : (64u << 20);
    enc->command_budget = desc && desc->max_commands ? desc->max_commands : (1u << 20);
    *out = enc;
    return FLUX_OK;
}

void flux_encoder_destroy(flux_encoder *enc) {
    if (!enc)
        return;
    commands_release(enc->buffer, enc->used);
    free(enc);
}

static bool finite_rect(flux_rect r) {
    return isfinite(r.x) && isfinite(r.y) && isfinite(r.w) && isfinite(r.h) && r.w >= 0 &&
           r.h >= 0 && isfinite(r.x + r.w) && isfinite(r.y + r.h);
}

static bool valid_geometry(const flux_geometry *g) {
    if (!g || !isfinite(g->stroke_width) || g->stroke_width < 0)
        return false;
    switch (g->kind) {
    case FLUX_GEOM_RECT:
        return finite_rect(g->rect.rect);
    case FLUX_GEOM_RRECT:
        return finite_rect(g->rrect.rect) && isfinite(g->rrect.radius) && g->rrect.radius >= 0;
    case FLUX_GEOM_SQUIRCLE:
        return finite_rect(g->squircle.rect) && isfinite(g->squircle.radius) &&
               g->squircle.radius >= 0 && isfinite(g->squircle.curvature) &&
               g->squircle.curvature >= 0 && g->squircle.curvature <= 1;
    case FLUX_GEOM_CIRCLE:
        return isfinite(g->circle.cx) && isfinite(g->circle.cy) && isfinite(g->circle.radius) &&
               g->circle.radius >= 0;
    case FLUX_GEOM_LINE:
        return isfinite(g->line.x0) && isfinite(g->line.y0) && isfinite(g->line.x1) &&
               isfinite(g->line.y1);
    case FLUX_GEOM_PATH: {
        const flux_path *p = g->path.path;
        if (!p || p->dropped || (p->count && !p->segments))
            return false;
        for (uint32_t i = 0; i < p->count; i++) {
            const flux_path_segment *seg = &p->segments[i];
            if (seg->op > FLUX_PATH_CLOSE)
                return false;
            unsigned n = seg->op == FLUX_PATH_CLOSE   ? 0
                         : seg->op == FLUX_PATH_CUBIC ? 6
                         : seg->op == FLUX_PATH_QUAD  ? 4
                                                      : 2;
            for (unsigned j = 0; j < n; j++)
                if (!isfinite(seg->pts[j]))
                    return false;
        }
        return true;
    }
    default:
        return false;
    }
}

static bool valid_brush(const flux_brush *b) {
    if (!isfinite(b->opacity) || b->opacity < 0 || b->opacity > 1 ||
        b->blend < FLUX_BLEND_SRC_OVER || b->blend > FLUX_BLEND_MULTIPLY)
        return false;
    if (b->kind == FLUX_BRUSH_SOLID)
        return true;
    if (b->kind == FLUX_BRUSH_IMAGE_PATTERN)
        return b->image.image && finite_rect(b->image.src_rect);
    if (b->kind != FLUX_BRUSH_LINEAR_GRADIENT && b->kind != FLUX_BRUSH_RADIAL_GRADIENT)
        return false;
    const flux_brush_gradient_data *g = &b->gradient;
    if (!isfinite(g->start.x) || !isfinite(g->start.y) || !isfinite(g->end.x) ||
        !isfinite(g->end.y) || !isfinite(g->radius) || g->radius < 0 || !g->stops.count ||
        g->stops.count > FLUX_GRADIENT_MAX_STOPS)
        return false;
    float prev = 0;
    for (uint32_t i = 0; i < g->stops.count; i++) {
        float t = g->stops.stops[i].t;
        if (!isfinite(t) || t < prev || t > 1)
            return false;
        prev = t;
    }
    return true;
}

void flux_encoder_draw_geometry(flux_encoder *enc, const flux_geometry *geom,
                                const flux_brush *brush) {
    if (!encoder_ready(enc))
        return;
    flux_brush b = brush ? *brush : flux_brush_solid(0xff000000u);
    if (!valid_geometry(geom) || !valid_brush(&b)) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    uint32_t count = geom->kind == FLUX_GEOM_PATH ? geom->path.path->count : 0;
    if (count > (UINT32_MAX - sizeof(flux_cmd_draw_geom) - 7u) / sizeof(flux_path_segment)) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return;
    }
    flux_cmd_draw_geom *cmd = encoder_push(
        enc, FLUX_CMD_DRAW_GEOM, sizeof(*cmd) + (size_t)count * sizeof(flux_path_segment));
    if (!cmd)
        return;
    cmd->geom = *geom;
    cmd->brush = b;
    cmd->path_seg_count = count;
    if (geom->kind == FLUX_GEOM_PATH) {
        cmd->geom.path.path = nullptr;
        if (count)
            memcpy(cmd + 1, geom->path.path->segments, (size_t)count * sizeof(flux_path_segment));
    }
    if (b.kind == FLUX_BRUSH_IMAGE_PATTERN) {
        (void)flux_image_retain(b.image.image);
        (void)flux_sampler_retain(b.image.sampler);
    }
}

void flux_encoder_draw_glyph_run(flux_encoder *enc, const flux_glyph_run_desc *desc) {
    if (!encoder_ready(enc))
        return;
    if (!desc || desc->type != FLUX_TYPE_GLYPH_RUN_DESC || (desc->quad_count && !desc->quads) ||
        (desc->quad_count && !desc->atlas && !desc->host_coverage) ||
        (desc->host_coverage && (!desc->host_atlas_w || !desc->host_atlas_h))) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    const flux_glyph_run_host_atlas_desc *ext = desc->next;
    if (ext && (ext->type != FLUX_TYPE_GLYPH_RUN_HOST_ATLAS_DESC || ext->next)) {
        flux_encoder_fail(enc, FLUX_ERROR_UNSUPPORTED);
        return;
    }
    if (!desc->quad_count)
        return;
    size_t quads = (size_t)desc->quad_count * sizeof(flux_glyph_quad);
    uint32_t x0 = desc->host_atlas_w, y0 = desc->host_atlas_h, x1 = 0, y1 = 0;
    if (desc->host_coverage && desc->host_atlas_w > SIZE_MAX / desc->host_atlas_h) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return;
    }
    for (uint32_t i = 0; i < desc->quad_count; i++) {
        const flux_glyph_quad *q = &desc->quads[i];
        if (!finite_rect((flux_rect){q->sx, q->sy, q->sw, q->sh}) || !q->aw || !q->ah) {
            flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
            return;
        }
        if (desc->host_coverage) {
            uint32_t right = (uint32_t)q->ax + q->aw, bottom = (uint32_t)q->ay + q->ah;
            if (right > desc->host_atlas_w || bottom > desc->host_atlas_h) {
                flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
                return;
            }
            if (q->ax < x0)
                x0 = q->ax;
            if (q->ay < y0)
                y0 = q->ay;
            if (right > x1)
                x1 = right;
            if (bottom > y1)
                y1 = bottom;
        }
    }
    size_t cov = desc->host_coverage ? (size_t)(x1 - x0) * (y1 - y0) : 0;
    if (quads > UINT32_MAX - sizeof(flux_cmd_glyph_run) - 7u ||
        cov > UINT32_MAX - sizeof(flux_cmd_glyph_run) - 7u - quads) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return;
    }
    flux_cmd_glyph_run *cmd =
        encoder_push(enc, FLUX_CMD_DRAW_GLYPH_RUN, sizeof(*cmd) + quads + cov);
    if (!cmd)
        return;
    cmd->desc = *desc;
    cmd->desc.next = nullptr; /* Copied coverage has no external generation dependency. */
    cmd->desc.quads = nullptr;
    cmd->desc.host_coverage = nullptr;
    cmd->quad_count = desc->quad_count;
    cmd->host_cov_bytes = cov;
    uint8_t *payload = (void *)(cmd + 1);
    if (quads)
        memcpy(payload, desc->quads, quads);
    if (cov) {
        uint32_t width = x1 - x0, height = y1 - y0;
        cmd->desc.host_atlas_w = width;
        cmd->desc.host_atlas_h = height;
        flux_glyph_quad *owned = (void *)payload;
        for (uint32_t i = 0; i < desc->quad_count; i++) {
            owned[i].ax -= x0;
            owned[i].ay -= y0;
        }
        for (uint32_t y = 0; y < height; y++)
            memcpy(payload + quads + (size_t)y * width,
                   desc->host_coverage + (size_t)(y + y0) * desc->host_atlas_w + x0, width);
    }
    (void)flux_image_retain(desc->atlas);
    (void)flux_sampler_retain(desc->sampler);
}

void flux_encoder_save(flux_encoder *enc) {
    if (!encoder_ready(enc))
        return;
    if (enc->depth >= FLUX_CANVAS_MAX_STATES - 1u) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return;
    }
    if (encoder_push(enc, FLUX_CMD_SAVE, sizeof(flux_cmd_header))) {
        enc->depth++;
        if (enc->depth > enc->max_depth)
            enc->max_depth = enc->depth;
    }
}

void flux_encoder_restore(flux_encoder *enc) {
    if (!encoder_ready(enc))
        return;
    if (!enc->depth) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_STATE);
        return;
    }
    if (encoder_push(enc, FLUX_CMD_RESTORE, sizeof(flux_cmd_header)))
        enc->depth--;
}

void flux_encoder_clip_rect(flux_encoder *enc, flux_rect r) {
    if (!finite_rect(r)) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    flux_cmd_clip *cmd = encoder_push(enc, FLUX_CMD_CLIP_RECT, sizeof(*cmd));
    if (cmd)
        cmd->rect = r;
}

void flux_encoder_save_layer(flux_encoder *enc, const flux_rect *bounds, float opacity) {
    if (!encoder_ready(enc))
        return;
    if (!isfinite(opacity) || opacity < 0 || opacity > 1 || (bounds && !finite_rect(*bounds))) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (enc->depth >= FLUX_CANVAS_MAX_STATES - 1u) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return;
    }
    flux_cmd_save_layer *cmd = encoder_push(enc, FLUX_CMD_SAVE_LAYER, sizeof(*cmd));
    if (cmd) {
        cmd->bounds = bounds ? *bounds : (flux_rect){};
        cmd->opacity = opacity;
        enc->depth++;
        if (enc->depth > enc->max_depth)
            enc->max_depth = enc->depth;
    }
}

static void encoder_vec2(flux_encoder *enc, flux_cmd_kind kind, float x, float y) {
    if (!isfinite(x) || !isfinite(y)) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    flux_cmd_vec2 *cmd = encoder_push(enc, kind, sizeof(*cmd));
    if (cmd) {
        cmd->x = x;
        cmd->y = y;
    }
}
void flux_encoder_translate(flux_encoder *enc, float x, float y) {
    encoder_vec2(enc, FLUX_CMD_TRANSLATE, x, y);
}
void flux_encoder_scale(flux_encoder *enc, float x, float y) {
    encoder_vec2(enc, FLUX_CMD_SCALE, x, y);
}
void flux_encoder_rotate(flux_encoder *enc, float radians) {
    if (!isfinite(radians)) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    flux_cmd_scalar *cmd = encoder_push(enc, FLUX_CMD_ROTATE, sizeof(*cmd));
    if (cmd)
        cmd->value = radians;
}
void flux_encoder_transform(flux_encoder *enc, flux_mat3x2 m) {
    /* Matrix fields are validated through their point images, without aliasing
     * a struct as a float array. */
    flux_point a = flux_mat3x2_transform_point(m, (flux_point){0, 0});
    flux_point b = flux_mat3x2_transform_point(m, (flux_point){1, 1});
    if (!isfinite(a.x) || !isfinite(a.y) || !isfinite(b.x) || !isfinite(b.y)) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    flux_cmd_transform *cmd = encoder_push(enc, FLUX_CMD_TRANSFORM, sizeof(*cmd));
    if (cmd)
        cmd->matrix = m;
}

flux_result flux_encoder_finish(flux_encoder *enc, flux_display_list **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    if (!enc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (enc->finished)
        return FLUX_ERROR_INVALID_STATE;
    enc->finished = true;
    if (enc->depth)
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_STATE);
    if (enc->error != FLUX_OK)
        return enc->error;
    flux_display_list *list = calloc(1, sizeof(*list));
    if (!list) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
        return enc->error;
    }
    atomic_init(&list->refs, 1);
    list->commands = enc->buffer;
    list->size = enc->used;
    list->count = enc->count;
    list->max_depth = enc->max_depth;
    list->tree_depth = enc->tree_depth;
    enc->buffer = nullptr;
    enc->used = enc->capacity = 0;
    *out = list;
    return FLUX_OK;
}

flux_display_list *flux_display_list_retain(flux_display_list *list) {
    if (list)
        atomic_fetch_add_explicit(&list->refs, 1, memory_order_relaxed);
    return list;
}
void flux_display_list_release(flux_display_list *list) {
    if (!list || atomic_fetch_sub_explicit(&list->refs, 1, memory_order_acq_rel) != 1)
        return;
    commands_release(list->commands, list->size);
    free(list);
}
size_t flux_display_list_size(const flux_display_list *list) {
    return list ? list->size : 0;
}
uint32_t flux_display_list_command_count(const flux_display_list *list) {
    return list ? list->count : 0;
}

static void execute_commands(flux_canvas *c, const flux_display_list *list) {
    uint32_t entry_top = c->state_top;
    const uint8_t *ptr = list->commands;
    const uint8_t *end = ptr + list->size;

    while (ptr < end && c->pass_error == FLUX_OK) {
        const flux_cmd_header *h = (const flux_cmd_header *)ptr;
        if (h->size == 0 || ptr + h->size > end)
            break;

        switch (h->kind) {
        case FLUX_CMD_CHILD: {
            const flux_cmd_child *cmd = (const void *)ptr;
            flux_canvas_state saved_state = c->states[c->state_top];
            execute_commands(c, cmd->list);
            c->states[c->state_top] = saved_state;
            c->backend->set_scissor(c->backend, c, saved_state.scissor);
            break;
        }
        case FLUX_CMD_DRAW_GEOM: {
            const flux_cmd_draw_geom *cmd = (const flux_cmd_draw_geom *)ptr;
            flux_geometry geom = cmd->geom;
            flux_brush brush = cmd->brush;
            const uint8_t *payload = (const uint8_t *)(cmd + 1);

            flux_path path = {};
            if (geom.kind == FLUX_GEOM_PATH) {
                path.count = path.capacity = cmd->path_seg_count;
                path.segments = (flux_path_segment *)(void *)payload;
                geom.path.path = &path;
            }

            flux_canvas_draw_geometry(c, &geom, &brush);
            break;
        }
        case FLUX_CMD_DRAW_GLYPH_RUN: {
            const flux_cmd_glyph_run *cmd = (const flux_cmd_glyph_run *)ptr;
            flux_glyph_run_desc desc = cmd->desc;
            const uint8_t *payload = (const uint8_t *)(cmd + 1);

            if (cmd->quad_count > 0) {
                desc.quads = (const flux_glyph_quad *)payload;
                payload += (size_t)cmd->quad_count * sizeof(flux_glyph_quad);
            }
            if (cmd->host_cov_bytes > 0) {
                desc.host_coverage = payload;
            }

            flux_canvas_draw_glyph_run(c, &desc);
            break;
        }
        case FLUX_CMD_SAVE:
            flux_canvas_save(c);
            break;
        case FLUX_CMD_RESTORE:
            flux_canvas_restore(c);
            break;
        case FLUX_CMD_CLIP_RECT: {
            const flux_cmd_clip *cmd = (const flux_cmd_clip *)ptr;
            flux_canvas_clip_rect(c, cmd->rect);
            break;
        }
        case FLUX_CMD_SAVE_LAYER: {
            const flux_cmd_save_layer *cmd = (const flux_cmd_save_layer *)ptr;
            flux_canvas_save_layer(c, &cmd->bounds, cmd->opacity);
            break;
        }
        case FLUX_CMD_TRANSLATE: {
            const flux_cmd_vec2 *cmd = (const flux_cmd_vec2 *)ptr;
            flux_canvas_translate(c, cmd->x, cmd->y);
            break;
        }
        case FLUX_CMD_SCALE: {
            const flux_cmd_vec2 *cmd = (const flux_cmd_vec2 *)ptr;
            flux_canvas_scale(c, cmd->x, cmd->y);
            break;
        }
        case FLUX_CMD_ROTATE: {
            const flux_cmd_scalar *cmd = (const flux_cmd_scalar *)ptr;
            flux_canvas_rotate(c, cmd->value);
            break;
        }
        case FLUX_CMD_TRANSFORM: {
            const flux_cmd_transform *cmd = (const flux_cmd_transform *)ptr;
            flux_canvas_transform(c, cmd->matrix);
            break;
        }
        default:
            break;
        }

        ptr += h->size;
    }
    /* Unwind layers even after a terminal draw error so the backend does
     * not retain an unmatched offscreen pass into the next execution. */
    while (c->state_top > entry_top)
        flux_canvas_restore(c);
}

flux_result flux_canvas_submit_display_list(flux_canvas *c, const flux_display_list *list) {
    if (!c || !list)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!c->recording || c->pass_error != FLUX_OK)
        return FLUX_ERROR_INVALID_STATE;
    if (list->size == 0)
        return FLUX_OK;
    for (uint32_t i = 0; i <= c->state_top; i++)
        if (c->states[i].is_layer)
            return FLUX_ERROR_INVALID_STATE;
    if (!canvas_track_display_list(c, list))
        return FLUX_ERROR_OUT_OF_MEMORY;

    /* Lists execute in their own state domain. Target scale and the pass
     * render area are execution inputs; caller transform/clip are not. */
    flux_canvas_state saved[FLUX_CANVAS_MAX_STATES];
    memcpy(saved, c->states, sizeof(saved));
    uint32_t saved_top = c->state_top;
    c->state_top = 0;
    c->states[0] = (flux_canvas_state){
        .transform = flux_mat3x2_scale(c->content_scale, c->content_scale),
        .scissor = c->pass_scissor,
    };
    c->backend->set_scissor(c->backend, c, c->states[0].scissor);
    execute_commands(c, list);
    flux_result result = c->pass_error;
    memcpy(c->states, saved, sizeof(saved));
    c->state_top = saved_top;
    c->backend->set_scissor(c->backend, c, c->states[c->state_top].scissor);
    return result;
}

flux_result flux_encoder_draw_display_list(flux_encoder *enc, const flux_display_list *list) {
    if (!enc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!list) {
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return enc->error;
    }
    if (!encoder_ready(enc))
        return enc->error;
    if (!list->count)
        return FLUX_OK;
    /* Only already-published lists can be referenced, so cycles cannot be
     * constructed. Bound nesting before publication, including child state
     * isolation, to keep execution and destruction stacks bounded. */
    uint32_t depth = enc->depth + list->max_depth;
    if (depth >= FLUX_CANVAS_MAX_STATES || list->tree_depth >= 256u ||
        list->count > enc->command_budget - enc->count) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_RANGE);
        return enc->error;
    }
    flux_cmd_child *cmd = encoder_push(enc, FLUX_CMD_CHILD, sizeof(*cmd));
    if (!cmd)
        return enc->error;
    cmd->list = flux_display_list_retain((flux_display_list *)list);
    enc->count += list->count - 1u;
    if (depth > enc->max_depth)
        enc->max_depth = depth;
    if (list->tree_depth + 1u > enc->tree_depth)
        enc->tree_depth = list->tree_depth + 1u;
    return FLUX_OK;
}
