/* replay.c — resolve the draw list against final_rect and emit canvas
 * calls (ADR-0030). Walks front-to-back (parent before children). */

#include "../internal.h"
#include <math.h>

/* Resolve a node-relative rect against the final box. A non-positive
 * rel.w / rel.h means "extend symmetrically to the box edge" (inset by
 * rel.x / rel.y on both sides), so widgets can say "fill me" with
 * rel = {0,0,0,0} before layout has assigned a size. */
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

static inline flux_rect rect_intersect(flux_rect a, flux_rect b) {
    float x1 = a.x > b.x ? a.x : b.x;
    float y1 = a.y > b.y ? a.y : b.y;
    float x2 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    float y2 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x2 <= x1 || y2 <= y1)
        return (flux_rect){0, 0, 0, 0};
    return (flux_rect){x1, y1, x2 - x1, y2 - y1};
}

/* Walk a node and replay its draw list. Exposed (non-static) so the
 * overlay layer (ADR-0037) can reuse the same emission for its sub-roots
 * — otherwise overlays would have to duplicate this logic. */
void lensi_render_node(lens *ui, flux_canvas *canvas, lens_node *n, flux_rect clip) {
    flux_rect box = n->final_rect;
    float scale = ui->scale > 0.0f ? ui->scale : 1.0f;

    /* Cull nodes that are completely outside the clip region. */
    if (!rect_overlaps(box, clip))
        return;

    /* Damage tracking: skip entire unchanged subtrees.
     * Root is always rendered (it may have no cmds but its children
     * are gated by their own subtree_changed).
     *
     * NOTE: disabled until subtree offscreen cache (ADR-0030) lands.
     * Today the host clears the whole framebuffer each frame, so
     * skipping a subtree leaves a blank hole. Re-enable once we
     * blit cached subtrees instead of replaying draw commands. */
    (void)n->subtree_changed; /* hash still computed for future use */

    /* Draw commands can nest logical clips (table viewport -> body -> cell).
     * Flux's clip_rect sets an absolute scissor rather than intersecting it,
     * so replay owns the intersection stack and always submits the effective
     * device-space rectangle. */
    flux_rect command_clip = clip;
    flux_rect command_clip_stack[16];
    uint32_t command_clip_depth = 0;
    for (uint32_t i = 0; i < n->cmd_count; i++) {
        const lens_draw_cmd *c = &n->cmds[i];
        flux_rect r = offset_rel(box, c->rel);
        r = snap_rect(r, scale);

        bool is_clip_cmd = c->kind == LENS_DRAW_CLIP_PUSH || c->kind == LENS_DRAW_CLIP_POP;
        /* Clip commands must stay balanced even if their rectangle is empty;
         * regular draws outside the current effective clip can be culled. */
        if (!is_clip_cmd && !rect_overlaps(r, command_clip))
            continue;

        /* Path tessellation and glyph shaping below allocate transient
         * scratch from the per-frame arena. That scratch is consumed by the
         * canvas immediately and never referenced again, so rewind the arena
         * after each command — otherwise render scratch accumulates across
         * the whole tree and a busy frame exhausts the arena, silently
         * dropping later draws (notably overlays, which render last). */
        size_t arena_mark = ui->arena.used;

        /* snap_rect keeps far edges fixed so adjacent widgets don't drift,
         * but for a circle (radius >= half the smaller side) this can turn
         * a square into a rectangle.  Force it back to a square so the
         * round-rect stays a perfect circle.
         *
         * Only do this when the original draw command was already a square
         * (e.g. a radio knob or checkbox checkmark).  Long bars like
         * progress tracks and slider rails have radius == height/2 but are
         * intentionally rectangular and must not be squashed to a dot. */
        if (c->radius > 0.5f && c->radius >= fminf(r.w, r.h) * 0.5f - 0.001f && c->rel.w > 0.0f &&
            c->rel.h > 0.0f && fabsf(c->rel.w - c->rel.h) < 0.5f) {
            float side = fminf(r.w, r.h);
            r.x += (r.w - side) * 0.5f;
            r.y += (r.h - side) * 0.5f;
            r.w = side;
            r.h = side;
        }

        switch (c->kind) {
        case LENS_DRAW_RECT:
            if (c->radius > 0.5f) {
                /* SDF fill: analytic AA, crisp at any DPI (incl. 100%). */
                flux_canvas_fill_rrect(canvas, r, c->radius, c->color);
            } else {
                flux_canvas_fill_rect_color(canvas, r, c->color);
            }
            break;

        case LENS_DRAW_CONNECTED_TAB: {
            /* The connected tab surface extends into the rail's lower inset.
             * Optional shoulders curve into adjacent tab space, so selection
             * reads as one continuous shape instead of an isolated pill. */
            float shoulder = fminf(c->width, fminf(r.w * 0.25f, r.h * 0.45f));
            float depth = fmaxf(0.0f, c->text_size);
            float bottom = r.y + r.h + depth;
            float radius = fminf(c->radius, fminf(r.w * 0.5f, r.h * 0.5f));
            bool connect_left = (c->flags & LENSI_TAB_CONNECT_LEFT) != 0;
            bool connect_right = (c->flags & LENSI_TAB_CONNECT_RIGHT) != 0;

            flux_path *p = NULL;
            if (flux_path_create(&p, &ui->arena) != FLUX_OK)
                break;

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
            flux_path_cubic_to(p, r.x + r.w - radius * 0.45f, r.y, r.x + r.w, r.y + radius * 0.45f,
                               r.x + r.w, r.y + radius);
            if (connect_right) {
                flux_path_line_to(p, r.x + r.w, bottom - shoulder);
                flux_path_cubic_to(p, r.x + r.w, bottom - shoulder * 0.42f,
                                   r.x + r.w + shoulder * 0.42f, bottom, r.x + r.w + shoulder,
                                   bottom);
            } else {
                flux_path_line_to(p, r.x + r.w, bottom);
            }
            flux_path_close(p);

            flux_paint paint = flux_paint_solid(c->color);
            flux_canvas_fill_path(canvas, p, &paint);
            break;
        }

        case LENS_DRAW_TAB_INDICATOR: {
            float thickness = c->width > 0.0f ? c->width : 3.0f;
            flux_rect indicator = {r.x, box.y + box.h - thickness, r.w, thickness};
            indicator = snap_rect(indicator, scale);
            flux_canvas_fill_rrect(canvas, indicator, thickness * 0.5f, c->color);
            break;
        }

        case LENS_DRAW_BORDER: {
            float bw = c->width > 0 ? c->width : 1.0f;
            if (c->radius > 0.5f) {
                /* SDF ring: analytic AA for rounded/circular borders. */
                flux_canvas_stroke_rrect(canvas, r, c->radius, c->color, bw);
            } else {
                flux_path *p = NULL;
                if (flux_path_create(&p, &ui->arena) == FLUX_OK) {
                    flux_path_add_rect(p, r);
                    flux_paint paint = flux_paint_solid(c->color);
                    paint.stroke_width = bw;
                    flux_canvas_stroke_path(canvas, p, &paint);
                }
            }
            break;
        }

        case LENS_DRAW_TEXT:
            /* Strip any "##key" id-disambiguation suffix and paint the
             * visible prefix through flux-text, which takes (ptr, len) so no
             * NUL-terminated copy is needed. The device scale is held on the
             * text context (kept in sync by lens_set_scale). */
            if (ui->text && c->text && c->text[0]) {
                const char *end = strstr(c->text, "##");
                size_t vlen = end ? (size_t)(end - c->text) : strlen(c->text);
                if (vlen) {
                    float x = r.x;
                    float y = r.y;
                    lens_text_metrics tm = {0};
                    bool measured_text = false;
                    if (c->rel.w < 0.0f) {
                        /* Negative rel.w means "center in the resolved rect". */
                        tm = lensi_text_measure_label(ui, c->text, c->text_size, c->text_weight);
                        measured_text = true;
                        x = r.x + (r.w - tm.width) * 0.5f;
                        if (x < r.x)
                            x = r.x;
                    }
                    if (c->rel.h < 0.0f) {
                        /* Negative rel.h means "center in the final node
                         * height". Unlike a build-time y offset, this remains
                         * correct when the parent constrains the node below
                         * its intrinsic padded height. */
                        if (!measured_text)
                            tm = lensi_text_measure_label(ui, c->text, c->text_size,
                                                          c->text_weight);
                        y = r.y + (r.h - tm.height) * 0.5f;
                        if (y < r.y)
                            y = r.y;
                    }

                    flux_text_draw(ui->text, canvas, &ui->arena, x, y, c->text, vlen,
                                   &(flux_text_style){.size_px = c->text_size,
                                                      .weight = c->text_weight,
                                                      .color = c->color});
                }
            }
            break;

        case LENS_DRAW_IMAGE:
            /* Host-owned raster texture (e.g. a decoded application icon),
             * scaled to fill the resolved rect. NULL image is a no-op so a
             * failed icon decode does not crash the frame. */
            if (c->image)
                flux_canvas_draw_image(canvas, c->image, r, NULL);
            break;

        case LENS_DRAW_CLIP_PUSH: {
            if (command_clip_depth >= 16) {
                ui->overflow = true;
                break;
            }
            command_clip_stack[command_clip_depth++] = command_clip;
            command_clip = rect_intersect(command_clip, r);
            flux_canvas_save(canvas);
            if (scale != 1.0f) {
                flux_rect device_clip = {command_clip.x * scale, command_clip.y * scale,
                                         command_clip.w * scale, command_clip.h * scale};
                flux_canvas_clip_rect(canvas, device_clip);
            } else {
                flux_canvas_clip_rect(canvas, command_clip);
            }
            break;
        }
        case LENS_DRAW_CLIP_POP:
            if (command_clip_depth > 0) {
                flux_canvas_restore(canvas);
                command_clip = command_clip_stack[--command_clip_depth];
            }
            break;

        case LENS_DRAW_ICON: {
            if (c->icon_id < 0 || c->icon_id >= (int32_t)LENS_ICON_COUNT)
                break;
            const lens_icon_desc *desc = &lens_icon_table[c->icon_id];
            if (!desc->cmds || desc->count == 0)
                break;

            float s = r.w / 24.0f;
            float ox = r.x;
            float oy = r.y;

            flux_path *p = NULL;
            if (flux_path_create(&p, &ui->arena) != FLUX_OK)
                break;

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

            flux_paint paint = flux_paint_solid(c->color);
            if (lens_icon_render_modes[c->icon_id] == LENSI_ICON_RENDER_FILL) {
                flux_canvas_fill_path(canvas, p, &paint);
            } else {
                paint.stroke_width = c->width > 0 ? c->width : 2.0f * s;
                paint.cap = FLUX_CAP_ROUND;
                paint.join = FLUX_JOIN_ROUND;
                flux_canvas_stroke_path(canvas, p, &paint);
            }
            break;
        }
        }

        ui->arena.used = arena_mark; /* free this command's scratch */
    }
    while (command_clip_depth > 0) {
        flux_canvas_restore(canvas);
        command_clip_depth--;
    }

    bool pushed_canvas_clip = false;
    if (n->is_scroll && n->first_child) {
        /* Layout reserves scroll_gutter from the children's cross axis when
         * a vertical scrollbar is present. Apply the same reservation to
         * the child clip: descendants such as long text can paint beyond
         * their own arranged box, and otherwise cover the scrollbar because
         * parent draw commands are replayed before child nodes. */
        float viewport_w = box.w - 2.0f * n->pad - n->scroll_gutter;
        if (viewport_w < 0.0f)
            viewport_w = 0.0f;
        flux_rect viewport = {box.x + n->pad, box.y + n->pad, viewport_w, box.h - 2.0f * n->pad};
        clip = rect_intersect(clip, viewport);
        if (clip.w <= 0.0f || clip.h <= 0.0f)
            return;
        flux_canvas_save(canvas);
        /* flux_canvas_clip_rect sets the scissor directly; it does not
         * apply the current canvas transform.  Convert logical clip to
         * device pixels to match the scale transform set in
         * lensi_render_tree (ADR-0030). */
        if (scale != 1.0f) {
            flux_rect device_clip = {clip.x * scale, clip.y * scale, clip.w * scale,
                                     clip.h * scale};
            flux_canvas_clip_rect(canvas, device_clip);
        } else {
            flux_canvas_clip_rect(canvas, clip);
        }
        pushed_canvas_clip = true;
    }

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        lensi_render_node(ui, canvas, c, clip);
    }

    if (pushed_canvas_clip) {
        flux_canvas_restore(canvas);
    }
}

