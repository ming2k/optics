/* Publish immutable visuals from the resolved UI tree. Rendering consumes
 * only the published snapshot; there is no live-tree Canvas replay path. */
#include "../internal.h"
#include "icon_mask.h"
#include <math.h>
#include <stdatomic.h>

static inline optics_gfx_rect rect_intersect(optics_gfx_rect a, optics_gfx_rect b) {
    float x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    return (optics_gfx_rect){x, y, fmaxf(0, fminf(a.x + a.w, b.x + b.w) - x),
                             fmaxf(0, fminf(a.y + a.h, b.y + b.h) - y)};
}

typedef struct snapshot_node {
    optics_ui_id id, semantic_parent;
    uint64_t incarnation, visual_revision, self_revision;
    optics_gfx_rect bounds, clip;
    /* The pixels this node's own draw commands covered, resolved against
     * `bounds` at capture time (ADR-0144). `bounds` is where layout put the
     * node — for a stretched label that is the whole column — so it cannot
     * describe a vacated region. Captured while the command list is still
     * alive, and kept as a plain value so it survives the per-frame arena. */
    optics_gfx_rect paint_bounds;
    /* The clip this node imposed on its children, and the band it was composited
     * into (ADR-0149). Persisted here because the next frame's change detection
     * must compare against what was *presented*, not against what was merely
     * built — a frame that is built and then superseded never reached the screen,
     * so its clip and band are not the baseline. */
    optics_gfx_rect child_clip;
    optics_ui_scroll_geometry scroll;
    bool is_scroll;
    optics_ui_semantics semantics;
    optics_ui_band band;
    uint32_t band_order;
    bool overlay, hit_transparent;
    size_t next_overlay;
} snapshot_node;

struct optics_ui_scene_snapshot {
    atomic_uint ref_count;
    /* Retained device, for allocation attribution and ledger settlement
     * (ADR-0135). Deliberately NOT a `ui *`: a snapshot's last release can
     * happen after `optics_ui_release` (a host may hold a reference), so reaching
     * back through the context would be a use-after-free. The device is
     * refcounted and outlives the context, which is exactly the lifetime the
     * snapshot needs. */
    optics_gfx_device *device;
    uint64_t generation;
    optics_gfx_display_list *display_list;
    /* The caret phase this snapshot paints, captured at build time
     * (ADR-0151). Activation records it as "what is on screen", which is
     * what the next frame's phase comparison is made against. */
    bool caret_visible;
    uint64_t owner_identity;
    snapshot_node *nodes;
    size_t node_count, overlay_head;
};

static optics_gfx_result capture_metadata(optics_ui *ui, optics_ui_scene_snapshot *snapshot);

/* Resolve one draw command's `rel` geometry against the node box it was emitted
 * in. The definition lives in `internal.h` so damage tracking reports exactly
 * the pixels replay writes. */

/* Snap a logical rect to the device-pixel grid so sharp edges (1 px
 * borders, glyph bitmaps) map cleanly to physical pixels.  Keeps the
 * right/bottom edge invariant so adjacent widgets don't drift apart. */
static optics_gfx_rect snap_rect(optics_gfx_rect r, float scale) {
    /* Snapping moves the leading edge to the device grid and preserves the
     * trailing edge. For an extent smaller than one device pixel the leading
     * edge can round past the trailing edge, which would turn a legitimate
     * sub-pixel sliver into a negative width. gfx rejects the whole command
     * stream on a negative rect, so the snapped extent must be clamped: a
     * command with zero area simply draws nothing. */
    if (scale <= 0.0f || scale == 1.0f) {
        float x = roundf(r.x);
        float y = roundf(r.y);
        return (optics_gfx_rect){x, y, fmaxf(0.0f, r.w + r.x - x), fmaxf(0.0f, r.h + r.y - y)};
    }
    float inv = 1.0f / scale;
    float x = roundf(r.x * scale) * inv;
    float y = roundf(r.y * scale) * inv;
    return (optics_gfx_rect){x, y, fmaxf(0.0f, r.w + r.x - x), fmaxf(0.0f, r.h + r.y - y)};
}

static inline bool rect_overlaps(optics_gfx_rect a, optics_gfx_rect b) {
    return a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y;
}

