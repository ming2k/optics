/* atlas.c — packed R8 coverage atlas, shelf rect allocation, subpixel
 * rasterisation, and the public glyph fetch glue.
 *
 * Responsibility split (see glyph_cache.h):
 *
 *   glyph_cache.c — owns the open-addressing hash table + LRU policy.
 *                   Decides "which glyphs do we know?"
 *   atlas.c (here)— owns the R8 texture, shelf packer, FreeType
 *                   rasterisation, and the full-texture reclaim that
 *                   runs when the packer exhausts the atlas image.
 *                   Decides "where do this glyph's pixels live?"
 *
 * The fetch entry point `txt_glyph_get()` is the only glue between the
 * two: hash lookup → raster on miss → put into cache → allocate atlas
 * rect → blit + upload. Allocation failure triggers a full atlas reset
 * that repacks every still-live entry into a fresh texture; entries
 * that no longer fit are dropped via `glyph_cache_invalidate()` so the
 * cache's live count stays consistent with what's actually on the
 * texture. This is the fix for the long-running "candidates lag after
 * a while" regression: previously the dead-entry count was never
 * decremented, the table falsely reported saturation, and every
 * `glyph_cache_put()` returned NULL — forcing a FreeType raster on
 * every visible glyph on every frame.
 */

#include "text_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

bool txt_atlas_init(flux_text *t) {
    t->atlas_pixels = calloc((size_t)ATLAS_W * ATLAS_H, 1);
    if (!t->atlas_pixels)
        return false;

    if (t->dev) {
        flux_image_desc d = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = ATLAS_W,
            .height = ATLAS_H,
            .format = FLUX_FORMAT_R8_UNORM,
            .initial_data = t->atlas_pixels,
        };
        if (flux_image_create(t->dev, &d, &t->atlas) != FLUX_OK) {
            txt_logf(t, FLUX_LOG_ERROR, "atlas image create failed");
            t->atlas = NULL;
        }
    }
    t->atlas_cursor_x = 0;
    t->atlas_cursor_y = 0;
    t->atlas_row_height = 0;
    t->atlas_dirty = false;
    return true;
}

void txt_atlas_destroy(flux_text *t) {
    if (t->atlas) {
        flux_image_release(t->atlas);
        t->atlas = NULL;
    }
    free(t->atlas_pixels);
    t->atlas_pixels = NULL;
}

/* ------------------------------------------------------------------ */
/*  Shelf allocation                                                   */
/* ------------------------------------------------------------------ */

/* Reserve a w x h cell plus a 1px gutter on the right/bottom. */
static bool atlas_alloc(flux_text *t, int w, int h, uint16_t *out_x, uint16_t *out_y) {
    if (w <= 0 || h <= 0) {
        *out_x = 0;
        *out_y = 0;
        return true;
    }

    uint32_t aw = (uint32_t)w + ATLAS_PAD;
    uint32_t ah = (uint32_t)h + ATLAS_PAD;
    if (aw > ATLAS_W || ah > ATLAS_H)
        return false;

    uint32_t cx = t->atlas_cursor_x;
    uint32_t cy = t->atlas_cursor_y;
    uint32_t rh = t->atlas_row_height;

    if (cx + aw > ATLAS_W) {
        cx = 0;
        cy += rh;
        rh = 0;
    }

    if (cy + ah > ATLAS_H)
        return false;

    *out_x = (uint16_t)cx;
    *out_y = (uint16_t)cy;
    cx += aw;
    if (ah > rh)
        rh = ah;

    t->atlas_cursor_x = cx;
    t->atlas_cursor_y = cy;
    t->atlas_row_height = rh;
    return true;
}