/* ------------------------------------------------------------------ */
/*  Damage tracking — mark nodes whose geometry or appearance changed  */
/* ------------------------------------------------------------------ */

static bool mark_subtree_changed(lens_node *n) {
    bool changed = false;
    if (n->phase != LENS_NODE_STABLE)
        changed = true;
    if (n->has_prev) {
        if (n->final_rect.x != n->prev_rect.x || n->final_rect.y != n->prev_rect.y ||
            n->final_rect.w != n->prev_rect.w || n->final_rect.h != n->prev_rect.h)
            changed = true;
    } else {
        /* First frame with geometry: must paint. */
        if (n->final_rect.w > 0.0f || n->final_rect.h > 0.0f)
            changed = true;
    }
    if (n->hover_t != n->last_hover_t || n->active_t != n->last_active_t)
        changed = true;
    if (n->cmd_hash != n->last_cmd_hash)
        changed = true;
    n->last_hover_t = n->hover_t;
    n->last_active_t = n->active_t;

    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        if (mark_subtree_changed(c))
            changed = true;
    }
    n->subtree_changed = changed;
    return changed;
}

void lensi_mark_dirty(lens *ui) {
    if (ui->root)
        mark_subtree_changed(ui->root);
}

flux_result lensi_render_tree(lens *ui, flux_canvas *canvas) {
    if (!canvas)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!ui->root)
        return FLUX_OK;

    /* HiDPI: layout and draw commands stay in logical pixels; here we
     * scale the canvas transform so 1 logical px -> ui->scale device px.
     * save/restore so any caller transform survives this. */
    bool scaled = ui->scale > 0.0f && ui->scale != 1.0f;
    if (scaled) {
        flux_canvas_save(canvas);
        flux_canvas_scale(canvas, ui->scale, ui->scale);
    }
    flux_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};
    lensi_render_node(ui, canvas, ui->root, no_clip);
    lensi_overlay_render(ui, canvas); /* floating layers above the base */
    if (scaled)
        flux_canvas_restore(canvas);
    return FLUX_OK;
}

