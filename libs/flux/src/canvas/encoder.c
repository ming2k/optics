/*
 * encoder.c — Pure CPU display-list command encoder and submitter (ADR-0088 / ADR-0090 / ADR-0091).
 *
 * Separation of recording and execution:
 *   - flux_encoder records drawing commands with owned payload capture (paths,
 *     gradient stops, glyph quads, host coverage, image/sampler references).
 *     Zero GPU dependencies. Thread-safe by isolation.
 *   - flux_display_list is an immutable data slice of recorded commands that owns
 *     its command storage and retained resources. Resetting or destroying the encoder
 *     leaves published display lists 100% valid.
 *   - flux_canvas_submit_display_list unpacks the commands into a canvas pass.
 */

#include "internal.h"
#include <flux/canvas.h>
#include <flux/canvas_helpers.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Command binary format                                             */
/* ------------------------------------------------------------------ */

typedef enum flux_cmd_kind : uint8_t {
    FLUX_CMD_DRAW_GEOM = 1,
    FLUX_CMD_DRAW_GLYPH_RUN = 2,
    FLUX_CMD_SAVE = 3,
    FLUX_CMD_RESTORE = 4,
    FLUX_CMD_CLIP_RECT = 5,
    FLUX_CMD_SAVE_LAYER = 6,
    FLUX_CMD_TRANSLATE = 7,
    FLUX_CMD_SCALE = 8,
    FLUX_CMD_ROTATE = 9,
    FLUX_CMD_TRANSFORM = 10,
} flux_cmd_kind;

typedef struct flux_cmd_header {
    flux_cmd_kind kind;
    uint8_t _pad[3];
    uint32_t size; /* total command size in bytes */
} flux_cmd_header;

typedef struct flux_cmd_draw_geom {
    flux_cmd_header header;
    flux_geometry geom;
    flux_brush brush;
    uint32_t path_seg_count;
    /* Trailing payload:
     * - if path_seg_count > 0: flux_path + flux_path_segment[path_seg_count]
     */
} flux_cmd_draw_geom;

typedef struct flux_cmd_glyph_run {
    flux_cmd_header header;
    flux_glyph_run_desc desc;
    uint32_t quad_count;
    uint32_t host_cov_bytes;
    /* Trailing payload:
     * - flux_glyph_quad[quad_count]
     * - uint8_t[host_cov_bytes]
     */
} flux_cmd_glyph_run;

typedef struct flux_cmd_clip {
    flux_cmd_header header;
    flux_rect rect;
} flux_cmd_clip;

typedef struct flux_cmd_save_layer {
    flux_cmd_header header;
    flux_rect bounds;
    float opacity;
} flux_cmd_save_layer;

typedef struct flux_cmd_transform {
    flux_cmd_header header;
    flux_mat3x2 matrix;
} flux_cmd_transform;

typedef struct flux_cmd_vec2 {
    flux_cmd_header header;
    float x;
    float y;
} flux_cmd_vec2;

typedef struct flux_cmd_scalar {
    flux_cmd_header header;
    float value;
} flux_cmd_scalar;

/* ------------------------------------------------------------------ */
/*  Encoder structure                                                 */
/* ------------------------------------------------------------------ */

#define ENCODER_INITIAL_CAP (4096u)

struct flux_encoder {
    flux_arena *arena;
    uint8_t *buffer;
    size_t capacity;
    size_t used;
    uint32_t count;
    flux_rect bounds;
    bool finished;
    flux_result error;
};

static void *encoder_push_space(flux_encoder *enc, size_t bytes) {
    if (!enc || enc->finished)
        return nullptr;
    if (enc->error != FLUX_OK)
        return nullptr;
    size_t aligned_bytes = (bytes + 7u) & ~7u;
    if (enc->used + aligned_bytes > enc->capacity) {
        size_t new_cap = enc->capacity ? (enc->capacity * 2u) : ENCODER_INITIAL_CAP;
        while (new_cap < enc->used + aligned_bytes)
            new_cap *= 2u;
        uint8_t *new_buf = realloc(enc->buffer, new_cap);
        if (!new_buf) {
            enc->error = FLUX_ERROR_OUT_OF_MEMORY;
            return nullptr;
        }
        enc->buffer = new_buf;
        enc->capacity = new_cap;
    }
    void *ptr = enc->buffer + enc->used;
    enc->used += aligned_bytes;
    enc->count++;
    return ptr;
}