static inline bool rect_equal(optics_gfx_rect a, optics_gfx_rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

void optics_ui__node_release_cache(optics_ui *ui, optics_ui_node *n) {
    (void)ui;
    optics_gfx_display_list_release(n->cached_dl);
    n->cached_dl = nullptr;
}

bool optics_ui__mark_subtree_changed(optics_ui *ui, optics_ui_node *n) {
    /* `self_changed` is the node's OWN paint delta: lifecycle, geometry,
     * opacity, eased hover/active, draw-list hash, and its own linked-child
     * sequence. `subtree_changed` adds every FLOW descendant's delta, because a
     * container's display list owns its flow children's commands.
     *
     * The distinction is what makes regional damage possible: optics_ui_damage_rects
     * consumes `subtree_changed` at ABS/band roots (a placed body owns its whole
     * subtree's pixels) but `self_changed` for FLOW interior nodes, so editing one
     * label inside a stretch column reports that label's rect rather than the
     * column's — which is the full display. */
    bool self = false;
    bool clip_changed = false;
    bool restacked = false;
    if (n->phase != OPTICS_UI_NODE_STABLE)
        self = true;
    n->prev_paint_rect = n->has_render_rect ? n->render_paint_rect : (optics_gfx_rect){0, 0, 0, 0};
    n->damage_prev_clip = n->has_prev_clip ? n->prev_clip : (optics_gfx_rect){0, 0, 0, 0};
    if (n->has_render_rect) {
        if (n->final_rect.x != n->render_rect.x || n->final_rect.y != n->render_rect.y ||
            n->final_rect.w != n->render_rect.w || n->final_rect.h != n->render_rect.h)
            self = true;
    } else {
        /* First rendered frame with geometry: must paint. */
        if (n->final_rect.w > 0.0f || n->final_rect.h > 0.0f)
            self = true;
    }
    if (n->opacity != n->last_opacity)
        self = true;
    n->last_opacity = n->opacity;
    if (n->hover_t != n->last_hover_t || n->active_t != n->last_active_t)
        self = true;
    if (n->cmd_hash != n->last_cmd_hash)
        self = true;
    /* Child-list churn (removal/reorder/replacement) is invisible to the
     * per-node checks above: untouched children simply vanish from the
     * sibling walk. Compare the linked-child sequence hash instead. */
    if (n->child_hash != n->last_child_hash)
        self = true;
    /* The clip this node imposed on its children changed. A scrolling viewport
     * that shrinks (a scrollbar appeared, a `place_bounds` clamp moved) reveals
     * and hides descendants' pixels without touching any descendant's own state,
     * and without touching this node's box, opacity or command list — so nothing
     * above notices. Compared against `prev_clip`, which is updated on snapshot
     * *activation* rather than here: the pixels that have to be erased are the
     * ones the last presented frame showed, not the ones this frame will. */
    if ((n->is_scroll && n->child_count > 0) || n->has_frame_clip) {
        const optics_gfx_rect clip_now = optics_ui__node_child_clip(n);
        if (!n->has_prev_clip || clip_now.x != n->prev_clip.x || clip_now.y != n->prev_clip.y ||
            clip_now.w != n->prev_clip.w || clip_now.h != n->prev_clip.h) {
            clip_changed = true;
            self = true;
        }
    }
    /* Re-compositing without repainting. Moving a placed node between bands or
     * reordering it within one changes which pixels it occludes, but leaves the
     * node's own geometry, opacity and draw list identical, so nothing else here
     * fires. Order is the node's index in the band bucket, which is what replay
     * actually composites by — `child_seq` is a sibling-disambiguation counter,
     * not a z-index, and comparing it would be comparing the wrong thing. */
    if (n->place == OPTICS_UI_PLACE_ABS) {
        uint32_t order = 0;
        for (uint32_t i = 0; i < ui->band_counts[n->band]; i++) {
            if (ui->bands[n->band][i] == n) {
                order = i;
                break;
            }
        }
        if (n->band != n->prev_band || order != n->prev_band_order) {
            restacked = true;
            self = true;
        }
    }
    n->last_hover_t = n->hover_t;
    n->last_active_t = n->active_t;

    bool changed = self;
    for (optics_ui_node *c = n->first_child; c; c = c->next_sibling) {
        bool child_changed = optics_ui__mark_subtree_changed(ui, c);
        if (c->place != OPTICS_UI_PLACE_ABS && child_changed)
            changed = true;
    }
    if (self) {
        n->self_revision++;
    }
    if (changed) {
        n->visual_revision++;
    }
    n->self_changed = self;
    n->clip_changed = clip_changed;
    n->restacked = restacked;
    n->subtree_changed = changed;
    return changed;
}

void optics_ui__mark_dirty(optics_ui *ui) {
    if (ui->root)
        optics_ui__mark_subtree_changed(ui, ui->root);
    for (uint32_t b = 0; b < (uint32_t)OPTICS_UI_BAND_COUNT; b++) {
        for (uint32_t i = 0; i < ui->band_counts[b]; i++) {
            if (ui->bands[b][i])
                optics_ui__mark_subtree_changed(ui, ui->bands[b][i]);
        }
    }
}

optics_gfx_result optics_ui__compile_commands(optics_ui *ui, optics_gfx_encoder *enc,
                                              optics_gfx_rect box, optics_gfx_rect clip,
                                              const optics_ui_draw_cmd *cmds, uint32_t cmd_count,
                                              float alpha) {
    float scale = ui->scale > 0.0f ? ui->scale : 1.0f;
    optics_gfx_rect command_clip = clip;
    optics_gfx_rect command_clip_stack[16];
    uint32_t command_clip_depth = 0;

    for (uint32_t i = 0; i < cmd_count; i++) {
        size_t arena_mark = ui->arena.used;
        const optics_ui_draw_cmd *c = &cmds[i];
        optics_gfx_rect r = optics_ui__cmd_rel_rect(box, c);
        r = snap_rect(r, scale);

        bool is_clip_cmd =
            (c->kind == OPTICS_UI_DRAW_CLIP_PUSH || c->kind == OPTICS_UI_DRAW_CLIP_POP);
        if (!is_clip_cmd && !rect_overlaps(r, command_clip))
            continue;

        optics_gfx_corner_radii cr_init = c->corner_radii;
        bool uniform_circle = cr_init.tl == cr_init.tr && cr_init.tr == cr_init.br &&
                              cr_init.br == cr_init.bl && cr_init.tl > 0.5f &&
                              cr_init.tl >= fminf(r.w, r.h) * 0.5f - 0.001f;
        if (uniform_circle && c->rel.w > 0.0f && c->rel.h > 0.0f &&
            fabsf(c->rel.w - c->rel.h) < 0.5f) {
            float side = fminf(r.w, r.h);
            r.x += (r.w - side) * 0.5f;
            r.y += (r.h - side) * 0.5f;
            r.w = side;
            r.h = side;
        }

        optics_ui_draw_cmd faded;
        if (alpha < 1.0f) {
            faded = *c;
            faded.color = optics_ui__opacity_color(faded.color, alpha);
            faded.outline_color = optics_ui__opacity_color(faded.outline_color, alpha);
            c = &faded;
        }

        switch (c->kind) {
        case OPTICS_UI_DRAW_RECT:
        case OPTICS_UI_DRAW_BORDER: {
            if (c->kind == OPTICS_UI_DRAW_BORDER && (c->width <= 0.0f || (c->color >> 24) == 0))
                break;
            optics_gfx_corner_radii cr = c->corner_radii;
            bool has_radius = cr.tl > 0.0f || cr.tr > 0.0f || cr.br > 0.0f || cr.bl > 0.0f;
            optics_gfx_geometry g = has_radius ? optics_gfx_geom_rrect(r, cr, c->corner_curvature)
                                               : optics_gfx_geom_rect(r);
            if (c->kind == OPTICS_UI_DRAW_BORDER)
                g.stroke.width = c->width;
            optics_gfx_brush b = optics_gfx_brush_solid(c->color);
            optics_gfx_encoder_draw_geometry(
                enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &b});
            break;
        }
        case OPTICS_UI_DRAW_IMAGE: {
            if (c->image) {
                optics_gfx_geometry g = optics_gfx_geom_rect(r);
                optics_gfx_brush b = (optics_gfx_brush){
                    .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                    .image = {.image = c->image, .tint = c->color},
                    .opacity = 1.0f,
                };
                if (c->outline_width > 0.0f && c->outline_color != 0) {
                    float edge = c->outline_width;
                    optics_gfx_geometry underlay = optics_gfx_geom_rect((optics_gfx_rect){
                        r.x - edge, r.y - edge, r.w + edge * 2.0f, r.h + edge * 2.0f});
                    optics_gfx_brush outline = b;
                    outline.image.tint = c->outline_color;
                    optics_gfx_encoder_draw_geometry(
                        enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &underlay,
                                                                      .brush = &outline});
                }
                optics_gfx_encoder_draw_geometry(
                    enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &b});
            }
            break;
        }
        case OPTICS_UI_DRAW_ICON: {
            if (c->icon_id == OPTICS_UI_ICON_NONE)
                break;
            const optics_ui_icon_desc *desc = optics_ui__icon_desc(ui, c->icon_id);
            if (!desc || !desc->cmds || desc->count == 0)
                break;

            optics_ui__icon_run_span spans[OPTICS_UI_ICON_MAX_RUNS];
            uint32_t span_count = optics_ui__icon_run_spans(desc, spans, OPTICS_UI_ICON_MAX_RUNS);
            if (span_count == 0)
                break;

            /* Device-space geometry. A mask is baked at one texel per device
             * pixel and blitted 1:1, so both the box edge and the origin must
             * be whole device pixels — a fractional origin would resample the
             * mask and blur exactly what it exists to keep crisp. The origin is
             * therefore snapped in *device* space: at scale 1.25/1.5 rounding a
             * logical coordinate lands between device pixels. */
            float dev_scale = scale > 0.0f ? scale : 1.0f;
            float icon_dim = c->rel.w > 0.0f ? c->rel.w : r.w;
            float dev_px_f = roundf(icon_dim * dev_scale);
            if (dev_px_f < 1.0f)
                dev_px_f = 1.0f;
            uint32_t dev_px = (uint32_t)dev_px_f;
            float dev_origin_x = floorf(r.x * dev_scale + 0.5f);
            float dev_origin_y = floorf(r.y * dev_scale + 0.5f);

            /* Stroke width in device pixels. Skins derive it as 2*(size/24);
             * scaling here keeps HiDPI honest instead of baking a line thinner
             * than the widget asked for. */
            float stroke_logical = c->width > 0.0f ? c->width : 2.0f * (icon_dim / 24.0f);
            float stroke_dev = stroke_logical * dev_scale;
            bool has_outline = c->outline_width > 0.0f && (c->outline_color >> 24) != 0;
            float outline_dev = has_outline ? c->outline_width * dev_scale : 0.0f;

            /* Coverage-mask path. Every layer of every run must bake, or none
             * does: a partially masked icon would mix two rasterisations inside
             * one glyph. */
            bool all_masks = true;
            for (uint32_t run = 0; run < span_count && all_masks; ++run) {
                if (!optics_ui__icon_mask_acquire(ui, c->icon_id, dev_px, stroke_dev, 0.0f, run,
                                                  OPTICS_UI_ICON_MASK_BODY))
                    all_masks = false;
                else if (has_outline && !optics_ui__icon_mask_acquire(ui, c->icon_id, dev_px,
                                                                      stroke_dev, outline_dev, run,
                                                                      OPTICS_UI_ICON_MASK_OUTLINE))
                    all_masks = false;
            }

            if (all_masks) {
                optics_gfx_sampler *sampler = optics_ui__icon_mask_sampler(ui);
                for (uint32_t run = 0; run < span_count; ++run) {
                    optics_gfx_color body_tint = optics_ui__opacity_color(
                        optics_ui__icon_run_color(spans[run].color, c->color), alpha);
                    optics_gfx_color outline_tint =
                        optics_ui__opacity_color(c->outline_color, alpha);
                    /* Outline first, body over it — the outline is painted
                     * beneath the body, exactly like the vector path. */
                    for (int pass = 0; pass < 2; ++pass) {
                        bool outline_pass = pass == 0;
                        if (outline_pass && !has_outline)
                            continue;
                        optics_ui__icon_mask *m = optics_ui__icon_mask_acquire(
                            ui, c->icon_id, dev_px, stroke_dev, outline_pass ? outline_dev : 0.0f,
                            run,
                            outline_pass ? OPTICS_UI_ICON_MASK_OUTLINE : OPTICS_UI_ICON_MASK_BODY);
                        if (!m)
                            continue;
                        optics_gfx_glyph_quad quad = {
                            .sx = (dev_origin_x - (float)m->halo) / dev_scale,
                            .sy = (dev_origin_y - (float)m->halo) / dev_scale,
                            .sw = (float)m->w / dev_scale,
                            .sh = (float)m->h / dev_scale,
                            .ax = 0,
                            .ay = 0,
                            .aw = (uint16_t)m->w,
                            .ah = (uint16_t)m->h,
                            .color = outline_pass ? outline_tint : body_tint,
                        };
                        optics_gfx_glyph_run_desc gd = OPTICS_GFX_GLYPH_RUN_DESC_INIT;
                        gd.atlas = m->image;
                        gd.host_coverage = m->host;
                        gd.host_atlas_w = m->host ? m->w : 0;
                        gd.host_atlas_h = m->host ? m->h : 0;
                        gd.sampler = sampler;
                        gd.quads = &quad;
                        gd.quad_count = 1;
                        optics_gfx_encoder_draw_glyph_run(enc, &gd);
                    }
                }
                break;
            }

            /* Vector fallback: reached only when a bake is impossible. It must
             * render the icon *exactly as described* — per-run colour, per-run
             * fill/stroke, and the outline underlay — because a fallback that
             * collapses the stream to one paint silently discards every colour
             * an SVG declares. */
            float ox = dev_origin_x / dev_scale;
            float oy = dev_origin_y / dev_scale;
            float s = icon_dim / 24.0f;

            for (uint32_t run = 0; run < span_count; ++run) {
                optics_gfx_path *p = nullptr;
                if (optics_gfx_path_create(&p, &ui->arena) != OPTICS_GFX_OK)
                    return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
                optics_ui__emit_run_path(p, desc->cmds, spans[run].first_cmd, spans[run].count, s,
                                         ox, oy);

                optics_gfx_geometry g = {.kind = OPTICS_GFX_GEOM_PATH, .path = {.path = p}};
                if (!spans[run].fill) {
                    g.stroke.width = stroke_logical;
                    g.stroke.cap = OPTICS_GFX_CAP_ROUND;
                    g.stroke.join = OPTICS_GFX_JOIN_ROUND;
                }
                optics_gfx_color body = optics_ui__opacity_color(
                    optics_ui__icon_run_color(spans[run].color, c->color), alpha);
                if (has_outline) {
                    optics_gfx_geometry outline = g;
                    outline.stroke.width = g.stroke.width + c->outline_width * 2.0f;
                    optics_gfx_brush ob =
                        optics_gfx_brush_solid(optics_ui__opacity_color(c->outline_color, alpha));
                    optics_gfx_encoder_draw_geometry(enc, &(optics_gfx_encoder_draw_geometry_opts){
                                                              .geom = &outline, .brush = &ob});
                }
                optics_gfx_brush bb = optics_gfx_brush_solid(body);
                optics_gfx_encoder_draw_geometry(
                    enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g, .brush = &bb});
            }
            break;
        }
        case OPTICS_UI_DRAW_CLIP_PUSH: {
            if (command_clip_depth >= 16)
                return OPTICS_GFX_ERROR_OUT_OF_RANGE;
            {
                command_clip_stack[command_clip_depth++] = command_clip;
                command_clip = rect_intersect(command_clip, r);
                optics_gfx_encoder_save(enc);
                optics_gfx_encoder_clip_rect(enc, command_clip);
            }
            break;
        }
        case OPTICS_UI_DRAW_CLIP_POP: {
            if (!command_clip_depth)
                return OPTICS_GFX_ERROR_INVALID_STATE;
            {
                optics_gfx_encoder_restore(enc);
                command_clip = command_clip_stack[--command_clip_depth];
            }
            break;
        }
        case OPTICS_UI_DRAW_TEXT: {
            /* Placement of the glyph run is resolved once, in
             * `optics_ui__text_bounds`, because damage tracking must report exactly
             * these pixels: the centered/edge-aligned sentinels in `rel` mean the
             * run is not at the command's corner. */
            if (ui->text && c->text && c->text[0]) {
                const char *end = strstr(c->text, "##");
                size_t vlen = end ? (size_t)(end - c->text) : strlen(c->text);
                if (vlen) {
                    const optics_text_style style = {
                        .size_px = c->text_size,
                        .weight = c->text_weight,
                        .color = c->color,
                        .family = (optics_text_family)c->text_family,
                    };
                    /* Positioned from the advance the widget recorded: replay
                     * and the damage walk must agree on these pixels without
                     * either of them re-shaping. */
                    const optics_gfx_rect run = optics_ui__text_bounds(
                        r, c->text_advance, c->rel.w < 0.0f, c->rel.h < 0.0f);
                    optics_gfx_result result = optics_text_record(
                        ui->text, &(optics_text_record_opts){.encoder = enc,
                                                             .x = run.x,
                                                             .y = run.y,
                                                             .utf8 = c->text,
                                                             .len = vlen,
                                                             .style = style,
                                                             .outline_color = c->outline_color,
                                                             .outline_width = c->outline_width});
                    if (result != OPTICS_GFX_OK)
                        return result;
                }
            }
            break;
        }
        default:
            return OPTICS_GFX_ERROR_UNSUPPORTED;
        }
        ui->arena.used = arena_mark;
    }
    if (command_clip_depth)
        return OPTICS_GFX_ERROR_INVALID_STATE;
    return OPTICS_GFX_OK;
}

