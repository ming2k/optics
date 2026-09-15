/* Publish immutable visuals from the resolved UI tree. Rendering consumes
 * only the published snapshot; there is no live-tree Canvas replay path. */
#include "../internal.h"
#include <math.h>
#include <stdatomic.h>

static inline flux_rect rect_intersect(flux_rect a, flux_rect b) {
    float x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    return (flux_rect){x, y, fmaxf(0, fminf(a.x + a.w, b.x + b.w) - x),
                       fmaxf(0, fminf(a.y + a.h, b.y + b.h) - y)};
}

typedef struct snapshot_node {
    lens_id id, semantic_parent;
    uint64_t incarnation, visual_revision;
    flux_rect bounds, clip;
    lens_scroll_geometry scroll;
    bool is_scroll;
    lens_semantics semantics;
    lens_band band;
    uint32_t band_order;
    bool overlay, hit_transparent;
    size_t next_overlay;
} snapshot_node;

struct lens_scene_snapshot {
    atomic_uint ref_count;
    uint64_t generation;
    flux_display_list *display_list;
    bool has_damage;
    uint64_t owner_identity;
    snapshot_node *nodes;
    size_t node_count, overlay_head;
};

static flux_result capture_metadata(lens *ui, lens_scene_snapshot *snapshot);

static flux_rect offset_rel(flux_rect box, flux_rect rel) {
    float w = rel.w > 0 ? rel.w : box.w - 2.0f * rel.x;
    float h = rel.h > 0 ? rel.h : box.h - 2.0f * rel.y;
    if (w < 0)
        w = 0;
    if (h < 0)
        h = 0;
    float x = (rel.x < 0.0f && rel.w > 0.0f) ? box.x + box.w + rel.x - w : box.x + rel.x;
    return (flux_rect){x, box.y + rel.y, w, h};
}

/* Snap a logical rect to the device-pixel grid so sharp edges (1 px
 * borders, glyph bitmaps) map cleanly to physical pixels.  Keeps the
 * right/bottom edge invariant so adjacent widgets don't drift apart. */
static flux_rect snap_rect(flux_rect r, float scale) {
    if (scale <= 0.0f || scale == 1.0f) {
        float x = roundf(r.x);
        float y = roundf(r.y);
        return (flux_rect){x, y, r.w + r.x - x, r.h + r.y - y};
    }
    float inv = 1.0f / scale;
    float x = roundf(r.x * scale) * inv;
    float y = roundf(r.y * scale) * inv;
    return (flux_rect){x, y, r.w + r.x - x, r.h + r.y - y};
}