flux_result flux_encoder_create(flux_arena *arena, flux_encoder **out) {
    if (!out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    flux_encoder *enc = nullptr;
    if (arena) {
        enc = flux_arena_alloc_aligned(arena, sizeof(flux_encoder), alignof(flux_encoder));
    } else {
        enc = malloc(sizeof(flux_encoder));
    }
    if (!enc)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(enc, 0, sizeof(*enc));
    enc->arena = arena;
    enc->capacity = ENCODER_INITIAL_CAP;
    enc->buffer = malloc(enc->capacity);
    if (!enc->buffer) {
        if (!arena)
            free(enc);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    enc->error = FLUX_OK;
    *out = enc;
    return FLUX_OK;
}

void flux_encoder_reset(flux_encoder *enc) {
    if (!enc)
        return;
    if (enc->buffer) {
        if (!enc->finished) {
            flux_display_list tmp = {
                .commands = enc->buffer,
                .size = enc->used,
                .count = enc->count,
            };
            flux_display_list_destroy(&tmp);
        } else {
            free(enc->buffer);
        }
    }
    enc->buffer = malloc(ENCODER_INITIAL_CAP);
    enc->capacity = enc->buffer ? ENCODER_INITIAL_CAP : 0;
    enc->used = 0;
    enc->count = 0;
    enc->finished = false;
    enc->error = enc->buffer ? FLUX_OK : FLUX_ERROR_OUT_OF_MEMORY;
}

void flux_encoder_destroy(flux_encoder *enc) {
    if (!enc)
        return;
    if (enc->buffer && !enc->finished) {
        flux_display_list tmp = {
            .commands = enc->buffer,
            .size = enc->used,
            .count = enc->count,
        };
        flux_display_list_destroy(&tmp);
    }
    enc->buffer = nullptr;
    if (!enc->arena)
        free(enc);
}

void flux_encoder_draw_geometry(flux_encoder *enc, const flux_geometry *geom,
                                const flux_brush *brush) {
    if (!enc || !geom)
        return;

    uint32_t path_seg_count = 0;
    size_t path_payload_size = 0;
    if (geom->kind == FLUX_GEOM_PATH && geom->path.path) {
        path_seg_count = geom->path.path->count;
        path_payload_size = sizeof(flux_path) + path_seg_count * sizeof(flux_path_segment);
    }

    size_t total_cmd_size = sizeof(flux_cmd_draw_geom) + path_payload_size;
    flux_cmd_draw_geom *cmd = encoder_push_space(enc, total_cmd_size);
    if (!cmd)
        return;

    cmd->header.kind = FLUX_CMD_DRAW_GEOM;
    cmd->header.size = (uint32_t)total_cmd_size;
    cmd->geom = *geom;
    cmd->brush = brush ? *brush : flux_brush_solid(0xFF000000u);
    cmd->path_seg_count = path_seg_count;

    uint8_t *payload = (uint8_t *)(cmd + 1);
    if (path_seg_count > 0 && geom->path.path) {
        flux_path *p_dst = (flux_path *)payload;
        flux_path_segment *segs_dst = (flux_path_segment *)(p_dst + 1);
        p_dst->count = path_seg_count;
        p_dst->capacity = path_seg_count;
        p_dst->dropped = 0;
        p_dst->cursor_x = geom->path.path->cursor_x;
        p_dst->cursor_y = geom->path.path->cursor_y;
        p_dst->arena = nullptr;
        p_dst->segments = segs_dst;
        memcpy(segs_dst, geom->path.path->segments, path_seg_count * sizeof(flux_path_segment));
        payload += path_payload_size;
    }

    if (cmd->brush.kind == FLUX_BRUSH_IMAGE_PATTERN) {
        if (cmd->brush.image.image)
            (void)flux_image_retain(cmd->brush.image.image);
        if (cmd->brush.image.sampler)
            (void)flux_sampler_retain(cmd->brush.image.sampler);
    }
}

void flux_encoder_draw_glyph_run(flux_encoder *enc, const flux_glyph_run_desc *desc) {
    if (!enc || !desc)
        return;

    uint32_t quad_count = desc->quad_count;
    size_t quads_size = (size_t)quad_count * sizeof(flux_glyph_quad);
    uint32_t host_cov_bytes = 0;
    if (desc->host_coverage && desc->host_atlas_w > 0 && desc->host_atlas_h > 0) {
        host_cov_bytes = desc->host_atlas_w * desc->host_atlas_h;
    }

    size_t total_cmd_size = sizeof(flux_cmd_glyph_run) + quads_size + host_cov_bytes;
    flux_cmd_glyph_run *cmd = encoder_push_space(enc, total_cmd_size);
    if (!cmd)
        return;

    cmd->header.kind = FLUX_CMD_DRAW_GLYPH_RUN;
    cmd->header.size = (uint32_t)total_cmd_size;
    cmd->desc = *desc;
    cmd->quad_count = quad_count;
    cmd->host_cov_bytes = host_cov_bytes;

    uint8_t *payload = (uint8_t *)(cmd + 1);
    if (quad_count > 0 && desc->quads) {
        memcpy(payload, desc->quads, quads_size);
        payload += quads_size;
    }
    if (host_cov_bytes > 0 && desc->host_coverage) {
        memcpy(payload, desc->host_coverage, host_cov_bytes);
        payload += host_cov_bytes;
    }

    if (desc->atlas)
        (void)flux_image_retain(desc->atlas);
    if (desc->sampler)
        (void)flux_sampler_retain(desc->sampler);
}

void flux_encoder_save(flux_encoder *enc) {
    flux_cmd_header *cmd = encoder_push_space(enc, sizeof(flux_cmd_header));
    if (cmd) {
        cmd->kind = FLUX_CMD_SAVE;
        cmd->size = sizeof(flux_cmd_header);
    }
}

void flux_encoder_restore(flux_encoder *enc) {
    flux_cmd_header *cmd = encoder_push_space(enc, sizeof(flux_cmd_header));
    if (cmd) {
        cmd->kind = FLUX_CMD_RESTORE;
        cmd->size = sizeof(flux_cmd_header);
    }
}

void flux_encoder_clip_rect(flux_encoder *enc, flux_rect r) {
    flux_cmd_clip *cmd = encoder_push_space(enc, sizeof(flux_cmd_clip));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_CLIP_RECT;
        cmd->header.size = sizeof(flux_cmd_clip);
        cmd->rect = r;
    }
}

void flux_encoder_save_layer(flux_encoder *enc, const flux_rect *bounds, float opacity) {
    flux_cmd_save_layer *cmd = encoder_push_space(enc, sizeof(flux_cmd_save_layer));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_SAVE_LAYER;
        cmd->header.size = sizeof(flux_cmd_save_layer);
        cmd->bounds = bounds ? *bounds : (flux_rect){0.0f, 0.0f, 0.0f, 0.0f};
        cmd->opacity = opacity;
    }
}