static optics_gfx_result optics_ui__compile_node(optics_ui *ui, optics_gfx_encoder *enc,
                                                 optics_ui_node *n, optics_gfx_rect clip);

static optics_gfx_result optics_ui__compile_node_body(optics_ui *ui, optics_gfx_encoder *enc,
                                                      optics_ui_node *n, optics_gfx_rect clip) {
    optics_gfx_rect box = n->final_rect;
    optics_gfx_result result =
        optics_ui__compile_commands(ui, enc, box, clip, n->cmds, n->cmd_count, n->opacity);
    if (result != OPTICS_GFX_OK)
        return result;

    bool pushed_clip = false;
    if (n->is_scroll && n->first_child) {
        float viewport_w = box.w - 2.0f * n->pad - n->scroll_gutter;
        if (viewport_w < 0.0f)
            viewport_w = 0.0f;
        optics_gfx_rect viewport = {box.x + n->pad, box.y + n->pad, viewport_w,
                                    box.h - 2.0f * n->pad};
        clip = rect_intersect(clip, viewport);
        if (clip.w <= 0.0f || clip.h <= 0.0f) {
            return OPTICS_GFX_OK;
        }
        optics_gfx_encoder_save(enc);
        optics_gfx_encoder_clip_rect(enc, clip);
        pushed_clip = true;
    }

    for (optics_ui_node *c = n->first_child; c; c = c->next_sibling) {
        if (c->place == OPTICS_UI_PLACE_ABS)
            continue;
        result = optics_ui__compile_node(ui, enc, c, clip);
        if (result != OPTICS_GFX_OK)
            return result;
    }

    if (pushed_clip)
        optics_gfx_encoder_restore(enc);
    return OPTICS_GFX_OK;
}