static inline bool rect_overlaps(flux_rect a, flux_rect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

static inline bool rect_equal(flux_rect a, flux_rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void lensi_node_release_cache(lens *ui, lens_node *n) {
    (void)ui;
    flux_display_list_release(n->cached_dl);
    n->cached_dl = nullptr;
}

bool lensi_mark_subtree_changed(lens_node *n) {
    bool changed = false;
    if (n->phase != LENS_NODE_STABLE)
        changed = true;
    if (n->has_render_rect) {
        if (n->final_rect.x != n->render_rect.x || n->final_rect.y != n->render_rect.y ||
            n->final_rect.w != n->render_rect.w || n->final_rect.h != n->render_rect.h)
            changed = true;
    } else {
        /* First rendered frame with geometry: must paint. */
        if (n->final_rect.w > 0.0f || n->final_rect.h > 0.0f)
            changed = true;
    }
    if (n->opacity != n->last_opacity)
        changed = true;
    n->last_opacity = n->opacity;
    if (n->hover_t != n->last_hover_t || n->active_t != n->last_active_t)
        changed = true;
    if (n->cmd_hash != n->last_cmd_hash)
        changed = true;
    /* Child-list churn (removal/reorder/replacement) is invisible to the
     * per-node checks above: untouched children simply vanish from the
     * sibling walk. Compare the linked-child sequence hash instead. */
    if (n->child_hash != n->last_child_hash)
        changed = true;
    n->last_hover_t = n->hover_t;
    n->last_active_t = n->active_t;

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        bool child_changed = lensi_mark_subtree_changed(c);
        if (c->place != LENS_PLACE_ABS && child_changed)
            changed = true;
    }
    if (changed)
        n->visual_revision++;
    n->subtree_changed = changed;
    return changed;
}

void lensi_mark_dirty(lens *ui) {
    if (ui->root)
        lensi_mark_subtree_changed(ui->root);
}

flux_result lensi_compile_commands(lens *ui, flux_encoder *enc, flux_rect box, flux_rect clip,
                                   const lens_draw_cmd *cmds, uint32_t cmd_count, float alpha) {
    float scale = ui->scale > 0.0f ? ui->scale : 1.0f;
    flux_rect command_clip = clip;
    flux_rect command_clip_stack[16];
    uint32_t command_clip_depth = 0;

    for (uint32_t i = 0; i < cmd_count; i++) {
        size_t arena_mark = ui->arena.used;
        const lens_draw_cmd *c = &cmds[i];
        flux_rect r = offset_rel(box, c->rel);
        r = snap_rect(r, scale);

        bool is_clip_cmd = (c->kind == LENS_DRAW_CLIP_PUSH || c->kind == LENS_DRAW_CLIP_POP);
        if (!is_clip_cmd && !rect_overlaps(r, command_clip))
            continue;

        if (c->radius > 0.5f && c->radius >= fminf(r.w, r.h) * 0.5f - 0.001f && c->rel.w > 0.0f &&
            c->rel.h > 0.0f && fabsf(c->rel.w - c->rel.h) < 0.5f) {
            float side = fminf(r.w, r.h);
            r.x += (r.w - side) * 0.5f;
            r.y += (r.h - side) * 0.5f;
            r.w = side;
            r.h = side;
        }

        lens_draw_cmd faded;
        if (alpha < 1.0f) {
            faded = *c;
            faded.color = lensi_opacity_color(faded.color, alpha);
            faded.outline_color = lensi_opacity_color(faded.outline_color, alpha);
            c = &faded;
        }

        switch (c->kind) {
        case LENS_DRAW_RECT: {
            flux_geometry g =
                (c->radius > 0.5f) ? flux_geom_rrect(r, c->radius) : flux_geom_rect(r);
            flux_brush b = flux_brush_solid(c->color);
            flux_encoder_draw_geometry(enc, &g, &b);
            break;
        }
        case LENS_DRAW_BORDER: {
            float bw = c->width > 0 ? c->width : 1.0f;
            flux_geometry g =
                (c->radius > 0.5f) ? flux_geom_rrect(r, c->radius) : flux_geom_rect(r);
            g.stroke_width = bw;
            flux_brush b = flux_brush_solid(c->color);
            flux_encoder_draw_geometry(enc, &g, &b);
            break;
        }
        case LENS_DRAW_TAB_INDICATOR: {
            float thickness = c->width > 0.0f ? c->width : 3.0f;
            flux_rect indicator = {r.x, box.y + box.h - thickness, r.w, thickness};
            indicator = snap_rect(indicator, scale);
            flux_geometry g = flux_geom_rrect(indicator, thickness * 0.5f);
            flux_brush b = flux_brush_solid(c->color);
            flux_encoder_draw_geometry(enc, &g, &b);
            break;
        }
        case LENS_DRAW_CONNECTED_TAB: {
            float shoulder = fminf(c->width, fminf(r.w * 0.25f, r.h * 0.45f));
            float depth = fmaxf(0.0f, c->text_size);
            float bottom = r.y + r.h + depth;
            float radius = fminf(c->radius, fminf(r.w * 0.5f, r.h * 0.5f));
            bool connect_left = (c->flags & LENSI_TAB_CONNECT_LEFT) != 0;
            bool connect_right = (c->flags & LENSI_TAB_CONNECT_RIGHT) != 0;

            flux_path *p = NULL;
            flux_result path_result = flux_path_create(&p, &ui->arena);
            if (path_result != FLUX_OK)
                return path_result;
            {
                if (connect_left) {
                    flux_path_move_to(p, r.x - shoulder, bottom);
                    flux_path_cubic_to(p, r.x - shoulder * 0.42f, bottom, r.x,
                                       bottom - shoulder * 0.42f, r.x, bottom - shoulder);
                } else {
                    flux_path_move_to(p, r.x, bottom);
                }
                flux_path_line_to(p, r.x, r.y + radius);
                flux_path_cubic_to(p, r.x, r.y + radius * 0.45f, r.x + radius * 0.45f, r.y,
                                   r.x + radius, r.y);
                flux_path_line_to(p, r.x + r.w - radius, r.y);
                flux_path_cubic_to(p, r.x + r.w - radius * 0.45f, r.y, r.x + r.w,
                                   r.y + radius * 0.45f, r.x + r.w, r.y + radius);
                if (connect_right) {
                    flux_path_line_to(p, r.x + r.w, bottom - shoulder);
                    flux_path_cubic_to(p, r.x + r.w, bottom - shoulder * 0.42f,
                                       r.x + r.w + shoulder * 0.42f, bottom, r.x + r.w + shoulder,
                                       bottom);
                } else {
                    flux_path_line_to(p, r.x + r.w, bottom);
                }
                flux_path_close(p);

                flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = p}};
                flux_brush b = flux_brush_solid(c->color);
                flux_encoder_draw_geometry(enc, &g, &b);
            }
            break;
        }
        case LENS_DRAW_IMAGE: {
            if (c->image) {
                flux_geometry g = flux_geom_rect(r);
                flux_brush b = (flux_brush){
                    .kind = FLUX_BRUSH_IMAGE_PATTERN,
                    .image = {.image = c->image, .tint = c->color},
                    .opacity = 1.0f,
                };
                if (c->outline_width > 0.0f && c->outline_color != 0) {
                    float edge = c->outline_width;
                    flux_geometry underlay = flux_geom_rect(
                        (flux_rect){r.x - edge, r.y - edge, r.w + edge * 2.0f, r.h + edge * 2.0f});
                    flux_brush outline = b;
                    outline.image.tint = c->outline_color;
                    flux_encoder_draw_geometry(enc, &underlay, &outline);
                }
                flux_encoder_draw_geometry(enc, &g, &b);
            }
            break;
        }
        case LENS_DRAW_ICON: {
            if (c->icon_id < 0)
                break;
            const lens_icon_desc *desc = lensi_icon_desc(c->icon_id);
            if (!desc || !desc->cmds || desc->count == 0)
                break;

            float s = r.w / 24.0f;
            float ox = r.x;
            float oy = r.y;

            if (desc->runs && desc->run_count > 0) {
                for (uint32_t run = 0; run < desc->run_count; run++) {
                    const lens_icon_run *ri = &desc->runs[run];
                    uint32_t end = ri->first_cmd + ri->count;
                    if (end < ri->first_cmd || end > desc->count)
                        return FLUX_ERROR_INVALID_ARGUMENT;
                    flux_path *p = NULL;
                    flux_result path_result = flux_path_create(&p, &ui->arena);
                    if (path_result != FLUX_OK)
                        return path_result;
                    for (uint32_t i = ri->first_cmd; i < end; i++) {
                        const lens_icon_cmd *cmd = &desc->cmds[i];
                        const float *pp = cmd->params;
                        switch (cmd->type) {
                        case 0:
                            flux_path_move_to(p, pp[0] * s + ox, pp[1] * s + oy);
                            break;
                        case 1:
                            flux_path_line_to(p, pp[0] * s + ox, pp[1] * s + oy);
                            break;
                        case 2:
                            flux_path_cubic_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                               pp[3] * s + oy, pp[4] * s + ox, pp[5] * s + oy);
                            break;
                        case 3:
                            flux_path_quad_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                              pp[3] * s + oy);
                            break;
                        case 4:
                            flux_path_close(p);
                            break;
                        case 5:
                            flux_path_add_circle(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s);
                            break;
                        case 6: {
                            flux_rect ir = {pp[0] * s + ox, pp[1] * s + oy, pp[2] * s, pp[3] * s};
                            flux_path_add_rect(p, ir);
                            break;
                        }
                        }
                    }
                    uint32_t rc = ri->color;
                    uint8_t rr = (uint8_t)(rc >> 16), rg = (uint8_t)(rc >> 8), rb = (uint8_t)rc,
                            ra = (uint8_t)(rc >> 24);
                    flux_color color = rc == 0 ? c->color : flux_color_rgba_premul(rr, rg, rb, ra);
                    flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = p}};
                    if (!ri->fill) {
                        g.stroke_width = c->width > 0 ? c->width : 2.0f * s;
                    }
                    flux_brush b = flux_brush_solid(color);
                    flux_encoder_draw_geometry(enc, &g, &b);
                }
                break;
            }

            flux_path *p = NULL;
            flux_result path_result = flux_path_create(&p, &ui->arena);
            if (path_result != FLUX_OK)
                return path_result;

            for (uint32_t i = 0; i < desc->count; i++) {
                const lens_icon_cmd *cmd = &desc->cmds[i];
                const float *pp = cmd->params;
                switch (cmd->type) {
                case 0:
                    flux_path_move_to(p, pp[0] * s + ox, pp[1] * s + oy);
                    break;
                case 1:
                    flux_path_line_to(p, pp[0] * s + ox, pp[1] * s + oy);
                    break;
                case 2:
                    flux_path_cubic_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                       pp[3] * s + oy, pp[4] * s + ox, pp[5] * s + oy);
                    break;
                case 3:
                    flux_path_quad_to(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s + ox,
                                      pp[3] * s + oy);
                    break;
                case 4:
                    flux_path_close(p);
                    break;
                case 5:
                    flux_path_add_circle(p, pp[0] * s + ox, pp[1] * s + oy, pp[2] * s);
                    break;
                case 6: {
                    flux_rect ir = {pp[0] * s + ox, pp[1] * s + oy, pp[2] * s, pp[3] * s};
                    flux_path_add_rect(p, ir);
                    break;
                }
                }
            }

            flux_geometry g = {.kind = FLUX_GEOM_PATH, .path = {.path = p}};
            if (lensi_icon_mode(c->icon_id) != LENSI_ICON_RENDER_FILL) {
                g.stroke_width = c->width > 0 ? c->width : 2.0f * s;
            }
            if (c->outline_width > 0.0f && c->outline_color != 0) {
                flux_geometry outline = g;
                outline.stroke_width = g.stroke_width + c->outline_width * 2.0f;
                flux_brush outline_brush = flux_brush_solid(c->outline_color);
                flux_encoder_draw_geometry(enc, &outline, &outline_brush);
            }
            flux_brush b = flux_brush_solid(c->color);
            flux_encoder_draw_geometry(enc, &g, &b);
            break;
        }
        case LENS_DRAW_CLIP_PUSH: {
            if (command_clip_depth >= 16)
                return FLUX_ERROR_OUT_OF_RANGE;
            {
                command_clip_stack[command_clip_depth++] = command_clip;
                command_clip = rect_intersect(command_clip, r);
                flux_encoder_save(enc);
                flux_encoder_clip_rect(enc, command_clip);
            }
            break;
        }
        case LENS_DRAW_CLIP_POP: {
            if (!command_clip_depth)
                return FLUX_ERROR_INVALID_STATE;
            {
                flux_encoder_restore(enc);
                command_clip = command_clip_stack[--command_clip_depth];
            }
            break;
        }
        case LENS_DRAW_TEXT: {
            if (ui->text && c->text && c->text[0]) {
                const char *end = strstr(c->text, "##");
                size_t vlen = end ? (size_t)(end - c->text) : strlen(c->text);
                if (vlen) {
                    float x = r.x;
                    float y = r.y;
                    const flux_text_style style = {
                        .size_px = c->text_size,
                        .weight = c->text_weight,
                        .color = c->color,
                        .family = (flux_text_family)c->text_family,
                    };
                    if (c->rel.w < 0.0f || c->rel.h < 0.0f) {
                        flux_text_metrics tm = flux_text_measure(ui->text, c->text, vlen, &style);
                        if (c->rel.w < 0.0f) {
                            x = r.x + (r.w - tm.width) * 0.5f;
                            if (x < r.x)
                                x = r.x;
                        }
                        if (c->rel.h < 0.0f) {
                            y = r.y + (r.h - tm.height) * 0.5f;
                            if (y < r.y)
                                y = r.y;
                        }
                    }
                    flux_result result = flux_text_record(
                        ui->text, enc,
                        &(flux_text_record_desc){.x = x,
                                                 .y = y,
                                                 .utf8 = c->text,
                                                 .len = vlen,
                                                 .style = style,
                                                 .outline_color = c->outline_color,
                                                 .outline_width = c->outline_width});
                    if (result != FLUX_OK)
                        return result;
                }
            }
            break;
        }
        default:
            return FLUX_ERROR_UNSUPPORTED;
        }
        ui->arena.used = arena_mark;
    }
    if (command_clip_depth)
        return FLUX_ERROR_INVALID_STATE;
    return FLUX_OK;
}