void flux_encoder_translate(flux_encoder *enc, float x, float y) {
    flux_cmd_vec2 *cmd = encoder_push_space(enc, sizeof(flux_cmd_vec2));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_TRANSLATE;
        cmd->header.size = sizeof(flux_cmd_vec2);
        cmd->x = x;
        cmd->y = y;
    }
}

void flux_encoder_scale(flux_encoder *enc, float sx, float sy) {
    flux_cmd_vec2 *cmd = encoder_push_space(enc, sizeof(flux_cmd_vec2));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_SCALE;
        cmd->header.size = sizeof(flux_cmd_vec2);
        cmd->x = sx;
        cmd->y = sy;
    }
}

void flux_encoder_rotate(flux_encoder *enc, float radians) {
    flux_cmd_scalar *cmd = encoder_push_space(enc, sizeof(flux_cmd_scalar));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_ROTATE;
        cmd->header.size = sizeof(flux_cmd_scalar);
        cmd->value = radians;
    }
}

void flux_encoder_transform(flux_encoder *enc, flux_mat3x2 m) {
    flux_cmd_transform *cmd = encoder_push_space(enc, sizeof(flux_cmd_transform));
    if (cmd) {
        cmd->header.kind = FLUX_CMD_TRANSFORM;
        cmd->header.size = sizeof(flux_cmd_transform);
        cmd->matrix = m;
    }
}

flux_result flux_encoder_finish(flux_encoder *enc, flux_display_list *out_list) {
    if (!enc || !out_list)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (enc->error != FLUX_OK) {
        memset(out_list, 0, sizeof(*out_list));
        return enc->error;
    }
    enc->finished = true;
    out_list->commands = enc->buffer;
    out_list->size = enc->used;
    out_list->count = enc->count;
    out_list->bounds = enc->bounds;

    /* Move buffer ownership from encoder to out_list */
    enc->buffer = nullptr;
    enc->capacity = 0;
    enc->used = 0;
    enc->count = 0;
    return FLUX_OK;
}

