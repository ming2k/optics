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
    uint32_t image_slot;
    bool has_image_slot;
    uint8_t _pad[3];
    /* Logical resources the caller handed to the recorder, before the
     * immutable snapshots replace them. Serialization IDs key on these. */
    const void *image_key;
    const void *sampler_key;
    /* Trailing flux_path_segment[], addressed relative to this command. */
} flux_cmd_draw_geom;

typedef struct flux_cmd_glyph_run {
    flux_cmd_header header;
    flux_glyph_run_desc desc;
    uint32_t quad_count;
    size_t host_cov_bytes;
    const void *atlas_key;
    const void *sampler_key;
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
    size_t retained_bytes;
    uint32_t count;
    uint32_t max_depth, tree_depth;
    uint64_t input_mask;
};

struct flux_encoder {
    uint8_t *buffer;
    size_t capacity, used, budget;
    size_t retained_bytes, max_retained_bytes;
    flux_image **retained_images;
    uint32_t retained_image_count, retained_image_capacity;
    uint32_t count, depth, max_depth, tree_depth, command_budget;
    uint64_t input_mask;
    bool finished;
    flux_result error;
};

static bool encoder_track_image(flux_encoder *enc, flux_image *image) {
    if (!enc || !image)
        return true;
    for (uint32_t i = 0; i < enc->retained_image_count; ++i)
        if (enc->retained_images[i] == image)
            return true;
    size_t bytes = flux_image_size_bytes(image);
    if (bytes > SIZE_MAX - enc->retained_bytes ||
        (enc->max_retained_bytes && bytes > enc->max_retained_bytes - enc->retained_bytes)) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
        return false;
    }
    if (enc->retained_image_count == enc->retained_image_capacity) {
        uint32_t capacity = enc->retained_image_capacity ? enc->retained_image_capacity * 2u : 16u;
        if (capacity < enc->retained_image_capacity)
            capacity = UINT32_MAX;
        flux_image **images = realloc(enc->retained_images, (size_t)capacity * sizeof(*images));
        if (!images) {
            flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
            return false;
        }
        enc->retained_images = images;
        enc->retained_image_capacity = capacity;
    }
    enc->retained_images[enc->retained_image_count++] = image;
    enc->retained_bytes += bytes;
    return true;
}

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
    if (!enc->buffer) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
        return nullptr;
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
            if (cmd->brush.kind == FLUX_BRUSH_IMAGE_PATTERN && !cmd->has_image_slot) {
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
    enc->max_retained_bytes = desc ? desc->max_retained_bytes : 0;
    *out = enc;
    return FLUX_OK;
}

/* An encoder is a frame-lifetime recorder: it is created, records one
 * frame's commands, and is destroyed before the frame ends. It is the one
 * resource in the stack whose lifetime genuinely coincides with a frame, so it
 * is the FLUX_LIFETIME_FRAME kind that `flux_observe_frame_drain_check`
 * enforces at the frame boundary (ADR-0129 decision 4).
 *
 * Device-free by design: an encoder may exist before any device (CPU canvas,
 * headless recording), so its row lives in a device ledger only when one is
 * reachable. Recording has no device argument, hence the row is declared with
 * a nullptr device and lands in the process ledger, whose drain point is
 * process exit — which would be the wrong drain point for a per-frame object.
 * Instead the encoder takes the device from its own canvas-bound creation path
 * when there is one; the flux_canvas_reserve path below passes it explicitly. */

void flux_encoder_destroy(flux_encoder *enc) {
    if (!enc)
        return;
    commands_release(enc->buffer, enc->used);
    free(enc->retained_images);
    free(enc);
}

static void encoder_draw_geometry_impl(flux_encoder *enc, const flux_geometry *geom,
                                       const flux_brush *brush, bool has_image_slot,
                                       uint32_t image_slot) {
    if (!encoder_ready(enc))
        return;
    flux_brush b = brush ? *brush : flux_brush_solid(0xff000000u);
    if (!geom) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "flux_encoder_draw_geometry: geom is NULL");
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (!canvas_valid_geometry(geom)) {
        switch (geom->kind) {
        case FLUX_GEOM_RECT:
            FLUX_FAILF(
                FLUX_ERROR_INVALID_ARGUMENT,
                "flux_encoder_draw_geometry: non-finite or negative rect [%.2f, %.2f, %.2f, %.2f]",
                (double)geom->rect.rect.x, (double)geom->rect.rect.y, (double)geom->rect.rect.w,
                (double)geom->rect.rect.h);
            break;
        case FLUX_GEOM_RRECT:
            FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                       "flux_encoder_draw_geometry: non-finite/negative rrect [%.2f, %.2f, %.2f, "
                       "%.2f], radius=%.2f",
                       (double)geom->rrect.rect.x, (double)geom->rrect.rect.y,
                       (double)geom->rrect.rect.w, (double)geom->rrect.rect.h,
                       (double)geom->rrect.radius);
            break;
        case FLUX_GEOM_SQUIRCLE:
            FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                       "flux_encoder_draw_geometry: invalid squircle [%.2f, %.2f, %.2f, %.2f], "
                       "r=%.2f, c=%.2f",
                       (double)geom->squircle.rect.x, (double)geom->squircle.rect.y,
                       (double)geom->squircle.rect.w, (double)geom->squircle.rect.h,
                       (double)geom->squircle.radius, (double)geom->squircle.curvature);
            break;
        case FLUX_GEOM_CIRCLE:
            FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                       "flux_encoder_draw_geometry: invalid circle center=[%.2f, %.2f], r=%.2f",
                       (double)geom->circle.cx, (double)geom->circle.cy,
                       (double)geom->circle.radius);
            break;
        case FLUX_GEOM_LINE:
            FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                       "flux_encoder_draw_geometry: non-finite line [%.2f, %.2f] -> [%.2f, %.2f]",
                       (double)geom->line.x0, (double)geom->line.y0, (double)geom->line.x1,
                       (double)geom->line.y1);
            break;
        default:
            FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                       "flux_encoder_draw_geometry: invalid geometry kind %d or stroke width %.2f",
                       (int)geom->kind, (double)geom->stroke.width);
            break;
        }
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (!(has_image_slot ? canvas_valid_brush_slot(&b) : canvas_valid_brush(&b))) {
        FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_encoder_draw_geometry: invalid brush opacity=%.3f (must be in [0, 1]) or "
                   "blend=%d",
                   (double)b.opacity, (int)b.blend);
        flux_encoder_fail(enc, FLUX_ERROR_INVALID_ARGUMENT);
        return;
    }
    if (has_image_slot &&
        (b.kind != FLUX_BRUSH_IMAGE_PATTERN || image_slot >= FLUX_DISPLAY_LIST_MAX_INPUTS)) {
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
    cmd->image_key = b.image.image;
    cmd->sampler_key = b.image.sampler;
    cmd->path_seg_count = count;
    cmd->image_slot = has_image_slot ? image_slot : 0u;
    cmd->has_image_slot = has_image_slot;
    if (has_image_slot) {
        /* The list carries a slot reference, not a resource. The binding is
         * validated and retained at execution. */
        cmd->brush.image.image = nullptr;
        cmd->brush.image.sampler = nullptr;
        enc->input_mask |= UINT64_C(1) << image_slot;
    }
    if (geom->kind == FLUX_GEOM_PATH) {
        cmd->geom.path.path = nullptr;
        if (count)
            memcpy(cmd + 1, geom->path.path->segments, (size_t)count * sizeof(flux_path_segment));
    }
    if (b.kind == FLUX_BRUSH_IMAGE_PATTERN && !has_image_slot) {
        cmd->brush.image.image = nullptr;
        flux_image *snapshot = nullptr;
        flux_result snap_result = flux_image_snapshot(b.image.image, &snapshot);
        if (snap_result != FLUX_OK) {
            flux_encoder_fail(enc, snap_result);
            return;
        }
        cmd->brush.image.image = snapshot;
        (void)flux_sampler_retain(b.image.sampler);
        (void)encoder_track_image(enc, snapshot);
    }
}