static optics_gfx_result optics_ui__compile_node(optics_ui *ui, optics_gfx_encoder *enc,
                                                 optics_ui_node *n, optics_gfx_rect clip) {
    if (!n || (n->final_rect.w <= 0.0f && n->final_rect.h <= 0.0f))
        return OPTICS_GFX_OK;
    /* A scoped frame clip follows the node into its placement band, so it is
     * applied after the caller's place/ancestor clip rather than being lost
     * when an ABS node escapes its parent's viewport. */
    if (n->has_frame_clip)
        clip = optics_ui__rect_intersect(clip, n->frame_clip);
    if (n->cached_dl && n->cached_revision == n->visual_revision && n->cached_scale == ui->scale &&
        rect_equal(n->cached_clip, clip))
        return optics_gfx_encoder_draw_display_list(enc, n->cached_dl);

    optics_gfx_encoder *sub_enc = nullptr;
    optics_gfx_result result = optics_gfx_encoder_create(nullptr, &sub_enc);
    if (result != OPTICS_GFX_OK)
        return result;
    optics_gfx_display_list *replacement = nullptr;
    result = optics_ui__compile_node_body(ui, sub_enc, n, clip);
    if (result == OPTICS_GFX_OK)
        result = optics_gfx_encoder_finish(sub_enc, &replacement);
    optics_gfx_encoder_destroy(sub_enc);
    if (result != OPTICS_GFX_OK)
        return result;

    result = optics_gfx_encoder_draw_display_list(enc, replacement);
    if (result == OPTICS_GFX_OK) {
        optics_gfx_display_list_release(n->cached_dl);
        n->cached_dl = replacement;
        n->cached_clip = clip;
        n->cached_revision = n->visual_revision;
        n->cached_scale = ui->scale;
    } else {
        optics_gfx_display_list_release(replacement);
    }
    return result;
}