static void atlas_upload_rect(flux_text *t, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!t->atlas || !t->dev)
        return;

    size_t row_bytes = (size_t)w;
    size_t upload_bytes = row_bytes * h;

    if (row_bytes == (size_t)ATLAS_W && x == 0) {
        (void)flux_image_update_region(t->atlas, x, y, w, h,
                                       t->atlas_pixels + (size_t)y * ATLAS_W + x, upload_bytes);
    } else {
        uint8_t *tmp = malloc(upload_bytes);
        if (!tmp)
            return;
        for (uint32_t row = 0; row < h; row++) {
            memcpy(tmp + row * row_bytes, t->atlas_pixels + (size_t)(y + row) * ATLAS_W + x,
                   row_bytes);
        }
        (void)flux_image_update_region(t->atlas, x, y, w, h, tmp, upload_bytes);
        free(tmp);
    }
}

/* ------------------------------------------------------------------ */
/*  Rasterisation                                                      */
/* ------------------------------------------------------------------ */

/* Load + render `gid` into f->face->glyph at the face's current pixel size,
 * baking the requested horizontal subpixel phase into outline glyphs. */
static bool raster_glyph(txt_face *f, uint32_t gid, uint8_t phase) {
    if (FT_Load_Glyph(f->face, gid, FT_LOAD_DEFAULT) != 0)
        return false;

    FT_GlyphSlot g = f->face->glyph;
    if (phase && g->format == FT_GLYPH_FORMAT_OUTLINE) {
        /* phase/PHASES of a pixel, in 26.6 fixed point. */
        FT_Pos dx = (FT_Pos)phase * 64 / TXT_SUBPIXEL_PHASES;
        FT_Outline_Translate(&g->outline, dx, 0);
    }
    return FT_Render_Glyph(g, FT_RENDER_MODE_NORMAL) == 0;
}

static void blit_glyph(flux_text *t, glyph_entry *e, FT_GlyphSlot g) {
    for (uint32_t row = 0; row < g->bitmap.rows; row++) {
        memcpy(t->atlas_pixels + (size_t)(e->atlas_y + row) * ATLAS_W + e->atlas_x,
               g->bitmap.buffer + (ptrdiff_t)row * g->bitmap.pitch, g->bitmap.width);
    }
}

/* ------------------------------------------------------------------ */
/*  Atlas clear (O(1) full reclaim)                                    */
/* ------------------------------------------------------------------ */

/* Clear the atlas when the shelf packer exhausts the texture.
 *
 * The previous implementation (atlas_reset) re-rasterised every live
 * cache entry via FreeType and re-packed them into a fresh texture.
 * With ~4000+ CJK glyphs at HiDPI scale that cost 40-55 ms per reset,
 * and because a full atlas leaves no room even after repacking, every
 * additional new glyph triggered another reset — 10 resets per frame,
 * ~470 ms total, watchdog kill.
 *
 * This O(1) clear simply resets the pack cursor and invalidates every
 * cache entry. The GPU texture keeps its stale pixels, but they are
 * never sampled again: cleared entries have no atlas positions, and
 * each new allocation is rasterised + blitted + flushed to the GPU
 * before the quad referencing it is drawn. The cost shifts from
 * "re-rasterise N cached glyphs now" to "re-rasterise ~40 visible
 * glyphs on the next frame" — a 100× reduction at typical cache sizes.
 *
 * The GPU image is NOT recreated (no vkDestroy/vkCreate, no staging
 * upload of zeros). The atlas_pixels CPU buffer is not zeroed either;
 * new allocations overwrite the regions they use. */
static void atlas_clear(flux_text *t) {
    t->atlas_clears++;
    t->atlas_cursor_x = 0;
    t->atlas_cursor_y = 0;
    t->atlas_row_height = 0;
    glyph_cache_clear(t->cache);
    t->atlas_dirty = false;
}

/* ------------------------------------------------------------------ */
/*  Dirty-box batched upload                                           */
/*                                                                    */
/*  Replaces the per-glyph submit-and-wait that caused one GPU        */
/*  pipeline stall per cache miss (vkQueueSubmit2 + vkWaitForFences   */
/*  inside flux_vk_upload_to_image). Cache-miss blits now expand a    */
/*  bounding box; txt_atlas_flush uploads the box in a single         */
/*  flux_image_update_region before the quads are drawn.              */
/* ------------------------------------------------------------------ */