void flux_encoder_draw_geometry(flux_encoder *enc, const flux_geometry *geom,
                                const flux_brush *brush) {
    encoder_draw_geometry_impl(enc, geom, brush, false, 0u);
}

void flux_encoder_draw_geometry_slot(flux_encoder *enc, const flux_geometry *geom,
                                     const flux_brush *brush, uint32_t slot) {
    encoder_draw_geometry_impl(enc, geom, brush, true, slot);
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
        if (!canvas_finite_rect((flux_rect){q->sx, q->sy, q->sw, q->sh}) || !q->aw || !q->ah) {
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
    cmd->atlas_key = desc->atlas;
    cmd->sampler_key = desc->sampler;
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
    cmd->desc.atlas = nullptr;
    if (desc->atlas) {
        flux_image *snapshot = nullptr;
        flux_result snap_result = flux_image_snapshot(desc->atlas, &snapshot);
        if (snap_result != FLUX_OK) {
            flux_encoder_fail(enc, snap_result);
            return;
        }
        cmd->desc.atlas = snapshot;
        (void)encoder_track_image(enc, snapshot);
    }
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
    if (!canvas_finite_rect(r)) {
        FLUX_FAILF(
            FLUX_ERROR_INVALID_ARGUMENT,
            "flux_encoder_clip_rect: non-finite or negative clip rect [%.2f, %.2f, %.2f, %.2f]",
            (double)r.x, (double)r.y, (double)r.w, (double)r.h);
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
    if (!isfinite(opacity) || opacity < 0 || opacity > 1 ||
        (bounds && !canvas_finite_rect(*bounds))) {
        FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_encoder_save_layer: invalid opacity=%.3f or bounds", (double)opacity);
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
        FLUX_FAILF(FLUX_ERROR_INVALID_ARGUMENT,
                   "flux_encoder transform (kind=%d): non-finite delta/scale [%.2f, %.2f]",
                   (int)kind, (double)x, (double)y);
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
    list->retained_bytes = enc->retained_bytes;
    list->count = enc->count;
    list->max_depth = enc->max_depth;
    list->tree_depth = enc->tree_depth;
    list->input_mask = enc->input_mask;
    enc->buffer = nullptr;
    enc->used = enc->capacity = 0;
    free(enc->retained_images);
    enc->retained_images = nullptr;
    enc->retained_image_count = enc->retained_image_capacity = 0;
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
uint64_t flux_display_list_input_mask(const flux_display_list *list) {
    return list ? list->input_mask : 0;
}
size_t flux_display_list_retained_bytes(const flux_display_list *list) {
    return list ? list->retained_bytes : 0;
}

typedef struct display_list_bindings {
    const flux_display_list_binding *items;
    uint32_t count;
} display_list_bindings;

static const flux_display_list_binding *find_binding(const display_list_bindings *bindings,
                                                     uint32_t slot) {
    if (!bindings)
        return nullptr;
    for (uint32_t i = 0; i < bindings->count; ++i)
        if (bindings->items[i].slot == slot)
            return &bindings->items[i];
    return nullptr;
}

static void execute_commands(flux_canvas *c, const flux_display_list *list,
                             const display_list_bindings *bindings) {
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
            execute_commands(c, cmd->list, bindings);
            c->states[c->state_top] = saved_state;
            c->backend->set_scissor(c->backend, c, saved_state.scissor);
            break;
        }
        case FLUX_CMD_DRAW_GEOM: {
            const flux_cmd_draw_geom *cmd = (const flux_cmd_draw_geom *)ptr;
            flux_geometry geom = cmd->geom;
            flux_brush brush = cmd->brush;
            const uint8_t *payload = (const uint8_t *)(cmd + 1);

            if (cmd->has_image_slot) {
                const flux_display_list_binding *binding = find_binding(bindings, cmd->image_slot);
                if (!binding || !binding->image) {
                    c->pass_error = FLUX_ERROR_INVALID_ARGUMENT;
                    break;
                }
                brush.image.image = binding->image;
                brush.image.sampler = binding->sampler;
            }

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
    if (list->input_mask)
        return FLUX_ERROR_INVALID_STATE;
    return flux_canvas_submit_display_list_bound(c, list, nullptr, 0);
}

flux_result flux_canvas_submit_display_list_bound(flux_canvas *c, const flux_display_list *list,
                                                  const flux_display_list_binding *bindings,
                                                  uint32_t binding_count) {
    if (!c || !list || (binding_count && !bindings))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!c->recording || c->pass_error != FLUX_OK)
        return FLUX_ERROR_INVALID_STATE;

    uint64_t seen = 0;
    for (uint32_t i = 0; i < binding_count; ++i) {
        const flux_display_list_binding *binding = &bindings[i];
        if (binding->slot >= FLUX_DISPLAY_LIST_MAX_INPUTS || !binding->image)
            return FLUX_ERROR_INVALID_ARGUMENT;
        uint64_t bit = UINT64_C(1) << binding->slot;
        if ((seen & bit) || !(list->input_mask & bit))
            return FLUX_ERROR_INVALID_ARGUMENT;
        seen |= bit;
    }
    if (seen != list->input_mask)
        return FLUX_ERROR_INVALID_STATE;

    if (list->size == 0)
        return FLUX_OK;
    for (uint32_t i = 0; i <= c->state_top; i++)
        if (c->states[i].is_layer)
            return FLUX_ERROR_INVALID_STATE;
    if (!canvas_track_display_list(c, list))
        return FLUX_ERROR_OUT_OF_MEMORY;

    display_list_bindings bound = {
        .items = bindings,
        .count = binding_count,
    };

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
    execute_commands(c, list, &bound);
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
    if (list->retained_bytes > SIZE_MAX - enc->retained_bytes ||
        (enc->max_retained_bytes &&
         list->retained_bytes > enc->max_retained_bytes - enc->retained_bytes)) {
        flux_encoder_fail(enc, FLUX_ERROR_OUT_OF_MEMORY);
        return enc->error;
    }
    cmd->list = flux_display_list_retain((flux_display_list *)list);
    enc->retained_bytes += list->retained_bytes;
    enc->input_mask |= list->input_mask;
    enc->count += list->count - 1u;
    if (depth > enc->max_depth)
        enc->max_depth = depth;
    if (list->tree_depth + 1u > enc->tree_depth)
        enc->tree_depth = list->tree_depth + 1u;
    return FLUX_OK;
}

/* ================================================================== */
/*  DisplayList serialization (ADR-0157)                              */
/*                                                                    */
/*  Canonical little-endian wire format; never native struct bytes.   */
/*  Header: magic u32, version u16, flags u16, resources u32,          */
/*  command_bytes u32, total_size u32, reserved u32.                  */
/*  Resource entry: kind u8, pad u8[3], id u32.                       */
/*  Command: op u8, flags u8, reserved u16, payload_len u32, payload, */
/*  zero padding to an 8-byte boundary.                               */
/* ================================================================== */

#define DL_MAGIC 0x4C445846u /* "FLXD" */
#define DL_VERSION 1u
#define DL_HEADER_BYTES 24u
#define DL_RESOURCE_BYTES 8u

typedef struct dl_bytes {
    uint8_t *data;
    size_t len, cap;
} dl_bytes;

typedef struct dl_serialized_resource {
    flux_display_list_resource_kind kind;
    const void *ptr;
    uint32_t id;
} dl_serialized_resource;

typedef struct dl_serializer {
    dl_bytes commands;
    dl_serialized_resource *resources;
    uint32_t resource_count, resource_capacity;
    const flux_display_list_serialize_opts *opts;
    flux_result error;
    uint32_t depth;
} dl_serializer;

static bool dl_bytes_reserve(dl_bytes *b, size_t extra) {
    if (extra > SIZE_MAX - b->len)
        return false;
    size_t need = b->len + extra;
    if (need <= b->cap)
        return true;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2)
            return false;
        cap *= 2;
    }
    uint8_t *p = realloc(b->data, cap);
    if (!p)
        return false;
    b->data = p;
    b->cap = cap;
    return true;
}

static bool dl_put(dl_bytes *b, const void *src, size_t n) {
    if (!dl_bytes_reserve(b, n))
        return false;
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return true;
}
static bool dl_u8(dl_bytes *b, uint8_t v) {
    return dl_put(b, &v, 1);
}
static bool dl_u16(dl_bytes *b, uint16_t v) {
    return dl_put(b, &v, 2);
}
static bool dl_u32(dl_bytes *b, uint32_t v) {
    return dl_put(b, &v, 4);
}
static bool dl_f32(dl_bytes *b, float v) {
    return dl_put(b, &v, 4);
}
static bool dl_pad8(dl_bytes *b) {
    while (b->len & 7u)
        if (!dl_u8(b, 0))
            return false;
    return true;
}

static bool dl_begin_command(dl_serializer *s, uint8_t op, uint8_t flags, size_t *start) {
    *start = s->commands.len;
    if (!dl_u8(&s->commands, op) || !dl_u8(&s->commands, flags) || !dl_u16(&s->commands, 0) ||
        !dl_u32(&s->commands, 0))
        return false;
    return true;
}
static bool dl_end_command(dl_serializer *s, size_t start) {
    size_t payload = s->commands.len - start - 8u;
    if (payload > UINT32_MAX)
        return false;
    uint32_t n = (uint32_t)payload;
    memcpy(s->commands.data + start + 4, &n, sizeof n);
    return dl_pad8(&s->commands);
}

static uint32_t dl_resource_id(dl_serializer *s, flux_display_list_resource_kind kind,
                               const void *ptr) {
    if (!ptr)
        return 0;
    for (uint32_t i = 0; i < s->resource_count; ++i)
        if (s->resources[i].kind == kind && s->resources[i].ptr == ptr)
            return s->resources[i].id;
    if (!s->opts || !s->opts->resource_id) {
        s->error = FLUX_ERROR_INVALID_ARGUMENT;
        return 0;
    }
    uint32_t id = s->opts->resource_id(s->opts->user, kind, ptr);
    if (id == 0) {
        s->error = FLUX_ERROR_INVALID_ARGUMENT;
        return 0;
    }
    for (uint32_t i = 0; i < s->resource_count; ++i)
        if (s->resources[i].id == id) {
            s->error = FLUX_ERROR_INVALID_ARGUMENT;
            return 0;
        }
    if (s->resource_count == s->resource_capacity) {
        uint32_t cap = s->resource_capacity ? s->resource_capacity * 2u : 16u;
        dl_serialized_resource *r = realloc(s->resources, (size_t)cap * sizeof(*r));
        if (!r) {
            s->error = FLUX_ERROR_OUT_OF_MEMORY;
            return 0;
        }
        s->resources = r;
        s->resource_capacity = cap;
    }
    s->resources[s->resource_count++] = (dl_serialized_resource){kind, ptr, id};
    return id;
}

static bool dl_write_brush(dl_serializer *s, const flux_brush *brush, bool has_slot, uint32_t slot,
                           const void *image_key, const void *sampler_key) {
    dl_bytes *b = &s->commands;
    if (!dl_u8(b, (uint8_t)brush->kind) || !dl_u8(b, (uint8_t)brush->blend) ||
        !dl_u8(b, has_slot) || !dl_u8(b, 0) || !dl_f32(b, brush->opacity))
        return false;
    if (brush->kind == FLUX_BRUSH_SOLID)
        return dl_u32(b, brush->solid.color);
    if (brush->kind == FLUX_BRUSH_IMAGE_PATTERN) {
        if (has_slot && !dl_u32(b, slot))
            return false;
        uint32_t image_id =
            has_slot ? 0 : dl_resource_id(s, FLUX_DISPLAY_LIST_RESOURCE_IMAGE, image_key);
        uint32_t sampler_id =
            has_slot ? 0 : dl_resource_id(s, FLUX_DISPLAY_LIST_RESOURCE_SAMPLER, sampler_key);
        if (s->error != FLUX_OK)
            return false;
        if (!dl_u32(b, image_id) || !dl_u32(b, sampler_id) || !dl_f32(b, brush->image.src_rect.x) ||
            !dl_f32(b, brush->image.src_rect.y) || !dl_f32(b, brush->image.src_rect.w) ||
            !dl_f32(b, brush->image.src_rect.h) || !dl_u8(b, brush->image.opaque_only) ||
            !dl_u8(b, 0) || !dl_u8(b, 0) || !dl_u8(b, 0) || !dl_u32(b, brush->image.tint) ||
            !dl_f32(b, brush->image.clip_rect.x) || !dl_f32(b, brush->image.clip_rect.y) ||
            !dl_f32(b, brush->image.clip_rect.w) || !dl_f32(b, brush->image.clip_rect.h) ||
            !dl_f32(b, brush->image.clip_radius))
            return false;
        return true;
    }
    const flux_brush_gradient_data *g = &brush->gradient;
    if (!dl_u32(b, g->stops.count))
        return false;
    for (uint32_t i = 0; i < g->stops.count; ++i)
        if (!dl_f32(b, g->stops.stops[i].t) || !dl_u32(b, g->stops.stops[i].color))
            return false;
    return dl_f32(b, g->start.x) && dl_f32(b, g->start.y) && dl_f32(b, g->end.x) &&
           dl_f32(b, g->end.y) && dl_f32(b, g->radius);
}

static bool dl_write_geometry(dl_serializer *s, const flux_cmd_draw_geom *cmd) {
    dl_bytes *b = &s->commands;
    const flux_geometry *g = &cmd->geom;
    if (!dl_u8(b, (uint8_t)g->kind) || !dl_u8(b, 0) || !dl_u8(b, 0) || !dl_u8(b, 0) ||
        !dl_f32(b, g->stroke.width) || !dl_f32(b, g->stroke.miter_limit) ||
        !dl_u8(b, (uint8_t)g->stroke.cap) || !dl_u8(b, (uint8_t)g->stroke.join) || !dl_u16(b, 0))
        return false;
    switch (g->kind) {
    case FLUX_GEOM_RECT:
        if (!dl_f32(b, g->rect.rect.x) || !dl_f32(b, g->rect.rect.y) ||
            !dl_f32(b, g->rect.rect.w) || !dl_f32(b, g->rect.rect.h))
            return false;
        break;
    case FLUX_GEOM_RRECT:
        if (!dl_f32(b, g->rrect.rect.x) || !dl_f32(b, g->rrect.rect.y) ||
            !dl_f32(b, g->rrect.rect.w) || !dl_f32(b, g->rrect.rect.h) ||
            !dl_f32(b, g->rrect.radius))
            return false;
        break;
    case FLUX_GEOM_SQUIRCLE:
        if (!dl_f32(b, g->squircle.rect.x) || !dl_f32(b, g->squircle.rect.y) ||
            !dl_f32(b, g->squircle.rect.w) || !dl_f32(b, g->squircle.rect.h) ||
            !dl_f32(b, g->squircle.radius) || !dl_f32(b, g->squircle.curvature))
            return false;
        break;
    case FLUX_GEOM_CIRCLE:
        if (!dl_f32(b, g->circle.cx) || !dl_f32(b, g->circle.cy) || !dl_f32(b, g->circle.radius))
            return false;
        break;
    case FLUX_GEOM_LINE:
        if (!dl_f32(b, g->line.x0) || !dl_f32(b, g->line.y0) || !dl_f32(b, g->line.x1) ||
            !dl_f32(b, g->line.y1))
            return false;
        break;
    case FLUX_GEOM_PATH: {
        uint32_t count = cmd->path_seg_count;
        if (!dl_u32(b, count) || !dl_u8(b, (uint8_t)g->path.fill_rule) || !dl_u8(b, 0) ||
            !dl_u8(b, 0) || !dl_u8(b, 0))
            return false;
        const flux_path_segment *segs = (const flux_path_segment *)(cmd + 1);
        for (uint32_t i = 0; i < count; ++i) {
            if (!dl_u32(b, segs[i].op))
                return false;
            for (uint32_t k = 0; k < 6; ++k)
                if (!dl_f32(b, segs[i].pts[k]))
                    return false;
        }
        break;
    }
    default:
        return false;
    }
    return dl_write_brush(s, &cmd->brush, cmd->has_image_slot, cmd->image_slot, cmd->image_key,
                          cmd->sampler_key);
}

static bool dl_write_commands(dl_serializer *s, const flux_display_list *list) {
    if (!list || s->depth++ > 256u) {
        s->error = FLUX_ERROR_OUT_OF_RANGE;
        return false;
    }
    const uint8_t *ptr = list->commands;
    const uint8_t *end = ptr + list->size;
    while (ptr < end) {
        const flux_cmd_header *h = (const void *)ptr;
        if (h->size == 0 || ptr + h->size > end)
            return false;
        size_t start;
        switch (h->kind) {
        case FLUX_CMD_CHILD: {
            const flux_cmd_child *c = (const void *)ptr;
            if (!dl_begin_command(s, FLUX_CMD_SAVE, 0, &start) || !dl_end_command(s, start) ||
                !dl_write_commands(s, c->list))
                return false;
            if (!dl_begin_command(s, FLUX_CMD_RESTORE, 0, &start) || !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_DRAW_GEOM: {
            const flux_cmd_draw_geom *c = (const void *)ptr;
            if (!dl_begin_command(s, FLUX_CMD_DRAW_GEOM, 0, &start) || !dl_write_geometry(s, c) ||
                !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_DRAW_GLYPH_RUN: {
            const flux_cmd_glyph_run *c = (const void *)ptr;
            if (!dl_begin_command(s, FLUX_CMD_DRAW_GLYPH_RUN, 0, &start))
                return false;
            dl_bytes *b = &s->commands;
            uint32_t atlas_id = dl_resource_id(s, FLUX_DISPLAY_LIST_RESOURCE_IMAGE, c->atlas_key);
            uint32_t sampler_id =
                dl_resource_id(s, FLUX_DISPLAY_LIST_RESOURCE_SAMPLER, c->sampler_key);
            if (s->error != FLUX_OK || !dl_u32(b, atlas_id) || !dl_u32(b, sampler_id) ||
                !dl_u32(b, c->quad_count) || !dl_u32(b, (uint32_t)c->host_cov_bytes) ||
                !dl_u32(b, c->desc.host_atlas_w) || !dl_u32(b, c->desc.host_atlas_h))
                return false;
            const uint8_t *payload = (const uint8_t *)(c + 1);
            for (uint32_t i = 0; i < c->quad_count; ++i) {
                const flux_glyph_quad *q = (const flux_glyph_quad *)payload + i;
                if (!dl_f32(b, q->sx) || !dl_f32(b, q->sy) || !dl_f32(b, q->sw) ||
                    !dl_f32(b, q->sh) || !dl_u16(b, q->ax) || !dl_u16(b, q->ay) ||
                    !dl_u16(b, q->aw) || !dl_u16(b, q->ah) || !dl_u32(b, q->color))
                    return false;
            }
            if (c->host_cov_bytes &&
                !dl_put(b, payload + (size_t)c->quad_count * sizeof(flux_glyph_quad),
                        (size_t)c->host_cov_bytes))
                return false;
            if (!dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_SAVE:
        case FLUX_CMD_RESTORE:
            if (!dl_begin_command(s, h->kind, 0, &start) || !dl_end_command(s, start))
                return false;
            break;
        case FLUX_CMD_CLIP_RECT: {
            const flux_cmd_clip *c = (const void *)ptr;
            if (!dl_begin_command(s, h->kind, 0, &start) || !dl_f32(&s->commands, c->rect.x) ||
                !dl_f32(&s->commands, c->rect.y) || !dl_f32(&s->commands, c->rect.w) ||
                !dl_f32(&s->commands, c->rect.h) || !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_SAVE_LAYER: {
            const flux_cmd_save_layer *c = (const void *)ptr;
            if (!dl_begin_command(s, h->kind, 0, &start) || !dl_f32(&s->commands, c->bounds.x) ||
                !dl_f32(&s->commands, c->bounds.y) || !dl_f32(&s->commands, c->bounds.w) ||
                !dl_f32(&s->commands, c->bounds.h) || !dl_f32(&s->commands, c->opacity) ||
                !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_TRANSLATE:
        case FLUX_CMD_SCALE: {
            const flux_cmd_vec2 *c = (const void *)ptr;
            if (!dl_begin_command(s, h->kind, 0, &start) || !dl_f32(&s->commands, c->x) ||
                !dl_f32(&s->commands, c->y) || !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_ROTATE: {
            const flux_cmd_scalar *c = (const void *)ptr;
            if (!dl_begin_command(s, h->kind, 0, &start) || !dl_f32(&s->commands, c->value) ||
                !dl_end_command(s, start))
                return false;
            break;
        }
        case FLUX_CMD_TRANSFORM: {
            const flux_cmd_transform *c = (const void *)ptr;
            if (!dl_begin_command(s, h->kind, 0, &start))
                return false;
            for (int i = 0; i < 6; ++i)
                if (!dl_f32(&s->commands, c->matrix.m[i]))
                    return false;
            if (!dl_end_command(s, start))
                return false;
            break;
        }
        default:
            return false;
        }
        ptr += h->size;
    }
    s->depth--;
    return true;
}

flux_result flux_display_list_serialize(const flux_display_list *list,
                                        const flux_display_list_serialize_opts *opts,
                                        uint8_t **out_data, size_t *out_size) {
    if (!out_data || !out_size)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_data = nullptr;
    *out_size = 0;
    if (!list)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (opts && opts->type != FLUX_TYPE_DISPLAY_LIST_SERIALIZE_OPTS)
        return FLUX_ERROR_INVALID_ARGUMENT;
    dl_serializer s = {.opts = opts};
    if (!dl_write_commands(&s, list)) {
        free(s.commands.data);
        free(s.resources);
        return s.error != FLUX_OK ? s.error : FLUX_ERROR_INVALID_ARGUMENT;
    }
    size_t total = DL_HEADER_BYTES + (size_t)s.resource_count * DL_RESOURCE_BYTES + s.commands.len;
    if (total > UINT32_MAX) {
        free(s.commands.data);
        free(s.resources);
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    uint8_t *data = malloc(total);
    if (!data) {
        free(s.commands.data);
        free(s.resources);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    size_t off = 0;
    uint32_t magic = DL_MAGIC;
    uint16_t version = DL_VERSION;
    uint16_t flags = 0;
    uint32_t resource_count = s.resource_count;
    uint32_t command_bytes = (uint32_t)s.commands.len;
    uint32_t total_size = (uint32_t)total;
    uint32_t reserved = 0;
#define DL_PUT_FIELD(value)                                                                        \
    do {                                                                                           \
        memcpy(data + off, &(value), sizeof(value));                                               \
        off += sizeof(value);                                                                      \
    } while (0)
    DL_PUT_FIELD(magic);
    DL_PUT_FIELD(version);
    DL_PUT_FIELD(flags);
    DL_PUT_FIELD(resource_count);
    DL_PUT_FIELD(command_bytes);
    DL_PUT_FIELD(total_size);
    DL_PUT_FIELD(reserved);
    for (uint32_t i = 0; i < s.resource_count; ++i) {
        uint8_t kind = (uint8_t)s.resources[i].kind;
        uint8_t zero[3] = {0, 0, 0};
        uint32_t id = s.resources[i].id;
        DL_PUT_FIELD(kind);
        DL_PUT_FIELD(zero);
        DL_PUT_FIELD(id);
    }
#undef DL_PUT_FIELD
    memcpy(data + off, s.commands.data, s.commands.len);
    free(s.commands.data);
    free(s.resources);
    *out_data = data;
    *out_size = total;
    return FLUX_OK;
}

void flux_display_list_serialized_free(uint8_t *data) {
    free(data);
}

typedef struct dl_reader {
    const uint8_t *data;
    size_t size, off;
    bool bad;
} dl_reader;

static uint32_t dl_read_u32(dl_reader *r) {
    if (r->bad || r->off + 4 > r->size) {
        r->bad = true;
        return 0;
    }
    uint32_t v;
    memcpy(&v, r->data + r->off, 4);
    r->off += 4;
    return v;
}
static uint8_t dl_read_u8(dl_reader *r) {
    if (r->bad || r->off + 1 > r->size) {
        r->bad = true;
        return 0;
    }
    return r->data[r->off++];
}
static uint16_t dl_read_u16(dl_reader *r) {
    if (r->bad || r->off + 2 > r->size) {
        r->bad = true;
        return 0;
    }
    uint16_t v;
    memcpy(&v, r->data + r->off, 2);
    r->off += 2;
    return v;
}
static float dl_read_f32(dl_reader *r) {
    uint32_t v = dl_read_u32(r);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static flux_rect dl_read_rect(dl_reader *r) {
    flux_rect rect;
    rect.x = dl_read_f32(r);
    rect.y = dl_read_f32(r);
    rect.w = dl_read_f32(r);
    rect.h = dl_read_f32(r);
    return rect;
}
static void dl_read_skip(dl_reader *r, size_t n) {
    if (r->bad || n > r->size - r->off)
        r->bad = true;
    else
        r->off += n;
}

typedef struct dl_resolved {
    flux_display_list_resource_kind kind;
    uint32_t id;
    const void *ptr;
} dl_resolved;

static const void *dl_find_resource(const dl_resolved *res, uint32_t count, uint32_t id,
                                    flux_display_list_resource_kind kind) {
    if (id == 0)
        return nullptr;
    for (uint32_t i = 0; i < count; ++i)
        if (res[i].id == id && res[i].kind == kind)
            return res[i].ptr;
    return nullptr;
}

static flux_result dl_read_brush(dl_reader *r, flux_brush *b, const dl_resolved *resources,
                                 uint32_t resource_count, bool *has_slot, uint32_t *slot) {
    memset(b, 0, sizeof(*b));
    b->kind = (flux_brush_kind)dl_read_u8(r);
    b->blend = (flux_blend_mode)dl_read_u8(r);
    *has_slot = dl_read_u8(r) != 0;
    dl_read_u8(r);
    b->opacity = dl_read_f32(r);
    if (r->bad)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (b->kind == FLUX_BRUSH_SOLID) {
        b->solid.color = dl_read_u32(r);
        return r->bad ? FLUX_ERROR_INVALID_ARGUMENT : FLUX_OK;
    }
    if (b->kind == FLUX_BRUSH_IMAGE_PATTERN) {
        if (*has_slot)
            *slot = dl_read_u32(r);
        uint32_t image_id = dl_read_u32(r);
        uint32_t sampler_id = dl_read_u32(r);
        b->image.image = (flux_image *)dl_find_resource(resources, resource_count, image_id,
                                                        FLUX_DISPLAY_LIST_RESOURCE_IMAGE);
        b->image.sampler = (flux_sampler *)dl_find_resource(resources, resource_count, sampler_id,
                                                            FLUX_DISPLAY_LIST_RESOURCE_SAMPLER);
        if ((image_id && !b->image.image) || (sampler_id && !b->image.sampler))
            return FLUX_ERROR_INVALID_ARGUMENT;
        b->image.src_rect.x = dl_read_f32(r);
        b->image.src_rect.y = dl_read_f32(r);
        b->image.src_rect.w = dl_read_f32(r);
        b->image.src_rect.h = dl_read_f32(r);
        b->image.opaque_only = dl_read_u8(r) != 0;
        dl_read_skip(r, 3);
        b->image.tint = dl_read_u32(r);
        b->image.clip_rect.x = dl_read_f32(r);
        b->image.clip_rect.y = dl_read_f32(r);
        b->image.clip_rect.w = dl_read_f32(r);
        b->image.clip_rect.h = dl_read_f32(r);
        b->image.clip_radius = dl_read_f32(r);
        return r->bad ? FLUX_ERROR_INVALID_ARGUMENT : FLUX_OK;
    }
    if (b->kind != FLUX_BRUSH_LINEAR_GRADIENT && b->kind != FLUX_BRUSH_RADIAL_GRADIENT)
        return FLUX_ERROR_INVALID_ARGUMENT;
    uint32_t count = dl_read_u32(r);
    if (count == 0 || count > FLUX_GRADIENT_MAX_STOPS)
        return FLUX_ERROR_INVALID_ARGUMENT;
    b->gradient.stops.count = count;
    for (uint32_t i = 0; i < count; ++i) {
        b->gradient.stops.stops[i].t = dl_read_f32(r);
        b->gradient.stops.stops[i].color = dl_read_u32(r);
    }
    b->gradient.start.x = dl_read_f32(r);
    b->gradient.start.y = dl_read_f32(r);
    b->gradient.end.x = dl_read_f32(r);
    b->gradient.end.y = dl_read_f32(r);
    b->gradient.radius = dl_read_f32(r);
    return r->bad ? FLUX_ERROR_INVALID_ARGUMENT : FLUX_OK;
}

flux_result flux_display_list_deserialize(const uint8_t *data, size_t size,
                                          const flux_display_list_deserialize_opts *opts,
                                          flux_display_list **out_list) {
    if (!out_list)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_list = nullptr;
    if (!data || size < DL_HEADER_BYTES ||
        (opts && opts->type != FLUX_TYPE_DISPLAY_LIST_DESERIALIZE_OPTS))
        return FLUX_ERROR_INVALID_ARGUMENT;
    dl_reader r = {.data = data, .size = size};
    if (dl_read_u32(&r) != DL_MAGIC || dl_read_u16(&r) != DL_VERSION)
        return FLUX_ERROR_INVALID_ARGUMENT;
    uint16_t flags = dl_read_u16(&r);
    uint32_t resource_count = dl_read_u32(&r);
    uint32_t command_bytes = dl_read_u32(&r);
    uint32_t total_size = dl_read_u32(&r);
    uint32_t reserved = dl_read_u32(&r);
    if (r.bad || flags != 0 || reserved != 0 || total_size != size ||
        resource_count > (UINT32_MAX / DL_RESOURCE_BYTES) ||
        DL_HEADER_BYTES + (size_t)resource_count * DL_RESOURCE_BYTES + command_bytes != size)
        return FLUX_ERROR_INVALID_ARGUMENT;
    dl_resolved *resources = resource_count ? calloc(resource_count, sizeof(*resources)) : nullptr;
    if (resource_count && !resources)
        return FLUX_ERROR_OUT_OF_MEMORY;
    flux_result result = FLUX_OK;
    for (uint32_t i = 0; i < resource_count; ++i) {
        flux_display_list_resource_kind kind = (flux_display_list_resource_kind)dl_read_u8(&r);
        uint8_t pad0 = dl_read_u8(&r);
        uint8_t pad1 = dl_read_u8(&r);
        uint8_t pad2 = dl_read_u8(&r);
        uint32_t id = dl_read_u32(&r);
        if (r.bad || pad0 || pad1 || pad2 || id == 0 ||
            (kind != FLUX_DISPLAY_LIST_RESOURCE_IMAGE &&
             kind != FLUX_DISPLAY_LIST_RESOURCE_SAMPLER) ||
            !opts || !opts->resolve) {
            result = FLUX_ERROR_INVALID_ARGUMENT;
            goto out;
        }
        for (uint32_t k = 0; k < i; ++k)
            if (resources[k].id == id) {
                result = FLUX_ERROR_INVALID_ARGUMENT;
                goto out;
            }
        const void *ptr = opts->resolve(opts->user, kind, id);
        if (!ptr) {
            result = FLUX_ERROR_INVALID_ARGUMENT;
            goto out;
        }
        resources[i] = (dl_resolved){kind, id, ptr};
    }
    flux_encoder *enc = nullptr;
    result = flux_encoder_create(nullptr, &enc);
    if (result != FLUX_OK)
        goto out;
    while (r.off < size && result == FLUX_OK) {
        uint8_t op = dl_read_u8(&r);
        uint8_t cmd_flags = dl_read_u8(&r);
        uint16_t cmd_reserved = dl_read_u16(&r);
        uint32_t payload = dl_read_u32(&r);
        if (cmd_flags != 0 || cmd_reserved != 0) {
            result = FLUX_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (r.bad || payload > size - r.off) {
            result = FLUX_ERROR_INVALID_ARGUMENT;
            break;
        }
        size_t next = r.off + payload;
        switch (op) {
        case FLUX_CMD_SAVE:
            flux_encoder_save(enc);
            break;
        case FLUX_CMD_RESTORE:
            flux_encoder_restore(enc);
            break;
        case FLUX_CMD_CLIP_RECT: {
            flux_rect rect = dl_read_rect(&r);
            flux_encoder_clip_rect(enc, rect);
            break;
        }
        case FLUX_CMD_SAVE_LAYER: {
            flux_rect b = dl_read_rect(&r);
            float opacity = dl_read_f32(&r);
            flux_encoder_save_layer(enc, &b, opacity);
            break;
        }
        case FLUX_CMD_TRANSLATE: {
            float x = dl_read_f32(&r);
            float y = dl_read_f32(&r);
            flux_encoder_translate(enc, x, y);
            break;
        }
        case FLUX_CMD_SCALE: {
            float x = dl_read_f32(&r);
            float y = dl_read_f32(&r);
            flux_encoder_scale(enc, x, y);
            break;
        }
        case FLUX_CMD_ROTATE:
            flux_encoder_rotate(enc, dl_read_f32(&r));
            break;
        case FLUX_CMD_TRANSFORM: {
            flux_mat3x2 m;
            for (int i = 0; i < 6; ++i)
                m.m[i] = dl_read_f32(&r);
            flux_encoder_transform(enc, m);
            break;
        }
        case FLUX_CMD_DRAW_GEOM: {
            flux_geometry g = {0};
            g.kind = (flux_geom_kind)dl_read_u8(&r);
            dl_read_skip(&r, 3);
            g.stroke.width = dl_read_f32(&r);
            g.stroke.miter_limit = dl_read_f32(&r);
            g.stroke.cap = (flux_line_cap)dl_read_u8(&r);
            g.stroke.join = (flux_line_join)dl_read_u8(&r);
            dl_read_u16(&r);
            switch (g.kind) {
            case FLUX_GEOM_RECT:
                g.rect.rect =
                    (flux_rect){dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r)};
                break;
            case FLUX_GEOM_RRECT:
                g.rrect.rect =
                    (flux_rect){dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r)};
                g.rrect.radius = dl_read_f32(&r);
                break;
            case FLUX_GEOM_SQUIRCLE:
                g.squircle.rect =
                    (flux_rect){dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r), dl_read_f32(&r)};
                g.squircle.radius = dl_read_f32(&r);
                g.squircle.curvature = dl_read_f32(&r);
                break;
            case FLUX_GEOM_CIRCLE:
                g.circle.cx = dl_read_f32(&r);
                g.circle.cy = dl_read_f32(&r);
                g.circle.radius = dl_read_f32(&r);
                break;
            case FLUX_GEOM_LINE:
                g.line.x0 = dl_read_f32(&r);
                g.line.y0 = dl_read_f32(&r);
                g.line.x1 = dl_read_f32(&r);
                g.line.y1 = dl_read_f32(&r);
                break;
            case FLUX_GEOM_PATH: {
                uint32_t count = dl_read_u32(&r);
                g.path.fill_rule = (flux_fill_rule)dl_read_u8(&r);
                dl_read_skip(&r, 3);
                if (count > (payload / (4u + 24u))) {
                    result = FLUX_ERROR_INVALID_ARGUMENT;
                    break;
                }
                flux_path_segment *segs = count ? malloc((size_t)count * sizeof(*segs)) : nullptr;
                if (count && !segs) {
                    result = FLUX_ERROR_OUT_OF_MEMORY;
                    break;
                }
                for (uint32_t i = 0; i < count; ++i) {
                    segs[i].op = dl_read_u32(&r);
                    for (uint32_t k = 0; k < 6; ++k)
                        segs[i].pts[k] = dl_read_f32(&r);
                }
                if (!r.bad) {
                    flux_path path = {.segments = segs, .capacity = count, .count = count};
                    g.path.path = &path;
                    flux_brush b;
                    bool has_slot = false;
                    uint32_t slot = 0;
                    result = dl_read_brush(&r, &b, resources, resource_count, &has_slot, &slot);
                    if (result == FLUX_OK) {
                        if (has_slot)
                            flux_encoder_draw_geometry_slot(enc, &g, &b, slot);
                        else
                            flux_encoder_draw_geometry(enc, &g, &b);
                        result = flux_encoder_status(enc);
                    }
                } else {
                    result = FLUX_ERROR_INVALID_ARGUMENT;
                }
                free(segs);
                break;
            }
            default:
                result = FLUX_ERROR_INVALID_ARGUMENT;
                break;
            }
            if (g.kind != FLUX_GEOM_PATH && result == FLUX_OK) {
                flux_brush b;
                bool has_slot = false;
                uint32_t slot = 0;
                result = dl_read_brush(&r, &b, resources, resource_count, &has_slot, &slot);
                if (result == FLUX_OK) {
                    if (has_slot)
                        flux_encoder_draw_geometry_slot(enc, &g, &b, slot);
                    else
                        flux_encoder_draw_geometry(enc, &g, &b);
                    result = flux_encoder_status(enc);
                }
            }
            break;
        }
        case FLUX_CMD_DRAW_GLYPH_RUN: {
            uint32_t atlas_id = dl_read_u32(&r);
            uint32_t sampler_id = dl_read_u32(&r);
            uint32_t quads_count = dl_read_u32(&r);
            uint32_t cov_bytes = dl_read_u32(&r);
            uint32_t host_w = dl_read_u32(&r);
            uint32_t host_h = dl_read_u32(&r);
            flux_image *atlas = (flux_image *)dl_find_resource(resources, resource_count, atlas_id,
                                                               FLUX_DISPLAY_LIST_RESOURCE_IMAGE);
            flux_sampler *sampler = (flux_sampler *)dl_find_resource(
                resources, resource_count, sampler_id, FLUX_DISPLAY_LIST_RESOURCE_SAMPLER);
            if (r.bad || (atlas_id && !atlas) || (sampler_id && !sampler) ||
                quads_count > payload / 28u || cov_bytes > payload ||
                (uint64_t)quads_count * 28u + cov_bytes > payload) {
                result = FLUX_ERROR_INVALID_ARGUMENT;
                break;
            }
            flux_glyph_quad *quads =
                quads_count ? malloc((size_t)quads_count * sizeof(*quads)) : nullptr;
            uint8_t *cov = cov_bytes ? malloc(cov_bytes) : nullptr;
            if ((quads_count && !quads) || (cov_bytes && !cov)) {
                free(quads);
                free(cov);
                result = FLUX_ERROR_OUT_OF_MEMORY;
                break;
            }
            for (uint32_t i = 0; i < quads_count; ++i) {
                quads[i].sx = dl_read_f32(&r);
                quads[i].sy = dl_read_f32(&r);
                quads[i].sw = dl_read_f32(&r);
                quads[i].sh = dl_read_f32(&r);
                quads[i].ax = dl_read_u16(&r);
                quads[i].ay = dl_read_u16(&r);
                quads[i].aw = dl_read_u16(&r);
                quads[i].ah = dl_read_u16(&r);
                quads[i].color = dl_read_u32(&r);
            }
            if (cov_bytes && !r.bad) {
                memcpy(cov, data + r.off, cov_bytes);
                r.off += cov_bytes;
            }
            if (!r.bad) {
                flux_glyph_run_desc desc = FLUX_GLYPH_RUN_DESC_INIT;
                desc.atlas = atlas;
                desc.sampler = sampler;
                desc.quads = quads;
                desc.quad_count = quads_count;
                desc.host_coverage = cov;
                desc.host_atlas_w = host_w;
                desc.host_atlas_h = host_h;
                flux_encoder_draw_glyph_run(enc, &desc);
                result = flux_encoder_status(enc);
            } else {
                result = FLUX_ERROR_INVALID_ARGUMENT;
            }
            free(quads);
            free(cov);
            break;
        }
        default:
            result = FLUX_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (result != FLUX_OK)
            break;
        size_t aligned = (next + 7u) & ~(size_t)7u;
        if (r.off > next || aligned > size) {
            result = FLUX_ERROR_INVALID_ARGUMENT;
            break;
        }
        r.off = aligned;
    }
    if (result == FLUX_OK && r.off != size)
        result = FLUX_ERROR_INVALID_ARGUMENT;
    if (result == FLUX_OK)
        result = flux_encoder_finish(enc, out_list);
    flux_encoder_destroy(enc);
out:
    free(resources);
    return result;
}
