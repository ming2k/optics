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

/* Create page `page` (its own R8 image) and make it current. Returns
 * false on allocation failure with no state changed. Page 0 carries the
 * initial_data upload; later pages start empty (their texels arrive via
 * the dirty-box path as glyphs are blitted). */
static bool atlas_page_create(flux_text *t, uint8_t page) {
    flux_image_desc d = {
        .type = FLUX_TYPE_IMAGE_DESC,
        .width = ATLAS_W,
        .height = ATLAS_H,
        .format = FLUX_FORMAT_R8_UNORM,
        .initial_data = (page == 0) ? t->atlas_pixels : NULL,
    };
    flux_image *img = NULL;
    if (flux_image_create(t->dev, &d, &img) != FLUX_OK || !img) {
        txt_logf(t, FLUX_LOG_ERROR, "atlas page %u create failed", (unsigned)page);
        return false;
    }
    t->atlas_pages[page] = img;
    if (page >= t->atlas_page_count)
        t->atlas_page_count = (uint8_t)(page + 1);
    t->atlas_page = page;
    t->atlas = img; /* compatibility alias */
    t->atlas_page_cursor_x[page] = 0;
    t->atlas_page_cursor_y[page] = 0;
    t->atlas_page_row_height[page] = 0;
    t->atlas_page_dirty[page] = false;
    /* Mirror into the legacy single-view fields so paths that still read
     * them (and the mark-dirty compatibility view) observe page 0 state
     * on first use. */
    t->atlas_cursor_x = 0;
    t->atlas_cursor_y = 0;
    t->atlas_row_height = 0;
    t->atlas_dirty = false;
    return true;
}

bool txt_atlas_init(flux_text *t) {
    t->atlas_pixels = calloc((size_t)ATLAS_W * ATLAS_H, 1);
    if (!t->atlas_pixels)
        return false;

    memset(t->atlas_pages, 0, sizeof(t->atlas_pages));
    t->atlas_page_count = 0;
    t->atlas_page = 0;
    t->atlas = NULL;
    if (t->dev && !atlas_page_create(t, 0)) {
        /* Measure-only degradation: no GPU pages; the host buffer still
         * serves the CPU canvas path. */
        t->atlas = NULL;
        t->atlas_page_count = 0;
    }
    return true;
}

void txt_atlas_destroy(flux_text *t) {
    for (uint8_t p = 0; p < TXT_ATLAS_MAX_PAGES; p++) {
        if (t->atlas_pages[p]) {
            flux_image_release(t->atlas_pages[p]);
            t->atlas_pages[p] = NULL;
        }
    }
    t->atlas = NULL;
    t->atlas_page_count = 0;
    t->atlas_page = 0;
    free(t->atlas_pixels);
    t->atlas_pixels = NULL;
}

/* ------------------------------------------------------------------ */
/*  Shelf allocation                                                   */
/* ------------------------------------------------------------------ */

/* Reserve a w x h cell plus a 1px gutter on the right/bottom on the
 * current page. Mirrors the pack state into the legacy single-view fields
 * so mark_dirty/flush observe the current page without every caller
 * caring about paging. */
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

    uint8_t page = t->atlas_page;
    uint32_t cx = t->atlas_page_cursor_x[page];
    uint32_t cy = t->atlas_page_cursor_y[page];
    uint32_t rh = t->atlas_page_row_height[page];

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

    t->atlas_page_cursor_x[page] = cx;
    t->atlas_page_cursor_y[page] = cy;
    t->atlas_page_row_height[page] = rh;
    t->atlas_cursor_x = cx;
    t->atlas_cursor_y = cy;
    t->atlas_row_height = rh;
    return true;
}

/* The current page is full. Open the next page when the cap allows;
 * otherwise report failure so the caller performs the legacy full
 * reclaim. Returns true when a fresh page became current. */
static bool atlas_advance_page(flux_text *t) {
    if (t->atlas_page_count >= TXT_ATLAS_MAX_PAGES)
        return false;
    /* GPU path only: the CPU canvas samples the single shared host
     * buffer, so extra pages have nothing to upload into. */
    if (!t->dev)
        return false;
    return atlas_page_create(t, t->atlas_page_count);
}