static flux_result lensi_compile_node(lens *ui, flux_encoder *enc, lens_node *n, flux_rect clip);

static flux_result lensi_compile_node_body(lens *ui, flux_encoder *enc, lens_node *n,
                                           flux_rect clip) {
    flux_rect box = n->final_rect;
    bool has_group_opacity = (n->opacity >= 0.0f && n->opacity < 1.0f);
    if (has_group_opacity) {
        flux_encoder_save_layer(enc, &box, n->opacity);
    }

    flux_result result = lensi_compile_commands(ui, enc, box, clip, n->cmds, n->cmd_count, 1.0f);
    if (result != FLUX_OK)
        return result;
    if (has_group_opacity)
        flux_encoder_restore(enc);

    bool pushed_clip = false;
    if (n->is_scroll && n->first_child) {
        float viewport_w = box.w - 2.0f * n->pad - n->scroll_gutter;
        if (viewport_w < 0.0f)
            viewport_w = 0.0f;
        flux_rect viewport = {box.x + n->pad, box.y + n->pad, viewport_w, box.h - 2.0f * n->pad};
        clip = rect_intersect(clip, viewport);
        if (clip.w <= 0.0f || clip.h <= 0.0f) {
            return FLUX_OK;
        }
        flux_encoder_save(enc);
        flux_encoder_clip_rect(enc, clip);
        pushed_clip = true;
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == LENS_PLACE_ABS)
            continue;
        result = lensi_compile_node(ui, enc, c, clip);
        if (result != FLUX_OK)
            return result;
    }

    if (pushed_clip)
        flux_encoder_restore(enc);
    return FLUX_OK;
}

