/* ghost.c — leave-animation snapshots and their replay (ADR-0078). */

#include "../internal.h"
#include <math.h>
#include <string.h>

static inline flux_rect rect_intersect(flux_rect a, flux_rect b) {
    float x = fmaxf(a.x, b.x), y = fmaxf(a.y, b.y);
    return (flux_rect){x, y, fmaxf(0, fminf(a.x + a.w, b.x + b.w) - x),
                       fmaxf(0, fminf(a.y + a.h, b.y + b.h) - y)};
}

/* ---- deep clone into lensi_alloc memory -------------------------------- */

static void free_snapshot(lens *ui, lens_ghost_node *g);

static lens_ghost_node *snapshot_node(lens *ui, lens_node *n) {
    lens_ghost_node *g = lensi_alloc(ui, sizeof *g);
    if (!g) {
        lensi_set_overflow(ui);
        return NULL;
    }
    memset(g, 0, sizeof *g);
    g->final_rect =
        (n->final_rect.w > 0.0f && n->final_rect.h > 0.0f) ? n->final_rect : n->prev_rect;
    g->is_scroll = n->is_scroll;
    g->opacity = n->opacity;
    g->pad = n->pad;
    g->scroll_gutter = n->scroll_gutter;
    g->place_bounds = n->place_bounds;
    g->has_place_bounds = n->has_place_bounds;

    if (n->cmd_count) {
        g->cmds = lensi_alloc(ui, (size_t)n->cmd_count * sizeof *g->cmds);
        if (!g->cmds) {
            lensi_free(ui, g);
            lensi_set_overflow(ui);
            return NULL;
        }
        for (uint32_t i = 0; i < n->cmd_count; i++) {
            lens_draw_cmd *c = &n->cmds[i];
            g->cmds[i].cmd = *c;
            g->cmd_count++;
            if (c->kind == LENS_DRAW_IMAGE)
                g->cmds[i].cmd.image = flux_image_retain(c->image);
            if (c->kind == LENS_DRAW_TEXT && c->text) {
                size_t len = strlen(c->text) + 1;
                char *copy = lensi_alloc(ui, len);
                if (copy) {
                    memcpy(copy, c->text, len);
                    g->cmds[i].cmd.text = copy;
                } else {
                    g->cmds[i].cmd.text = NULL;
                    free_snapshot(ui, g);
                    lensi_set_overflow(ui);
                    return NULL;
                }
            }
        }
        g->cmd_count = n->cmd_count;
    }

    /* Tail-pointer append: `next` always points at a real link slot
     * (either &g->first_child or &prev->next_sibling), so the "is this
     * the first child" branch disappears and the analyzer's flow
     * sensitivity isn't load-bearing for memory safety. */
    lens_ghost_node **next = &g->first_child;
    for (lens_node *c = n->first_child; c; c = c->next_sibling) {
        lens_ghost_node *cg = snapshot_node(ui, c);
        if (!cg) {
            free_snapshot(ui, g);
            lensi_set_overflow(ui);
            return NULL;
        }
        *next = cg;
        next = &cg->next_sibling;
    }
    return g;
}

static void free_snapshot(lens *ui, lens_ghost_node *g) {
    while (g) {
        lens_ghost_node *next = g->next_sibling;
        if (g->first_child)
            free_snapshot(ui, g->first_child);
        if (g->cmds) {
            for (uint32_t i = 0; i < g->cmd_count; i++) {
                if (g->cmds[i].cmd.kind == LENS_DRAW_IMAGE)
                    flux_image_release(g->cmds[i].cmd.image);
                const char *t = g->cmds[i].cmd.text;
                if (g->cmds[i].cmd.kind == LENS_DRAW_TEXT && t)
                    lensi_free(ui, (void *)t);
            }
            lensi_free(ui, g->cmds);
        }
        lensi_free(ui, g);
        g = next;
    }
}