static optics_gfx_result compile_visuals(optics_ui *ui, optics_gfx_display_list **out_list) {
    *out_list = nullptr;
    if (ui->overflow)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    size_t arena_mark = ui->arena.used;
    optics_gfx_encoder *enc = nullptr;
    optics_gfx_result r = optics_gfx_encoder_create(nullptr, &enc);
    if (r != OPTICS_GFX_OK)
        return r;

    bool scaled = ui->scale > 0.0f && ui->scale != 1.0f;
    if (scaled) {
        optics_gfx_encoder_save(enc);
        optics_gfx_encoder_scale(
            enc, &(optics_gfx_encoder_scale_opts){.sx = ui->scale, .sy = ui->scale});
    }
    optics_gfx_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};

    for (uint32_t i = 0; i < ui->band_counts[OPTICS_UI_BAND_BACKDROP]; ++i) {
        optics_ui_node *n = ui->bands[OPTICS_UI_BAND_BACKDROP][i];
        r = optics_ui__compile_node(ui, enc, n, n->has_place_bounds ? n->place_bounds : no_clip);
        if (r != OPTICS_GFX_OK)
            goto done;
    }

    if (ui->root) {
        r = optics_ui__compile_node(ui, enc, ui->root, no_clip);
        if (r != OPTICS_GFX_OK)
            goto done;
    }

    for (optics_ui_band b = OPTICS_UI_BAND_CHROME; b < OPTICS_UI_BAND_COUNT; b++) {
        for (uint32_t i = 0; i < ui->band_counts[b]; ++i) {
            optics_ui_node *n = ui->bands[b][i];
            r = optics_ui__compile_node(ui, enc, n,
                                        n->has_place_bounds ? n->place_bounds : no_clip);
            if (r != OPTICS_GFX_OK)
                goto done;
        }
    }

    r = optics_ui__ghost_compile(ui, enc);
    if (r != OPTICS_GFX_OK)
        goto done;

    if (ui->tooltip.active) {
        const optics_ui_theme *t = &ui->theme;
        const float pad = 4.0f;
        const float size = optics_ui__font_px(ui, t->font_size * 0.85f);
        /* The box was measured at `optics_ui__tooltip` time (ADR-0149) precisely so the
         * damage walk reports the same pixels this draws without re-shaping. */
        const optics_gfx_rect bg = ui->tooltip.painted;
        const float x = bg.x;
        const float y = bg.y;
        optics_gfx_geometry g_bg = optics_gfx_geom_rect(bg);
        optics_gfx_color tt_bg =
            t->color_surface_elevated ? t->color_surface_elevated : t->color_bg;
        optics_gfx_brush b_bg =
            optics_gfx_brush_solid(optics_ui__opacity_color(tt_bg, ui->tooltip.opacity));
        optics_gfx_encoder_draw_geometry(
            enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &g_bg, .brush = &b_bg});

        if (t->border_width > 0.0f && (t->color_border & 0xFF000000u) != 0) {
            optics_gfx_geometry g_border = optics_gfx_geom_rect(bg);
            g_border.stroke.width = t->border_width;
            optics_gfx_brush b_border = optics_gfx_brush_solid(
                optics_ui__opacity_color(t->color_border, ui->tooltip.opacity));
            optics_gfx_encoder_draw_geometry(enc, &(optics_gfx_encoder_draw_geometry_opts){
                                                      .geom = &g_border, .brush = &b_border});
        }

        if (ui->text) {
            optics_text_style ts = {.size_px = size,
                                    .color =
                                        optics_ui__opacity_color(t->color_fg, ui->tooltip.opacity)};
            r = optics_text_record(ui->text,
                                   &(optics_text_record_opts){.encoder = enc,
                                                              .x = x + pad,
                                                              .y = y + pad,
                                                              .utf8 = ui->tooltip.text,
                                                              .len = strlen(ui->tooltip.text),
                                                              .style = ts});
            if (r != OPTICS_GFX_OK)
                goto done;
        }
    }

    if (scaled)
        optics_gfx_encoder_restore(enc);

    r = optics_gfx_encoder_finish(enc, out_list);