static flux_result lensi_compile_node(lens *ui, flux_encoder *enc, lens_node *n, flux_rect clip) {
    if (!n || (n->final_rect.w <= 0.0f && n->final_rect.h <= 0.0f))
        return FLUX_OK;
    if (n->cached_dl && n->cached_revision == n->visual_revision && n->cached_scale == ui->scale &&
        rect_equal(n->cached_clip, clip))
        return flux_encoder_draw_display_list(enc, n->cached_dl);

    flux_encoder *sub_enc = nullptr;
    flux_result result = flux_encoder_create(nullptr, &sub_enc);
    if (result != FLUX_OK)
        return result;
    flux_display_list *replacement = nullptr;
    result = lensi_compile_node_body(ui, sub_enc, n, clip);
    if (result == FLUX_OK)
        result = flux_encoder_finish(sub_enc, &replacement);
    flux_encoder_destroy(sub_enc);
    if (result != FLUX_OK)
        return result;

    result = flux_encoder_draw_display_list(enc, replacement);
    if (result == FLUX_OK) {
        flux_display_list_release(n->cached_dl);
        n->cached_dl = replacement;
        n->cached_clip = clip;
        n->cached_revision = n->visual_revision;
        n->cached_scale = ui->scale;
    } else {
        flux_display_list_release(replacement);
    }
    return result;
}