flux_result lens_render(lens *ui, flux_canvas *canvas) {
    if (!ui || !canvas)
        return FLUX_ERROR_INVALID_ARGUMENT;
    flux_result res = lensi_render_tree(ui, canvas);
    if (res != FLUX_OK)
        return res;

    /* Tooltip is drawn after the tree so it escapes any scroll clips. */
    if (ui->tooltip.active) {
        const lens_theme *t = &ui->theme;
        float pad = 4.0f;
        float size = t->font_size * 0.85f;
        float scale = ui->scale > 0.0f ? ui->scale : 1.0f;

        bool scaled = scale != 1.0f;
        if (scaled) {
            flux_canvas_save(canvas);
            flux_canvas_scale(canvas, scale, scale);
        }

        lens_text_metrics tm = lensi_text_measure_label(ui, ui->tooltip.text, size, 0.0f);
        float w = tm.width + 2.0f * pad;
        float h = tm.height + 2.0f * pad;

        float x = ui->tooltip.anchor.x;
        float y = ui->tooltip.anchor.y + ui->tooltip.anchor.h + 4.0f;

        flux_rect bg = {x, y, w, h};
        flux_canvas_fill_rect_color(canvas, bg, t->color_bg);

        flux_path *p = NULL;
        if (flux_path_create(&p, &ui->arena) == FLUX_OK) {
            flux_path_add_rect(p, bg);
            flux_paint paint = flux_paint_solid(t->color_border);
            paint.stroke_width = 1.0f;
            flux_canvas_stroke_path(canvas, p, &paint);
        }

        if (ui->text) {
            flux_text_draw(ui->text, canvas, &ui->arena, x + pad, y + pad,
                           ui->tooltip.text, strlen(ui->tooltip.text),
                           &(flux_text_style){.size_px = size, .color = t->color_fg});
        }
        if (scaled)
            flux_canvas_restore(canvas);
    }
    return FLUX_OK;
}