void flux_display_list_destroy(flux_display_list *list) {
    if (!list || !list->commands || list->size == 0)
        return;

    const uint8_t *ptr = list->commands;
    const uint8_t *end = ptr + list->size;

    while (ptr < end) {
        const flux_cmd_header *h = (const flux_cmd_header *)ptr;
        if (h->size == 0 || ptr + h->size > end)
            break;

        if (h->kind == FLUX_CMD_DRAW_GEOM) {
            const flux_cmd_draw_geom *cmd = (const flux_cmd_draw_geom *)ptr;
            if (cmd->brush.kind == FLUX_BRUSH_IMAGE_PATTERN) {
                if (cmd->brush.image.image)
                    flux_image_release(cmd->brush.image.image);
                if (cmd->brush.image.sampler)
                    flux_sampler_release(cmd->brush.image.sampler);
            }
        } else if (h->kind == FLUX_CMD_DRAW_GLYPH_RUN) {
            const flux_cmd_glyph_run *cmd = (const flux_cmd_glyph_run *)ptr;
            if (cmd->desc.atlas)
                flux_image_release(cmd->desc.atlas);
            if (cmd->desc.sampler)
                flux_sampler_release(cmd->desc.sampler);
        }
        ptr += h->size;
    }

    free((void *)list->commands);
    memset(list, 0, sizeof(*list));
}

/* ------------------------------------------------------------------ */
/*  Canvas Execution of Geometry, Brushes, and DisplayList            */
/* ------------------------------------------------------------------ */