void txt_atlas_mark_dirty(flux_text *t, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (w == 0 || h == 0)
        return;
    uint16_t x1 = x + w;
    uint16_t y1 = y + h;
    if (!t->atlas_dirty) {
        t->atlas_dirty_x0 = x;
        t->atlas_dirty_y0 = y;
        t->atlas_dirty_x1 = x1;
        t->atlas_dirty_y1 = y1;
        t->atlas_dirty = true;
    } else {
        if (x < t->atlas_dirty_x0)
            t->atlas_dirty_x0 = x;
        if (y < t->atlas_dirty_y0)
            t->atlas_dirty_y0 = y;
        if (x1 > t->atlas_dirty_x1)
            t->atlas_dirty_x1 = x1;
        if (y1 > t->atlas_dirty_y1)
            t->atlas_dirty_y1 = y1;
    }
}

void txt_atlas_flush(flux_text *t) {
    if (!t->atlas_dirty)
        return;
    uint32_t w = (uint32_t)t->atlas_dirty_x1 - t->atlas_dirty_x0;
    uint32_t h = (uint32_t)t->atlas_dirty_y1 - t->atlas_dirty_y0;
    atlas_upload_rect(t, t->atlas_dirty_x0, t->atlas_dirty_y0, w, h);
    t->atlas_dirty = false;
}

/* ------------------------------------------------------------------ */
/*  Public: fetch (rasterising + caching on miss)                      */
/* ------------------------------------------------------------------ */

glyph_entry *txt_glyph_get(flux_text *t, int face_id, uint32_t gid, uint32_t rpx, uint8_t phase) {
    glyph_entry *hit = glyph_cache_lookup(t->cache, face_id, gid, rpx, phase);
    if (hit)
        return hit;

    int slot_idx = face_id >> 8;
    int face_idx = face_id & 0xFF;
    if (slot_idx < 0 || slot_idx >= FLUX_TEXT_NUM_FAMILIES * TXT_STYLE_SLOTS)
        return NULL;
    txt_face_slot *slot = &t->slots[slot_idx];
    if (face_idx < 0 || face_idx >= slot->count)
        return NULL;
    txt_face *f = &slot->faces[face_idx];
    if (!f->valid)
        return NULL;

    if (f->face_px != rpx && !txt_face_set_px(f, rpx))
        return NULL;
    if (!raster_glyph(f, gid, phase))
        return NULL;

    FT_GlyphSlot g = f->face->glyph;
    int gw = (int)g->bitmap.width;
    int gh = (int)g->bitmap.rows;

    glyph_entry *e = glyph_cache_put(t->cache, face_id, gid, rpx, phase);
    if (!e)
        return NULL;
    e->w = gw;
    e->h = gh;
    e->left = g->bitmap_left;
    e->top = g->bitmap_top;

    if (gw == 0 || gh == 0) {
        e->atlas_x = 0;
        e->atlas_y = 0;
        return e;
    }

    uint16_t ax, ay;
    if (!atlas_alloc(t, gw, gh, &ax, &ay)) {
        /* Atlas texture full. Clear everything (O(1) — no
         * re-rasterisation) and retry on the empty atlas. The clear
         * invalidated the entry we just put, so re-add it. */
        atlas_clear(t);
        e = glyph_cache_put(t->cache, face_id, gid, rpx, phase);
        if (!e)
            return NULL;
        e->w = gw;
        e->h = gh;
        e->left = g->bitmap_left;
        e->top = g->bitmap_top;
        if (!atlas_alloc(t, gw, gh, &ax, &ay))
            return NULL; /* glyph larger than the entire atlas */
    }

    e->atlas_x = ax;
    e->atlas_y = ay;
    blit_glyph(t, e, g);
    txt_atlas_mark_dirty(t, ax, ay, (uint16_t)gw, (uint16_t)gh);
    return e;
}