static uint32_t snapshot_cmd_count(const lens_node *n) {
    uint32_t count = n->cmd_count;
    for (lens_node *c = n->first_child; c; c = c->next_sibling)
        count += snapshot_cmd_count(c);
    return count;
}

/* ---- capture (lens_end, after mark_dirty) ----------------------------- */

void lensi_ghost_capture(lens *ui) {
    if (!ui)
        return;
    lens_store *s = &ui->store;
    const uint32_t link_none = UINT32_MAX;

    uint32_t cursor = s->live_head;
    while (cursor != link_none) {
        uint32_t i = cursor;
        lens_node *n = s->slots[i].node;
        cursor = s->slots[i].next_live;

        if (!ghost_wants(ui, n->id))
            continue;

        /* If already captured in a ghost, update alpha and refresh */
        bool already = false;
        for (uint32_t g = 0; g < ui->ghost_count; g++) {
            if (ui->ghosts[g]->root_id == n->id) {
                ui->ghosts[g]->refreshed_this_frame = true;
                for (uint32_t p = 0; p < ui->ghost_pin_count; p++) {
                    if (ui->ghost_pins[p] == n->id) {
                        ui->ghosts[g]->alpha = ui->ghost_pin_alphas[p];
                        break;
                    }
                }
                already = true;
                break;
            }
        }
        if (already)
            continue;

        bool enclosed = false;
        for (lens_node *p = n->parent; p; p = p->parent) {
            if (ghost_wants(ui, p->id)) {
                enclosed = true;
                break;
            }
        }
        if (enclosed || ui->ghost_count >= LENSI_GHOST_MAX)
            continue;
        flux_rect rect =
            (n->final_rect.w > 0.0f && n->final_rect.h > 0.0f) ? n->final_rect : n->prev_rect;
        if (rect.w <= 0.0f || rect.h <= 0.0f || snapshot_cmd_count(n) == 0u)
            continue;

        lens_ghost *gh = lensi_alloc(ui, sizeof *gh);
        if (!gh) {
            lensi_set_overflow(ui);
            continue;
        }
        memset(gh, 0, sizeof *gh);
        gh->root_id = n->id;
        gh->band = n->band;
        gh->frames_left = LENSI_GHOST_MAX_FRAMES;
        gh->alpha = 1.0f;
        for (uint32_t p = 0; p < ui->ghost_pin_count; p++) {
            if (ui->ghost_pins[p] == n->id) {
                gh->alpha = ui->ghost_pin_alphas[p];
                break;
            }
        }
        gh->root = snapshot_node(ui, n);
        if (!gh->root) {
            lensi_free(ui, gh);
            continue;
        }
        gh->refreshed_this_frame = true;
        ui->ghosts[ui->ghost_count++] = gh;
    }
}

/* ---- lifecycle (frame boundaries) --------------------------------------- */

void lensi_ghost_begin_frame(lens *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->ghost_count; i++)
        ui->ghosts[i]->refreshed_this_frame = false;
    ui->ghost_pin_count = 0;
}

void lensi_ghost_end_frame(lens *ui) {
    if (!ui)
        return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        lens_ghost *g = ui->ghosts[i];
        if (g->refreshed_this_frame) {
            g->frames_left = LENSI_GHOST_MAX_FRAMES;
            ui->ghosts[w++] = g;
        } else if (g->frames_left > 1) {
            g->frames_left--;
            ui->ghosts[w++] = g;
        } else {
            free_snapshot(ui, g->root);
            lensi_free(ui, g);
        }
    }
    ui->ghost_count = w;
}

/* ---- public API (lens_set_ghost) ---------------------------------------- */

void lensi_ghost_pin(lens *ui, lens_id id, float alpha) {
    if (!ui || id == 0)
        return;
    alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);

    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        if (ui->ghosts[i]->root_id == id) {
            ui->ghosts[i]->alpha = alpha;
            ui->ghosts[i]->refreshed_this_frame = true;
            return;
        }
    }

    for (uint32_t i = 0; i < ui->ghost_pin_count; i++) {
        if (ui->ghost_pins[i] == id) {
            ui->ghost_pin_alphas[i] = alpha;
            return;
        }
    }
    if (ui->ghost_pin_count < LENSI_GHOST_MAX) {
        ui->ghost_pins[ui->ghost_pin_count] = id;
        ui->ghost_pin_alphas[ui->ghost_pin_count] = alpha;
        ui->ghost_pin_count++;
    }
}