void flux_canvas_draw_geometry(flux_canvas *c, const flux_geometry *geom,
                               const flux_brush *brush) {
    if (!c || !geom)
        return;
    flux_brush b = brush ? *brush : flux_brush_solid(0xFF000000u);

    float alpha_scale = (b.opacity >= 0.0f && b.opacity <= 1.0f) ? b.opacity : 1.0f;

    flux_paint p = flux_paint_default();
    p.blend = b.blend;
    if (b.kind == FLUX_BRUSH_SOLID) {
        p.kind = FLUX_PAINT_SOLID;
        uint32_t color = b.solid.color;
        if (alpha_scale < 1.0f) {
            uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
            uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
            uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
            uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
            p.color = (a << 24) | (r << 16) | (g << 8) | bl;
        } else {
            p.color = color;
        }
    } else if (b.kind == FLUX_BRUSH_LINEAR_GRADIENT) {
        p.kind = FLUX_PAINT_LINEAR_GRADIENT;
        p.gradient.linear.from = b.gradient.start;
        p.gradient.linear.to = b.gradient.end;
        p.gradient.linear.stops = b.gradient.stops;
        if (alpha_scale < 1.0f) {
            for (uint32_t i = 0; i < p.gradient.linear.stops.count && i < 8; ++i) {
                uint32_t color = p.gradient.linear.stops.stops[i].color;
                uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
                uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
                uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
                uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
                p.gradient.linear.stops.stops[i].color = (a << 24) | (r << 16) | (g << 8) | bl;
            }
        }
    } else if (b.kind == FLUX_BRUSH_RADIAL_GRADIENT) {
        p.kind = FLUX_PAINT_RADIAL_GRADIENT;
        p.gradient.radial.center = b.gradient.start;
        p.gradient.radial.radius = b.gradient.radius;
        p.gradient.radial.stops = b.gradient.stops;
        if (alpha_scale < 1.0f) {
            for (uint32_t i = 0; i < p.gradient.radial.stops.count && i < 8; ++i) {
                uint32_t color = p.gradient.radial.stops.stops[i].color;
                uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
                uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
                uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
                uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
                p.gradient.radial.stops.stops[i].color = (a << 24) | (r << 16) | (g << 8) | bl;
            }
        }
    } else if (b.kind == FLUX_BRUSH_IMAGE_PATTERN) {
        if (b.image.image) {
            flux_rect dst;
            switch (geom->kind) {
            case FLUX_GEOM_RECT:
                dst = geom->rect.rect;
                break;
            case FLUX_GEOM_RRECT:
                dst = geom->rrect.rect;
                break;
            case FLUX_GEOM_SQUIRCLE:
                dst = geom->squircle.rect;
                break;
            case FLUX_GEOM_CIRCLE:
                dst = (flux_rect){geom->circle.cx - geom->circle.radius,
                                  geom->circle.cy - geom->circle.radius,
                                  geom->circle.radius * 2.0f,
                                  geom->circle.radius * 2.0f};
                break;
            case FLUX_GEOM_LINE:
                dst = (flux_rect){fminf(geom->line.x0, geom->line.x1),
                                  fminf(geom->line.y0, geom->line.y1),
                                  fabsf(geom->line.x1 - geom->line.x0),
                                  fabsf(geom->line.y1 - geom->line.y0)};
                break;
            default:
                dst = (flux_rect){0, 0, 100, 100};
                break;
            }
            flux_shape shape = {
                .kind = FLUX_SHAPE_IMAGE,
                .rect = dst,
                .image = b.image.image,
                .opaque_only = b.image.opaque_only,
            };
            if (geom->kind == FLUX_GEOM_RRECT) {
                shape.radius = geom->rrect.radius;
            }
            flux_canvas_draw(c, &shape, &p);
            return;
        }
    }

    switch (geom->kind) {
    case FLUX_GEOM_RECT:
        if (geom->stroke_width <= 0.0f) {
            canvas_fill_rect_internal(c, geom->rect.rect, &p);
        } else {
            flux_path_segment segs[5];
            flux_path path = {.segments = segs, .capacity = 5};
            flux_path_add_rect(&path, geom->rect.rect);
            p.stroke_width = geom->stroke_width;
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    case FLUX_GEOM_RRECT: {
        flux_rect r = geom->rrect.rect;
        float radius = geom->rrect.radius;
        if (geom->stroke_width <= 0.0f) {
            flux_shape s = flux_shape_rrect(r, radius);
            flux_canvas_draw(c, &s, &p);
        } else {
            flux_shape s = flux_shape_stroke_rrect(r, radius, geom->stroke_width);
            flux_canvas_draw(c, &s, &p);
        }
        break;
    }
    case FLUX_GEOM_SQUIRCLE: {
        flux_path_segment segs[32];
        flux_path path = {.segments = segs, .capacity = 32};
        flux_path_add_squircle(&path, geom->squircle.rect, geom->squircle.radius,
                               geom->squircle.curvature);
        if (geom->stroke_width <= 0.0f) {
            canvas_fill_path_internal(c, &path, &p);
        } else {
            p.stroke_width = geom->stroke_width;
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    }
    case FLUX_GEOM_CIRCLE: {
        float cx = geom->circle.cx;
        float cy = geom->circle.cy;
        float rad = geom->circle.radius;
        flux_rect r = {cx - rad, cy - rad, rad * 2.0f, rad * 2.0f};
        flux_shape s = (geom->stroke_width <= 0.0f)
                           ? flux_shape_rrect(r, rad)
                           : flux_shape_stroke_rrect(r, rad, geom->stroke_width);
        flux_canvas_draw(c, &s, &p);
        break;
    }
    case FLUX_GEOM_LINE: {
        flux_path_segment segs[3];
        flux_path path = {.segments = segs, .capacity = 3};
        flux_path_move_to(&path, geom->line.x0, geom->line.y0);
        flux_path_line_to(&path, geom->line.x1, geom->line.y1);
        p.stroke_width = geom->stroke_width > 0.0f ? geom->stroke_width : 1.0f;
        canvas_stroke_path_internal(c, &path, &p);
        break;
    }
    case FLUX_GEOM_PATH:
        if (geom->path.path) {
            if (geom->stroke_width <= 0.0f) {
                canvas_fill_path_internal(c, geom->path.path, &p);
            } else {
                p.stroke_width = geom->stroke_width;
                canvas_stroke_path_internal(c, geom->path.path, &p);
            }
        }
        break;
    default:
        break;
    }
}

flux_result flux_canvas_submit_display_list(flux_canvas *c, const flux_display_list *list) {
    if (!c || !list)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (list->size == 0 || !list->commands)
        return FLUX_OK;

    const uint8_t *ptr = list->commands;
    const uint8_t *end = ptr + list->size;

    while (ptr < end) {
        const flux_cmd_header *h = (const flux_cmd_header *)ptr;
        if (h->size == 0 || ptr + h->size > end)
            break;

        switch (h->kind) {
        case FLUX_CMD_DRAW_GEOM: {
            const flux_cmd_draw_geom *cmd = (const flux_cmd_draw_geom *)ptr;
            flux_geometry geom = cmd->geom;
            flux_brush brush = cmd->brush;
            const uint8_t *payload = (const uint8_t *)(cmd + 1);

            if (cmd->path_seg_count > 0 && geom.kind == FLUX_GEOM_PATH) {
                const flux_path *embedded_p = (const flux_path *)payload;
                geom.path.path = embedded_p;
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
                payload += cmd->host_cov_bytes;
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

    return FLUX_OK;
}