static flux_result compile_visuals(lens *ui, flux_display_list **out_list) {
    *out_list = nullptr;
    if (ui->overflow)
        return FLUX_ERROR_OUT_OF_MEMORY;
    size_t arena_mark = ui->arena.used;
    flux_encoder *enc = nullptr;
    flux_result r = flux_encoder_create(nullptr, &enc);
    if (r != FLUX_OK)
        return r;

    bool scaled = ui->scale > 0.0f && ui->scale != 1.0f;
    if (scaled) {
        flux_encoder_save(enc);
        flux_encoder_scale(enc, ui->scale, ui->scale);
    }
    flux_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};

    for (uint32_t i = 0; i < ui->band_counts[LENS_BAND_BACKDROP]; ++i) {
        lens_node *n = ui->bands[LENS_BAND_BACKDROP][i];
        r = lensi_compile_node(ui, enc, n, n->has_place_bounds ? n->place_bounds : no_clip);
        if (r != FLUX_OK)
            goto done;
    }

    if (ui->root) {
        r = lensi_compile_node(ui, enc, ui->root, no_clip);
        if (r != FLUX_OK)
            goto done;
    }

    for (lens_band b = LENS_BAND_CHROME; b < LENS_BAND_COUNT; b++) {
        for (uint32_t i = 0; i < ui->band_counts[b]; ++i) {
            lens_node *n = ui->bands[b][i];
            r = lensi_compile_node(ui, enc, n, n->has_place_bounds ? n->place_bounds : no_clip);
            if (r != FLUX_OK)
                goto done;
        }
    }

    r = lensi_ghost_compile(ui, enc);
    if (r != FLUX_OK)
        goto done;

    if (ui->tooltip.active) {
        const lens_theme *t = &ui->theme;
        float pad = 4.0f;
        float size = lensi_font_px(ui, t->font_size * 0.85f);
        lens_text_metrics tm = lensi_text_measure_label(ui, ui->tooltip.text, size, 0.0f);
        float w = tm.width + 2.0f * pad;
        float h = tm.height + 2.0f * pad;
        float x = ui->tooltip.anchor.x;
        float y = ui->tooltip.anchor.y + ui->tooltip.anchor.h + 4.0f;
        flux_rect bg = {x, y, w, h};
        flux_geometry g_bg = flux_geom_rect(bg);
        flux_brush b_bg = flux_brush_solid(lensi_opacity_color(t->color_bg, ui->tooltip.opacity));
        flux_encoder_draw_geometry(enc, &g_bg, &b_bg);

        flux_geometry g_border = flux_geom_rect(bg);
        g_border.stroke_width = 1.0f;
        flux_brush b_border =
            flux_brush_solid(lensi_opacity_color(t->color_border, ui->tooltip.opacity));
        flux_encoder_draw_geometry(enc, &g_border, &b_border);

        if (ui->text) {
            flux_text_style ts = {.size_px = size,
                                  .color = lensi_opacity_color(t->color_fg, ui->tooltip.opacity)};
            r = flux_text_record(ui->text, enc,
                                 &(flux_text_record_desc){.x = x + pad,
                                                          .y = y + pad,
                                                          .utf8 = ui->tooltip.text,
                                                          .len = strlen(ui->tooltip.text),
                                                          .style = ts});
            if (r != FLUX_OK)
                goto done;
        }
    }

    if (scaled)
        flux_encoder_restore(enc);

    r = flux_encoder_finish(enc, out_list);
