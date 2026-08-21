/* glyph_cache.h — open-addressing glyph hash table with bounded LRU
 * eviction.
 *
 * Responsibility split (see also atlas.c):
 *
 *   glyph_cache — "which (face, gid, rpx, phase) tuples do we know?"
 *                  Owns the open-addressing table, the live-count, and
 *                  the eviction policy. Knows nothing about textures,
 *                  FreeType, or the GPU.
 *   atlas.c     — "where do this glyph's coverage pixels live?"
 *                  Owns the R8 texture, the shelf packer, the upload
 *                  path, and the full-repack reclaim. Calls intom
 *                  glyph_cache when an entry must be dropped.
 *
 * The two previous bugs that motivated this split:
 *
 *   1. Dead entries were marked `used = false` inside atlas.c's
 *      repack path but the live count was never decremented. Once the
 *      overstated count pushed the load factor over the grow trigger
 *      at the table cap, every subsequent insert returned NULL — so
 *      every visible glyph was re-rasterised via FreeType on every
 *      frame. Restart cleared the count; the lag returned over hours.
 *
 *   2. There was no eviction policy at all. Even with correct
 *      bookkeeping, a long CJK session accumulates more distinct
 *      (glyph, subpixel-phase) tuples than the hard cap, so the cache
 *      saturates permanently. The only recovery was process restart.
 *
 * This module fixes both: the count is maintained centrally by the
 * cache's own invalidate() path, and put() at the cap evicts the
 * least-recently-used entry instead of failing.
 *
 * A third, subtler failure mode is designed against explicitly: at
 * saturation every eviction mints a tombstone, so tombstone hygiene
 * must not depend on the growth trigger (which always fires at
 * saturation and would otherwise starve the sweep — see put()).
 *
 * Only compiled when FLUX_TEXT_HAVE_FTHB is defined; consumers include
 * it via text_internal.h.
 */

#ifndef FLUX_GLYPH_CACHE_H
#define FLUX_GLYPH_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Entry                                                              */
/* ------------------------------------------------------------------ */

/* One cached glyph image. Keyed by (face_id, gid, rpx, phase): the same
 * outline at a different horizontal subpixel phase is a distinct entry
 * so fractional pen positions stay crisp without snapping.
 *
 * `last_used` and `lru_prev`/`lru_next` are private to the cache's LRU
 * policy (a doubly-linked ring through slot indices, MRU first);
 * callers should treat them as read-only.
 *
 * Slot lifecycle (tri-state, encoded by the occupied/live pair):
 *
 *   occupied=false, live=false  — empty (never written)
 *   occupied=true,  live=true   — holds a valid entry
 *   occupied=true,  live=false  — tombstone (was live, now invalidated)
 *
 * Open-addressing lookups must stop only at empty slots; tombstones
 * are part of a probe chain and must be skipped, not treated as
 * chain-terminating. Earlier versions of this cache used a single
 * `used` flag for both states, which silently broke probe chains on
 * invalidate() — entries past the tombstone became unreachable. */
typedef struct glyph_entry {
    bool occupied;
    bool live;
    int face_id;
    uint32_t gid;
    uint32_t rpx;
    uint8_t phase;
    /* Which atlas page the coverage pixels live on (multi-page atlas).
     * The CPU-canvas path always uses page 0's shared host buffer. */
    uint8_t atlas_page;
    uint16_t atlas_x, atlas_y;
    int w, h;
    int left, top;
    uint64_t last_used;          /* LRU tick; bumped on lookup hit */
    uint32_t lru_prev, lru_next; /* intrusive ring; UINT32_MAX = none */
} glyph_entry;

/* ------------------------------------------------------------------ */
/*  Cache                                                              */
/* ------------------------------------------------------------------ */

typedef struct glyph_cache glyph_cache;

/* Lifecycle. `init_cap` is rounded down to a power of two no smaller
 * than 16 and no larger than `max_cap`. `max_cap` is the hard ceiling
 * at which put() starts evicting; it must be a power of two.
 *
 * Returns NULL on allocation failure or invalid arguments. */
glyph_cache *glyph_cache_new(uint32_t init_cap, uint32_t max_cap);
void glyph_cache_destroy(glyph_cache *c);

/* Lookup. Returns NULL on miss. On hit, bumps the entry's LRU tick so
 * frequently-rendered glyphs survive eviction. */
glyph_entry *glyph_cache_lookup(glyph_cache *c, int face_id, uint32_t gid, uint32_t rpx,
                                uint8_t phase);

/* Insert a fresh entry. The returned entry has its key fields
 * (face_id/gid/rpx/phase) populated and all other fields zeroed; the
 * caller is expected to rasterise + populate atlas_x/y/w/h/left/top
 * before the next lookup that might evict it.
 *
 * Capacity policy — two independent rules, evaluated in this order:
 *   - working set: live entries are capped at max_cap/2; a put that
 *     would exceed the cap evicts the LRU tail in O(1);
 *   - occupancy: (live+tombstones) past 50 % of cap grows the table
 *     while cap < max_cap, and tombstones alone past 25 % of cap
 *     trigger a same-cap rehash — independently of the first rule, so
 *     the sweep still runs at saturation where evictions mint
 *     tombstones fastest.
 *
 * Never returns NULL when any entry is evictable, which is always true
 * for a non-empty cache. Returns NULL only on unrecoverable allocation
 * failure. */
glyph_entry *glyph_cache_put(glyph_cache *c, int face_id, uint32_t gid, uint32_t rpx,
                             uint8_t phase);

/* Mark a live entry dead and decrement the live count. No-op on an
 * already-dead entry. Safe to call from inside glyph_cache_visit(). */
void glyph_cache_invalidate(glyph_cache *c, glyph_entry *e);

/* Mark every entry dead; live count resets to 0. Does not free the
 * table itself. */
void glyph_cache_clear(glyph_cache *c);

/* ------------------------------------------------------------------ */
/*  Stats + iteration (for atlas reclaim and diagnostics)             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t cap;           /* current table capacity (power of two)   */
    uint32_t count;         /* live entries                            */
    uint32_t max_cap;       /* hard ceiling before eviction kicks in   */
    uint64_t hits;          /* lookup() calls that found an entry      */
    uint64_t misses;        /* lookup() calls that did not             */
    uint64_t evictions;     /* entries dropped by put() at max_cap     */
    uint64_t invalidations; /* entries dropped by invalidate()/clear() */
    uint64_t grows;         /* table doublings                         */
} glyph_cache_stats;

void glyph_cache_get_stats(const glyph_cache *c, glyph_cache_stats *out);

/* Visit every live entry in arbitrary order. The visitor returns false
 * to stop early. Entries may be invalidated during iteration; iteration
 * skips any entry that is dead at the time it is examined. */
typedef bool (*glyph_cache_visit_fn)(glyph_entry *e, void *ctx);
void glyph_cache_visit(glyph_cache *c, glyph_cache_visit_fn visit, void *ctx);

#endif /* FLUX_GLYPH_CACHE_H */