done:
    ui->arena.used = arena_mark;
    optics_gfx_encoder_destroy(enc);
    if (r != OPTICS_GFX_OK)
        return r;

    return OPTICS_GFX_OK;
}

/* Record a diagnostic for a failure ui detected itself (ADR-0153).
 *
 * gfx's slot is thread-local and is what every binding reads, so a ui-side
 * rejection that did not write it left the *previous* gfx diagnostic in place —
 * a different function, a different cause, or nothing. That is worse than no
 * detail: a reader would chase the wrong code path. Recording here keeps the
 * slot describing the failure the caller just received. */
#define OPTICS_UI_DIAG(result_, message_)                                                          \
    do {                                                                                           \
        optics_gfx_set_diagnostic(&(optics_gfx_error_info){.code = (result_),                      \
                                                           .function = __func__,                   \
                                                           .file = __FILE__,                       \
                                                           .line = __LINE__,                       \
                                                           .message = (message_)});                \
    } while (0)

optics_gfx_result optics_ui_snapshot_create(optics_ui *ui,
                                            optics_ui_scene_snapshot **out_snapshot) {
    if (!out_snapshot) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                       "optics_ui_snapshot_create needs an output slot");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out_snapshot = nullptr;
    if (!ui) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                       "optics_ui_snapshot_create needs a live ui context");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (ui->building) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_STATE,
                       "optics_ui_snapshot_create called while a frame is being built");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (!ui->generation) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_STATE,
                       "optics_ui_snapshot_create called before any frame was built");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_ui_scene_snapshot *s = optics_ui__alloc_by_device(ui->device, sizeof(*s));
    if (!s) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_OUT_OF_MEMORY, "could not allocate a scene snapshot");
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    atomic_init(&s->ref_count, 1u);
    s->device = optics_gfx_device_retain(ui->device);
    s->owner_identity = ui->identity;
    s->overlay_head = SIZE_MAX;
    /* Capture the phase this snapshot paints (ADR-0151). Taken here rather
     * than read back from the context at activation: the context may have
     * advanced its clock (and built a newer snapshot) by the time this one is
     * presented, and the pixels being published are this snapshot's. */
    s->caret_visible = ui->caret_visible;
    optics_gfx_result result = capture_metadata(ui, s);
    if (result == OPTICS_GFX_OK)
        result = compile_visuals(ui, &s->display_list);
    if (result != OPTICS_GFX_OK) {
        optics_ui_snapshot_release(s);
        return result;
    }
    s->generation = ui->generation;
    *out_snapshot = s;
    return OPTICS_GFX_OK;
}

const optics_gfx_display_list *
optics_ui_snapshot_display_list(const optics_ui_scene_snapshot *snapshot) {
    return snapshot ? snapshot->display_list : nullptr;
}

optics_ui_scene_snapshot *optics_ui_snapshot_retain(optics_ui_scene_snapshot *snapshot) {
    if (snapshot)
        atomic_fetch_add_explicit(&snapshot->ref_count, 1u, memory_order_relaxed);
    return snapshot;
}