done:
    ui->arena.used = arena_mark;
    flux_encoder_destroy(enc);
    if (r != FLUX_OK)
        return r;

    return FLUX_OK;
}

flux_result lens_snapshot_create(lens *ui, lens_scene_snapshot **out_snapshot) {
    if (!out_snapshot)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out_snapshot = nullptr;
    if (!ui)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (ui->building || !ui->generation)
        return FLUX_ERROR_INVALID_STATE;
    lens_scene_snapshot *s = calloc(1, sizeof(*s));
    if (!s)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&s->ref_count, 1u);
    s->owner_identity = ui->identity;
    s->overlay_head = SIZE_MAX;
    s->has_damage = lens_frame_needs_repaint(ui);
    flux_result result = capture_metadata(ui, s);
    if (result == FLUX_OK)
        result = compile_visuals(ui, &s->display_list);
    if (result != FLUX_OK) {
        lens_snapshot_release(s);
        return result;
    }
    s->generation = ui->generation;
    *out_snapshot = s;
    return FLUX_OK;
}

const flux_display_list *lens_snapshot_display_list(const lens_scene_snapshot *snapshot) {
    return snapshot ? snapshot->display_list : nullptr;
}

lens_scene_snapshot *lens_snapshot_retain(lens_scene_snapshot *snapshot) {
    if (snapshot)
        atomic_fetch_add_explicit(&snapshot->ref_count, 1u, memory_order_relaxed);
    return snapshot;
}