bool ghost_wants(const lens *ui, lens_id id) {
    if (!ui)
        return false;
    for (uint32_t i = 0; i < ui->ghost_pin_count; i++)
        if (ui->ghost_pins[i] == id)
            return true;
    for (uint32_t i = 0; i < ui->ghost_count; i++)
        if (ui->ghosts[i]->root_id == id)
            return true;
    return false;
}

bool lensi_ghost_active(const lens *ui) {
    return ui && ui->ghost_count > 0;
}

/* ---- render ------------------------------------------------------------- */

static flux_result compile_ghost_node(lens *ui, flux_encoder *encoder, const lens_ghost_node *g,
                                      flux_rect clip) {
    flux_rect box = g->final_rect;
    if (box.w <= 0.0f || box.h <= 0.0f)
        return FLUX_OK;
    if (g->opacity < 1.0f)
        flux_encoder_save_layer(encoder, nullptr, g->opacity);
    flux_result result = lensi_compile_commands(
        ui, encoder, box, clip, g->cmd_count ? &g->cmds[0].cmd : nullptr, g->cmd_count, 1.0f);
    if (result != FLUX_OK)
        return result;
    if (g->opacity < 1.0f)
        flux_encoder_restore(encoder);
    bool pushed = g->is_scroll && g->first_child;
    if (pushed) {
        float width = fmaxf(0.0f, box.w - 2.0f * g->pad - g->scroll_gutter);
        flux_rect viewport = {box.x + g->pad, box.y + g->pad, width,
                              fmaxf(0.0f, box.h - 2.0f * g->pad)};
        clip = rect_intersect(clip, viewport);
        if (clip.w <= 0 || clip.h <= 0)
            return FLUX_OK;
        flux_encoder_save(encoder);
        flux_encoder_clip_rect(encoder, clip);
    }
    for (const lens_ghost_node *c = g->first_child; c; c = c->next_sibling) {
        result = compile_ghost_node(ui, encoder, c, clip);
        if (result != FLUX_OK)
            return result;
    }
    if (pushed)
        flux_encoder_restore(encoder);
    return FLUX_OK;
}

flux_result lensi_ghost_compile(lens *ui, flux_encoder *encoder) {
    flux_rect no_clip = {-1e6f, -1e6f, 2e6f, 2e6f};
    for (lens_band b = LENS_BAND_BASE; b < LENS_BAND_COUNT; b++) {
        for (uint32_t i = 0; i < ui->ghost_count; i++) {
            lens_ghost *gh = ui->ghosts[i];
            if (gh->band != b || !gh->root)
                continue;
            lens_node *live = lensi_store_find(ui, gh->root_id);
            if (live && live->phase != LENS_NODE_LEAVING)
                continue;
            flux_rect clip = gh->root->has_place_bounds ? gh->root->place_bounds : no_clip;
            if (gh->alpha < 1.0f)
                flux_encoder_save_layer(encoder, nullptr, gh->alpha);
            else
                flux_encoder_save(encoder);
            flux_encoder_clip_rect(encoder, clip);
            flux_result result = compile_ghost_node(ui, encoder, gh->root, clip);
            if (result != FLUX_OK)
                return result;
            flux_encoder_restore(encoder);
        }
    }
    return FLUX_OK;
}

void lensi_ghost_destroy(lens *ui) {
    if (!ui)
        return;
    for (uint32_t i = 0; i < ui->ghost_count; i++) {
        free_snapshot(ui, ui->ghosts[i]->root);
        lensi_free(ui, ui->ghosts[i]);
    }
    ui->ghost_count = 0;
}