void optics_ui_snapshot_release(optics_ui_scene_snapshot *snapshot) {
    if (!snapshot)
        return;
    if (atomic_fetch_sub_explicit(&snapshot->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    if (snapshot->display_list)
        optics_gfx_display_list_release(snapshot->display_list);
    for (size_t i = 0; i < snapshot->node_count; i++) {
        free((void *)snapshot->nodes[i].semantics.name);  // RAW_ALLOC_OK(paired-platform)
        free((void *)snapshot->nodes[i].semantics.value); // RAW_ALLOC_OK(paired-platform)
    }
    /* Order matters and is easy to get wrong: `snapshot` itself is about to be
     * freed, so its `device` field must be read into a local BEFORE the free,
     * and the device reference dropped AFTER — releasing it first left the
     * subsequent free reading a freed pointer, which is what this crash was. */
    optics_gfx_device *device = snapshot->device;
    optics_ui__free_by_device(device, snapshot->nodes);
    optics_ui__free_by_device(device, snapshot);
    if (device)
        optics_gfx_device_release(device);
}

uint64_t optics_ui_snapshot_generation(const optics_ui_scene_snapshot *snapshot) {
    return snapshot ? snapshot->generation : 0;
}

optics_gfx_result optics_ui_snapshot_submit(const optics_ui_scene_snapshot *snapshot,
                                            optics_gfx_canvas *canvas) {
    if (!snapshot || !canvas)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    return optics_gfx_canvas_submit_display_list(
        canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = snapshot->display_list});
}

static optics_gfx_result capture_node(optics_ui *ui, optics_ui_scene_snapshot *s, optics_ui_node *n,
                                      optics_gfx_rect clip, optics_ui_id semantic_parent,
                                      optics_ui_band band, uint32_t order, bool hit_transparent) {
    if (!n)
        return OPTICS_GFX_OK;
    if (n->place == OPTICS_UI_PLACE_ABS) {
        clip = n->has_place_bounds ? n->place_bounds : (optics_gfx_rect){-1e6f, -1e6f, 2e6f, 2e6f};
        band = n->band;
        hit_transparent = band == OPTICS_UI_BAND_BACKDROP && !n->interactive;
        for (order = 0; order < ui->band_counts[band]; order++)
            if (ui->bands[band][order] == n)
                break;
    }
    /* The scoped clip is a build-time stamp that survives ABS placement. The
     * snapshot's `clip` is both the hit region and the child inheritance, so
     * intersecting it here makes rendering and hit-testing agree. */
    if (n->has_frame_clip)
        clip = optics_ui__rect_intersect(clip, n->frame_clip);
    snapshot_node *entry = &s->nodes[s->node_count++];
    *entry = (snapshot_node){
        .id = n->id,
        .incarnation = n->incarnation,
        .visual_revision = n->visual_revision,
        .self_revision = n->self_revision,
        .bounds = n->final_rect,
        .paint_bounds = optics_ui__node_paint_bounds(n),
        .child_clip = optics_ui__node_child_clip(n),
        .clip = clip,
        .semantic_parent = semantic_parent,
        .band = band,
        .band_order = order,
        .overlay = n->place == OPTICS_UI_PLACE_ABS,
        .hit_transparent = hit_transparent,
        .semantics = {.role = n->semantics.role, .flags = n->semantics.flags},
    };
    if (n->is_scroll) {
        const optics_ui_scroll_state *ss =
            n->state_bytes == sizeof(optics_ui_scroll_state) ? n->state : nullptr;
        if (ss) {
            float width = ui->theme.scrollbar_width;
            float x = n->final_rect.x + n->final_rect.w - width;
            entry->is_scroll = true;
            entry->scroll = (optics_ui_scroll_geometry){
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
        entry->semantics.name = strdup(n->semantics.name); // RAW_ALLOC_OK(paired-platform)
        if (!entry->semantics.name)
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    if (n->semantics.value) {
        entry->semantics.value = strdup(n->semantics.value); // RAW_ALLOC_OK(paired-platform)
        if (!entry->semantics.value)
            return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    if (n->semantics.role != OPTICS_UI_ROLE_NONE)
        semantic_parent = n->id;
    if (n->is_scroll) {
        optics_gfx_rect box = n->final_rect;
        optics_gfx_rect viewport = {box.x + n->pad, box.y + n->pad,
                                    fmaxf(0, box.w - 2 * n->pad - n->scroll_gutter),
                                    fmaxf(0, box.h - 2 * n->pad)};
        clip = rect_intersect(clip, viewport);
    }
    for (optics_ui_node *child = n->first_child; child; child = child->next_sibling) {
        optics_gfx_result result =
            capture_node(ui, s, child, clip, semantic_parent, band, order, hit_transparent);
        if (result != OPTICS_GFX_OK)
            return result;
    }
    return OPTICS_GFX_OK;
}

static optics_gfx_result capture_metadata(optics_ui *ui, optics_ui_scene_snapshot *snapshot) {
    if (!ui->root)
        return OPTICS_GFX_OK;
    snapshot->nodes =
        optics_ui__alloc_by_device(ui->device, ui->store.count * sizeof(*snapshot->nodes));
    if (!snapshot->nodes)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    return capture_node(ui, snapshot, ui->root, (optics_gfx_rect){-1e6f, -1e6f, 2e6f, 2e6f}, 0,
                        OPTICS_UI_BAND_BASE, 0, false);
}

optics_gfx_result optics_ui_snapshot_activate(optics_ui *ui, optics_ui_scene_snapshot *snapshot) {
    if (!ui || !snapshot) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                       "optics_ui_snapshot_activate needs both a ui and a snapshot");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (ui->building) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_STATE,
                       "optics_ui_snapshot_activate called while a frame is being built");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (snapshot->owner_identity != ui->identity) {
        OPTICS_UI_DIAG(OPTICS_GFX_ERROR_INVALID_STATE,
                       "snapshot belongs to a different ui context");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (snapshot->generation > ui->generation) {
        OPTICS_UI_DIAG(
            OPTICS_GFX_ERROR_INVALID_STATE,
            optics_gfx_diagnostic_format("snapshot generation %llu is newer than the context's "
                                         "%llu",
                                         (unsigned long long)snapshot->generation,
                                         (unsigned long long)ui->generation));
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (snapshot->generation < ui->last_presented_generation) {
        OPTICS_UI_DIAG(
            OPTICS_GFX_ERROR_INVALID_STATE,
            optics_gfx_diagnostic_format("snapshot generation %llu predates the last presented "
                                         "%llu (stale snapshot)",
                                         (unsigned long long)snapshot->generation,
                                         (unsigned long long)ui->last_presented_generation));
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    optics_ui_snapshot_retain(snapshot);
    optics_ui_snapshot_release(ui->presented_snapshot);
    ui->presented_snapshot = snapshot;
    ui->last_presented_generation = snapshot->generation;
    for (uint32_t i = 0; i < ui->store.cap; i++) {
        if (ui->store.slots[i].id)
            ui->store.slots[i].node->has_prev = false;
    }
    for (size_t i = 0; i < snapshot->node_count; i++) {
        const snapshot_node *entry = &snapshot->nodes[i];
        optics_ui_node *n = optics_ui__store_find(ui, entry->id);
        if (!n || n->incarnation != entry->incarnation || n->phase == OPTICS_UI_NODE_LEAVING)
            continue;
        n->prev_rect = entry->bounds;
        n->render_rect = entry->bounds;
        n->render_paint_rect = entry->paint_bounds;
        n->has_render_rect = true;
        /* Presentation is the moment "what is on screen" becomes true, so the
         * clip and band this snapshot imposed are what the next change detection
         * compares against (ADR-0149). Advancing them at build time would compare
         * against a frame the user never saw. */
        n->prev_clip = entry->child_clip;
        n->has_prev_clip = true;
        n->prev_band = entry->band;
        n->prev_band_order = entry->band_order;
        n->presented_revision = entry->visual_revision;
        n->presented_self_revision = entry->self_revision;
        n->presented_index = i;
        n->has_prev = true;
    }
    /* Presentation is what makes a caret phase real (ADR-0151). Recording it
     * here — not at build time — is what lets a skipped frame keep its damage
     * pending: the next build compares its phase against the one the user is
     * actually looking at. */
    ui->caret_presented_visible = snapshot->caret_visible;
    return OPTICS_GFX_OK;
}

static const snapshot_node *presented_node(const optics_ui_node *n) {
    if (!n || !n->has_prev || !n->ui->presented_snapshot)
        return nullptr;
    const optics_ui_scene_snapshot *s = n->ui->presented_snapshot;
    if (n->presented_index >= s->node_count)
        return nullptr;
    const snapshot_node *entry = &s->nodes[n->presented_index];
    return entry->id == n->id && entry->incarnation == n->incarnation ? entry : nullptr;
}

bool optics_ui__snapshot_scroll_geometry(const optics_ui_node *n, optics_ui_scroll_geometry *out) {
    const snapshot_node *entry = presented_node(n);
    if (!entry || !entry->is_scroll || !out)
        return false;
    *out = entry->scroll;
    return true;
}

bool optics_ui__snapshot_point_clipped(const optics_ui_node *n, optics_gfx_point p) {
    const snapshot_node *entry = presented_node(n);
    return !entry || !optics_ui__point_in(p, entry->clip);
}

static unsigned band_rank(optics_ui_band band) {
    return band == OPTICS_UI_BAND_BACKDROP ? 0 : band == OPTICS_UI_BAND_BASE ? 1 : (unsigned)band;
}

bool optics_ui__widget_occluded(const optics_ui *ui, const optics_ui_node *n) {
    const snapshot_node *entry = presented_node(n);
    if (!entry || entry->hit_transparent)
        return true;
    const optics_ui_scene_snapshot *s = ui->presented_snapshot;
    for (size_t i = s->overlay_head; i != SIZE_MAX; i = s->nodes[i].next_overlay) {
        const snapshot_node *above = &s->nodes[i];
        if (!above->overlay || above->hit_transparent)
            continue;
        if (band_rank(above->band) > band_rank(entry->band) ||
            (above->band == entry->band && above->band_order > entry->band_order)) {
            if (optics_ui__point_in(ui->input.cursor, above->bounds) &&
                optics_ui__point_in(ui->input.cursor, above->clip))
                return true;
        }
    }
    return false;
}

void optics_ui_snapshot_accessibility_walk(const optics_ui_scene_snapshot *snapshot,
                                           optics_ui_a11y_visit_fn visit, void *user) {
    if (!snapshot || !visit)
        return;
    for (size_t i = 0; i < snapshot->node_count; i++) {
        const snapshot_node *entry = &snapshot->nodes[i];
        if (entry->semantics.role != OPTICS_UI_ROLE_NONE)
            visit(&entry->semantics, entry->bounds, entry->id, entry->semantic_parent, user);
    }
}