static void atlas_upload_rect(flux_text *t, uint8_t page, uint32_t x, uint32_t y, uint32_t w,
                              uint32_t h) {
    flux_image *img = (page < TXT_ATLAS_MAX_PAGES) ? t->atlas_pages[page] : NULL;
    if (!img || !t->dev)
        return;

    size_t row_bytes = (size_t)w;
    size_t upload_bytes = row_bytes * h;

    if (row_bytes == (size_t)ATLAS_W && x == 0) {
        (void)flux_image_update_region(img, x, y, w, h,
                                       t->atlas_pixels + (size_t)y * ATLAS_W + x, upload_bytes);
    } else {
        /* Tight-row staging. This sits on the hottest text path (every
         * atlas flush whose dirty box is narrower than the full texture),
         * so the scratch buffer is retained on the context and reused
         * across flushes rather than paying malloc/free per flush. */
        if (!t->atlas_upload_scratch || t->atlas_upload_scratch_cap < upload_bytes) {
            free(t->atlas_upload_scratch);
            t->atlas_upload_scratch = malloc(upload_bytes);
            if (!t->atlas_upload_scratch) {
                t->atlas_upload_scratch_cap = 0;
                return;
            }
            t->atlas_upload_scratch_cap = upload_bytes;
        }
        uint8_t *tmp = t->atlas_upload_scratch;
        for (uint32_t row = 0; row < h; row++) {
            memcpy(tmp + row * row_bytes, t->atlas_pixels + (size_t)(y + row) * ATLAS_W + x,
                   row_bytes);
        }
        (void)flux_image_update_region(img, x, y, w, h, tmp, upload_bytes);
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
 * cache entry. The cost shifts from "re-rasterise N cached glyphs now"
 * to "re-rasterise ~40 visible glyphs on the next frame" — a 100×
 * reduction at typical cache sizes.
 *
 * Invalidation channel: texels are rearranged freely from here on, so
 * anything that baked old atlas UVs must keep sampling the OLD contents.
 * That is two audiences:
 *   - quads already emitted this frame (their draw batches sit in a
 *     command buffer, or in a canvas display-list segment, carrying the
 *     old image's bindless handle), and
 *   - the CPU-side dirty box, which still names regions of the old image.
 * The clear therefore (a) flushes any pending dirty box to the old image
 * so every quad emitted so far sees complete texels, (b) swaps t->atlas
 * for a fresh GPU image, and (c) releases the old one — flux_image_release
 * parks it on the device retire queue (and recorded segments hold their
 * own retain), so old UVs stay self-consistent with old contents until
 * every referencing batch has retired. New allocations are rasterised +
 * blitted + flushed to the NEW image before the quads referencing them
 * are drawn; flux_text_draw detects the mid-run swap and switches images
 * at a batch boundary. atlas_pixels is not zeroed; new allocations
 * overwrite the regions they use. The CPU backend has no GPU image at
 * all — its stale-segment hazard is covered by the host-atlas generation
 * check (flux_glyph_run_host_atlas_desc). */
static void atlas_clear(flux_text *t) {
    t->atlas_clears++;
    /* Flush every page's pending dirty box first so quads already emitted
     * against any page sample finished texels (see the invalidation
     * contract below — it now spans every page). */
    txt_atlas_flush(t);

    /* The clear must invalidate every page's texels wholesale: cache
     * entries carry (page, x, y) and any page's contents may be
     * rearranged by subsequent packing. Retire every page image, then
     * recreate only page 0. flux_image_release parks each old image on
     * the device retire queue (recorded segments hold their own retain),
     * so old UVs stay self-consistent with old contents until every
     * referencing batch has retired. */
    for (uint8_t p = 0; p < t->atlas_page_count && p < TXT_ATLAS_MAX_PAGES; p++) {
        flux_image *old = t->atlas_pages[p];
        t->atlas_pages[p] = NULL;
        if (p > 0 && old)
            flux_image_release(old);
    }
    if (t->atlas_page_count > 0 && t->atlas_pages[0] == NULL && t->dev) {
        flux_image_desc d = {
            .type = FLUX_TYPE_IMAGE_DESC,
            .width = ATLAS_W,
            .height = ATLAS_H,
            .format = FLUX_FORMAT_R8_UNORM,
        };
        if (flux_image_create(t->dev, &d, &t->atlas_pages[0]) != FLUX_OK) {
            /* Allocation failure: keep no atlas image and accept the
             * degraded (blank glyph) hazard rather than reusing stale
             * texels under rearranged UVs. */
            t->atlas_pages[0] = NULL;
            txt_logf(t, FLUX_LOG_ERROR, "atlas image recreate failed; glyphs will re-rasterise");
        }
    }
    t->atlas_page_count = t->atlas_pages[0] ? 1 : 0;
    t->atlas_page = 0;
    t->atlas = t->atlas_pages[0];
    for (uint8_t p = 0; p < TXT_ATLAS_MAX_PAGES; p++) {
        t->atlas_page_cursor_x[p] = 0;
        t->atlas_page_cursor_y[p] = 0;
        t->atlas_page_row_height[p] = 0;
        t->atlas_page_dirty[p] = false;
    }
    t->atlas_cursor_x = 0;
    t->atlas_cursor_y = 0;
    t->atlas_row_height = 0;
    t->atlas_dirty = false;
    glyph_cache_clear(t->cache);
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
    uint8_t page = t->atlas_page;
    if (!t->atlas_page_dirty[page]) {
        t->atlas_page_dirty_x0[page] = x;
        t->atlas_page_dirty_y0[page] = y;
        t->atlas_page_dirty_x1[page] = x1;
        t->atlas_page_dirty_y1[page] = y1;
        t->atlas_page_dirty[page] = true;
    } else {
        if (x < t->atlas_page_dirty_x0[page])
            t->atlas_page_dirty_x0[page] = x;
        if (y < t->atlas_page_dirty_y0[page])
            t->atlas_page_dirty_y0[page] = y;
        if (x1 > t->atlas_page_dirty_x1[page])
            t->atlas_page_dirty_x1[page] = x1;
        if (y1 > t->atlas_page_dirty_y1[page])
            t->atlas_page_dirty_y1[page] = y1;
    }
    /* Compatibility view: the legacy single dirty box follows the current
     * page so external readers (stats) see something coherent. */
    t->atlas_dirty = t->atlas_page_dirty[page];
    t->atlas_dirty_x0 = t->atlas_page_dirty_x0[page];
    t->atlas_dirty_y0 = t->atlas_page_dirty_y0[page];
    t->atlas_dirty_x1 = t->atlas_page_dirty_x1[page];
    t->atlas_dirty_y1 = t->atlas_page_dirty_y1[page];
}

void txt_atlas_flush(flux_text *t) {
    for (uint8_t p = 0; p < t->atlas_page_count && p < TXT_ATLAS_MAX_PAGES; p++) {
        if (!t->atlas_page_dirty[p])
            continue;
        uint32_t w = (uint32_t)t->atlas_page_dirty_x1[p] - t->atlas_page_dirty_x0[p];
        uint32_t h = (uint32_t)t->atlas_page_dirty_y1[p] - t->atlas_page_dirty_y0[p];
        atlas_upload_rect(t, p, t->atlas_page_dirty_x0[p], t->atlas_page_dirty_y0[p], w, h);
        t->atlas_page_dirty[p] = false;
        if (p == t->atlas_page)
            t->atlas_dirty = false;
    }
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
    e->atlas_page = t->atlas_page;

    if (gw == 0 || gh == 0) {
        e->atlas_x = 0;
        e->atlas_y = 0;
        return e;
    }

    uint16_t ax, ay;
    if (!atlas_alloc(t, gw, gh, &ax, &ay)) {
        /* Current page full. Prefer opening a fresh page (keeps every
         * cached glyph alive) over the wholesale clear; fall back to the
         * clear only at the page cap. */
        if (atlas_advance_page(t) && atlas_alloc(t, gw, gh, &ax, &ay)) {
            e->atlas_page = t->atlas_page;
        } else {
            /* Page cap reached (or new-page allocation failed): legacy
             * O(1) full reclaim. The clear invalidated the entry we just
             * put, so re-add it. */
            atlas_clear(t);
            e = glyph_cache_put(t->cache, face_id, gid, rpx, phase);
            if (!e)
                return NULL;
            e->w = gw;
            e->h = gh;
            e->left = g->bitmap_left;
            e->top = g->bitmap_top;
            e->atlas_page = t->atlas_page;
            if (!atlas_alloc(t, gw, gh, &ax, &ay))
                return NULL; /* glyph larger than the entire atlas */
        }
    }

    e->atlas_x = ax;
    e->atlas_y = ay;
    blit_glyph(t, e, g);
    txt_atlas_mark_dirty(t, ax, ay, (uint16_t)gw, (uint16_t)gh);
    return e;
}