void lens_snapshot_release(lens_scene_snapshot *snapshot) {
    if (!snapshot)
        return;
    if (atomic_fetch_sub_explicit(&snapshot->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    if (snapshot->display_list)
        flux_display_list_release(snapshot->display_list);
    for (size_t i = 0; i < snapshot->node_count; i++) {
        free((void *)snapshot->nodes[i].semantics.name);
        free((void *)snapshot->nodes[i].semantics.value);
    }
    free(snapshot->nodes);
    free(snapshot);
}

uint64_t lens_snapshot_generation(const lens_scene_snapshot *snapshot) {
    return snapshot ? snapshot->generation : 0;
}

flux_result lens_snapshot_submit(const lens_scene_snapshot *snapshot, flux_canvas *canvas) {
    if (!snapshot || !canvas)
        return FLUX_ERROR_INVALID_ARGUMENT;
    return flux_canvas_submit_display_list(canvas, snapshot->display_list);
}

static flux_result capture_node(lens *ui, lens_scene_snapshot *s, lens_node *n, flux_rect clip,
                                lens_id semantic_parent, lens_band band, uint32_t order,
                                bool hit_transparent) {
    if (!n)
        return FLUX_OK;
    if (n->place == LENS_PLACE_ABS) {
        clip = n->has_place_bounds ? n->place_bounds : (flux_rect){-1e6f, -1e6f, 2e6f, 2e6f};
        band = n->band;
        hit_transparent = band == LENS_BAND_BACKDROP && !n->interactive;
        for (order = 0; order < ui->band_counts[band]; order++)
            if (ui->bands[band][order] == n)
                break;
    }
    snapshot_node *entry = &s->nodes[s->node_count++];
    *entry = (snapshot_node){
        .id = n->id,
        .incarnation = n->incarnation,
        .visual_revision = n->visual_revision,
        .bounds = n->final_rect,
        .clip = clip,
        .semantic_parent = semantic_parent,
        .band = band,
        .band_order = order,
        .overlay = n->place == LENS_PLACE_ABS,
        .hit_transparent = hit_transparent,
        .semantics = {.role = n->semantics.role, .flags = n->semantics.flags},
    };
    if (n->is_scroll) {
        const lens_scroll_state *ss =
            n->state_bytes == sizeof(lens_scroll_state) ? n->state : nullptr;
        if (ss) {
            float width = ui->theme.scrollbar_width;
            float x = n->final_rect.x + n->final_rect.w - width;
            entry->is_scroll = true;
            entry->scroll = (lens_scroll_geometry){
                .thumb = {x, n->final_rect.y + ss->thumb_y, width, ss->thumb_h},
                .track = {x, n->final_rect.y, width, ss->track_len + ss->thumb_h},
                .offset_y = ss->offset_y,
                .track_len = ss->track_len,
                .scroll_range = ss->scroll_range,
            };
        }
    }
    if (entry->overlay) {
        entry->next_overlay = s->overlay_head;
        s->overlay_head = s->node_count - 1;
    }
    if (n->semantics.name) {
        entry->semantics.name = strdup(n->semantics.name);
        if (!entry->semantics.name)
            return FLUX_ERROR_OUT_OF_MEMORY;
    }
    if (n->semantics.value) {
        entry->semantics.value = strdup(n->semantics.value);
        if (!entry->semantics.value)
            return FLUX_ERROR_OUT_OF_MEMORY;
    }
    if (n->semantics.role != LENS_ROLE_NONE)
        semantic_parent = n->id;
    if (n->is_scroll) {
        flux_rect box = n->final_rect;
        flux_rect viewport = {box.x + n->pad, box.y + n->pad,
                              fmaxf(0, box.w - 2 * n->pad - n->scroll_gutter),
                              fmaxf(0, box.h - 2 * n->pad)};
        clip = rect_intersect(clip, viewport);
    }
    for (lens_node *child = n->first_child; child; child = child->next_sibling) {
        flux_result result =
            capture_node(ui, s, child, clip, semantic_parent, band, order, hit_transparent);
        if (result != FLUX_OK)
            return result;
    }
    return FLUX_OK;
}

static flux_result capture_metadata(lens *ui, lens_scene_snapshot *snapshot) {
    if (!ui->root)
        return FLUX_OK;
    snapshot->nodes = calloc(ui->store.count, sizeof(*snapshot->nodes));
    if (!snapshot->nodes)
        return FLUX_ERROR_OUT_OF_MEMORY;
    return capture_node(ui, snapshot, ui->root, (flux_rect){-1e6f, -1e6f, 2e6f, 2e6f}, 0,
                        LENS_BAND_BASE, 0, false);
}

flux_result lens_snapshot_activate(lens *ui, lens_scene_snapshot *snapshot) {
    if (!ui || !snapshot)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (ui->building || snapshot->owner_identity != ui->identity ||
        snapshot->generation > ui->generation ||
        snapshot->generation < ui->last_presented_generation)
        return FLUX_ERROR_INVALID_STATE;
    lens_snapshot_retain(snapshot);
    lens_snapshot_release(ui->presented_snapshot);
    ui->presented_snapshot = snapshot;
    ui->last_presented_generation = snapshot->generation;
    for (uint32_t i = 0; i < ui->store.cap; i++) {
        if (ui->store.slots[i].id)
            ui->store.slots[i].node->has_prev = false;
    }
    for (size_t i = 0; i < snapshot->node_count; i++) {
        const snapshot_node *entry = &snapshot->nodes[i];
        lens_node *n = lensi_store_find(ui, entry->id);
        if (!n || n->incarnation != entry->incarnation || n->phase == LENS_NODE_LEAVING)
            continue;
        n->prev_rect = entry->bounds;
        n->render_rect = entry->bounds;
        n->has_render_rect = true;
        n->presented_revision = entry->visual_revision;
        n->presented_index = i;
        n->has_prev = true;
    }
    return FLUX_OK;
}

static const snapshot_node *presented_node(const lens_node *n) {
    if (!n || !n->has_prev || !n->ui->presented_snapshot)
        return nullptr;
    const lens_scene_snapshot *s = n->ui->presented_snapshot;
    if (n->presented_index >= s->node_count)
        return nullptr;
    const snapshot_node *entry = &s->nodes[n->presented_index];
    return entry->id == n->id && entry->incarnation == n->incarnation ? entry : nullptr;
}

bool lensi_snapshot_scroll_geometry(const lens_node *n, lens_scroll_geometry *out) {
    const snapshot_node *entry = presented_node(n);
    if (!entry || !entry->is_scroll || !out)
        return false;
    *out = entry->scroll;
    return true;
}

bool lensi_snapshot_point_clipped(const lens_node *n, flux_point p) {
    const snapshot_node *entry = presented_node(n);
    return !entry || !lensi_point_in(p, entry->clip);
}

static unsigned band_rank(lens_band band) {
    return band == LENS_BAND_BACKDROP ? 0 : band == LENS_BAND_BASE ? 1 : (unsigned)band;
}

bool lensi_widget_occluded(const lens *ui, const lens_node *n) {
    const snapshot_node *entry = presented_node(n);
    if (!entry || entry->hit_transparent)
        return true;
    const lens_scene_snapshot *s = ui->presented_snapshot;
    for (size_t i = s->overlay_head; i != SIZE_MAX; i = s->nodes[i].next_overlay) {
        const snapshot_node *above = &s->nodes[i];
        if (!above->overlay || above->hit_transparent)
            continue;
        if (band_rank(above->band) > band_rank(entry->band) ||
            (above->band == entry->band && above->band_order > entry->band_order)) {
            if (lensi_point_in(ui->input.cursor, above->bounds) &&
                lensi_point_in(ui->input.cursor, above->clip))
                return true;
        }
    }
    return false;
}

void lens_snapshot_accessibility_walk(const lens_scene_snapshot *snapshot, lens_a11y_visit_fn visit,
                                      void *user) {
    if (!snapshot || !visit)
        return;
    for (size_t i = 0; i < snapshot->node_count; i++) {
        const snapshot_node *entry = &snapshot->nodes[i];
        if (entry->semantics.role != LENS_ROLE_NONE)
            visit(&entry->semantics, entry->bounds, entry->id, entry->semantic_parent, user);
    }
}
