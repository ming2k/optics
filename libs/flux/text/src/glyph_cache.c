/* glyph_cache.c — open-addressing glyph hash table with bounded LRU
 * eviction. See glyph_cache.h for the responsibility split between
 * this module and atlas.c. */

#include "glyph_cache.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Tunables                                                           */
/* ------------------------------------------------------------------ */

/* Load factor at which the table doubles. 50 % is the conventional
 * sweet spot for open addressing with linear probing — keeps average
 * probe length near 1.5 while not wasting half the memory. The trigger
 * counts both live and tombstone slots because tombstones contribute
 * to probe-chain length just as live entries do. */
#define LOAD_FACTOR_SHIFT 1 /* (live+tomb) * 2 >= cap */

/* Lower bound on init_cap. Below this the overhead of grow doubling
 * dominates and the table spends its first inserts rehashing. */
#define MIN_CAP 16

/* Tombstone-watermark at which a put() rehashes the table in place to
 * evict dead slots even when no grow is needed. Prevents tombstone
 * accumulation from inflating probe chains on long-running sessions
 * where invalidations outpace grows. */
#define TOMBSTONE_REHASH_SHIFT 2 /* tombstones * 4 >= cap */

/* Working-set ceiling, as a fraction of cap: past max_cap/2 live
 * entries the table stops growing and starts evicting. Kept at the
 * 50 % load factor so probe lengths stay short even at saturation. */
#define LIVE_CAP_SHIFT 1

/* Sentinel index for the intrusive LRU list. Any real slot index is
 * < cap <= UINT32_MAX/2, so this can never collide. */
#define LRU_NONE UINT32_MAX

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

struct glyph_cache {
    glyph_entry *slots;
    uint32_t cap;
    uint32_t live_count; /* entries that hold a valid glyph     */
    uint32_t tomb_count; /* occupied-but-invalidated slots      */
    uint32_t max_cap;
    uint64_t tick; /* monotonic; bumped on every lookup hit */
    /* Intrusive LRU: doubly-linked ring through slot indices, most
     * recent at head. Eviction pops the tail in O(1) — the linear
     * victim scan this replaces was O(cap) per evicted glyph, paid on
     * the frame path once a CJK working set exceeded the cap (20 new
     * glyphs per frame = 20 scans of ~1 MB each). 8 B per entry on a
     * 16 K table = 128 KB host memory, ~0.4 % of the 4×16 MiB atlas
     * it protects. Indices (not pointers) survive rehash realloc. */
    uint32_t lru_head, lru_tail;
    glyph_cache_stats stats;
};

/* ------------------------------------------------------------------ */
/*  Hashing + probing                                                  */
/* ------------------------------------------------------------------ */

static inline uint32_t glyph_hash(int face_id, uint32_t gid, uint32_t rpx, uint8_t phase) {
    /* Same mixing constants as the previous inline hash — preserved so
     * an existing working set hashes to the same slots after the
     * refactor, easing before/after comparisons during review. */
    uint32_t h = (uint32_t)face_id * 2654435761u;
    h ^= gid * 2246822519u;
    h ^= rpx * 3266489917u;
    h ^= (uint32_t)phase * 668265263u;
    h ^= h >> 16;
    return h;
}

static inline bool key_eq(const glyph_entry *e, int face_id, uint32_t gid, uint32_t rpx,
                          uint8_t phase) {
    return e->face_id == face_id && e->gid == gid && e->rpx == rpx && e->phase == phase;
}

/* ------------------------------------------------------------------ */
/*  Intrusive LRU ring (slot indices)                                  */
/* ------------------------------------------------------------------ */

static void lru_unlink(glyph_cache *c, uint32_t i) {
    glyph_entry *e = &c->slots[i];
    if (e->lru_prev != LRU_NONE)
        c->slots[e->lru_prev].lru_next = e->lru_next;
    else if (c->lru_head == i)
        c->lru_head = e->lru_next;
    if (e->lru_next != LRU_NONE)
        c->slots[e->lru_next].lru_prev = e->lru_prev;
    else if (c->lru_tail == i)
        c->lru_tail = e->lru_prev;
    e->lru_prev = e->lru_next = LRU_NONE;
}

static void lru_push_front(glyph_cache *c, uint32_t i) {
    glyph_entry *e = &c->slots[i];
    e->lru_prev = LRU_NONE;
    e->lru_next = c->lru_head;
    if (c->lru_head != LRU_NONE)
        c->slots[c->lru_head].lru_prev = i;
    c->lru_head = i;
    if (c->lru_tail == LRU_NONE)
        c->lru_tail = i;
}

static inline void lru_touch(glyph_cache *c, uint32_t i) {
    if (c->lru_head == i)
        return; /* already MRU */
    lru_unlink(c, i);
    lru_push_front(c, i);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

static uint32_t round_pow2(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo)
        v = lo;
    if (v > hi)
        v = hi;
    /* Floor to the largest power of two <= v. */
    uint32_t p = 1;
    while (p * 2u <= v)
        p *= 2u;
    return p;
}

glyph_cache *glyph_cache_new(uint32_t init_cap, uint32_t max_cap) {
    /* max_cap must be a power of two (mask arithmetic relies on it). */
    if (max_cap == 0 || (max_cap & (max_cap - 1)) != 0)
        return NULL;

    uint32_t cap = round_pow2(init_cap, MIN_CAP, max_cap);

    glyph_cache *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;

    c->slots = calloc(cap, sizeof *c->slots);
    if (!c->slots) {
        free(c);
        return NULL;
    }
    c->cap = cap;
    c->max_cap = max_cap;
    c->live_count = 0;
    c->tomb_count = 0;
    c->tick = 0;
    c->lru_head = c->lru_tail = LRU_NONE;
    return c;
}

void glyph_cache_destroy(glyph_cache *c) {
    if (!c)
        return;
    free(c->slots);
    free(c);
}

/* ------------------------------------------------------------------ */
/*  Rehash (grow + tombstone sweep share this path)                    */
/* ------------------------------------------------------------------ */

/* Re-insert every live entry into a fresh, zeroed table of `new_cap`.
 * Drops all tombstones and rebuilds the LRU ring in the same relative
 * order (the old ring is walked oldest→newest and relinked). Returns
 * true on success; on failure the cache is left untouched (caller can
 * proceed against the old table). */
static bool rehash(glyph_cache *c, uint32_t new_cap) {
    glyph_entry *new_slots = calloc(new_cap, sizeof *new_slots);
    if (!new_slots)
        return false;

    /* Rebuild the LRU ring in the same pass. The old ring is walked
     * tail→head (oldest→newest); each live entry is located in the new
     * table by probing for its key and relinked from scratch, so the
     * relative recency order survives the move. Slots carry stale
     * lru_prev/lru_next after the copy, which is why the walk reads
     * `next` from the OLD slot array before relinking. */

    uint32_t new_mask = new_cap - 1u;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *old = &c->slots[i];
        if (!old->occupied || !old->live)
            continue;
        uint32_t idx = glyph_hash(old->face_id, old->gid, old->rpx, old->phase) & new_mask;
        for (;;) {
            if (!new_slots[idx].occupied) {
                new_slots[idx] = *old;
                break;
            }
            idx = (idx + 1u) & new_mask;
        }
    }

    /* Relink the ring in the new slots. Walk the OLD ring tail→head
     * (oldest→newest) and push each entry onto the FRONT of the new
     * ring, so the last processed (the newest) ends at the head:
     * head = MRU, tail = LRU, exactly as before the move. */
    glyph_entry *old_slots = c->slots;
    uint32_t old_tail = c->lru_tail;
    uint32_t remaining = c->live_count;
    uint32_t cursor = old_tail;
    uint32_t new_head = LRU_NONE, new_tail = LRU_NONE;
    while (remaining-- > 0 && cursor != LRU_NONE) {
        glyph_entry *old = &old_slots[cursor];
        /* Find where this entry landed in the new table. The key is
         * still in the old slot; probe the new table for it. */
        uint32_t idx = glyph_hash(old->face_id, old->gid, old->rpx, old->phase) & new_mask;
        while (new_slots[idx].occupied &&
               !key_eq(&new_slots[idx], old->face_id, old->gid, old->rpx, old->phase))
            idx = (idx + 1u) & new_mask;

        uint32_t next = old->lru_prev; /* toward head = newer */
        new_slots[idx].lru_prev = LRU_NONE;
        new_slots[idx].lru_next = new_head;
        if (new_head != LRU_NONE)
            new_slots[new_head].lru_prev = idx;
        else
            new_tail = idx; /* first pushed = oldest = tail */
        new_head = idx;
        cursor = next;
    }

    free(c->slots);
    c->slots = new_slots;
    c->cap = new_cap;
    c->tomb_count = 0;
    c->lru_head = new_head;
    c->lru_tail = new_tail;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Lookup                                                             */
/* ------------------------------------------------------------------ */

glyph_entry *glyph_cache_lookup(glyph_cache *c, int face_id, uint32_t gid, uint32_t rpx,
                                uint8_t phase) {
    if (!c)
        return NULL;

    uint32_t mask = c->cap - 1u;
    uint32_t idx = glyph_hash(face_id, gid, rpx, phase) & mask;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *e = &c->slots[idx];
        /* Only a truly-empty slot terminates the probe chain.
         * Tombstones (occupied && !live) must be skipped. */
        if (!e->occupied)
            break;
        if (e->live && key_eq(e, face_id, gid, rpx, phase)) {
            e->last_used = ++c->tick;
            lru_touch(c, idx);
            c->stats.hits++;
            return e;
        }
        idx = (idx + 1u) & mask;
    }
    c->stats.misses++;
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Eviction (LRU)                                                     */
/* ------------------------------------------------------------------ */

/* Internal state-change for capacity-driven eviction. Identical to
 * glyph_cache_invalidate() but counted under `evictions` so the two
 * causes stay distinguishable in diagnostics. */
static void evict_for_capacity(glyph_cache *c, glyph_entry *e) {
    lru_unlink(c, (uint32_t)(e - c->slots));
    e->live = false;
    c->live_count--;
    c->tomb_count++;
    c->stats.evictions++;
}

/* ------------------------------------------------------------------ */
/*  Insert                                                             */
/* ------------------------------------------------------------------ */

glyph_entry *glyph_cache_put(glyph_cache *c, int face_id, uint32_t gid, uint32_t rpx,
                             uint8_t phase) {
    if (!c)
        return NULL;

    /* Admission control — two INDEPENDENT rules, deliberately not
     * chained as else-if:
     *
     *   1. Working-set bound: live entries are what the rasteriser can
     *      still hit. Past max_cap/2 the table cannot usefully grow
     *      further (50 % load is the probe-length contract), so the
     *      LRU tail is evicted — O(1) now that recency is an intrusive
     *      ring. Growth below max_cap is driven by rule 2.
     *
     *   2. Occupancy hygiene: (live+tombstones) past 50 % of cap grows
     *      the table (below max_cap); tombstones alone past 25 % of
     *      cap collapse via a same-cap rehash.
     *
     * Rule 2's sweep used to hang off an else-if of rule 1's load
     * branch, which made it dead code precisely at saturation — live
     * pinned at cap/2 meant the load branch always fired, so the
     * tombstones minted by every eviction could never be swept, and
     * probe chains degraded toward a full-table walk per miss. */
    if (c->live_count >= (c->max_cap >> LIVE_CAP_SHIFT)) {
        if (c->lru_tail != LRU_NONE)
            evict_for_capacity(c, &c->slots[c->lru_tail]);
    }

    uint32_t used = c->live_count + c->tomb_count;
    if ((used << LOAD_FACTOR_SHIFT) >= c->cap && c->cap < c->max_cap) {
        if (rehash(c, c->cap * 2u))
            c->stats.grows++;
    }

    if ((c->tomb_count << TOMBSTONE_REHASH_SHIFT) >= c->cap && c->cap >= MIN_CAP * 2u) {
        /* Same cap, just sweep tombstones. */
        (void)rehash(c, c->cap);
    }

    /* Walk the probe chain. The first non-occupied OR tombstone slot
     * is our insertion target. */
    uint32_t mask = c->cap - 1u;
    uint32_t idx = glyph_hash(face_id, gid, rpx, phase) & mask;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *e = &c->slots[idx];
        if (!e->occupied) {
            /* Fresh empty slot. */
            memset(e, 0, sizeof *e);
            e->occupied = true;
            e->live = true;
            e->face_id = face_id;
            e->gid = gid;
            e->rpx = rpx;
            e->phase = phase;
            e->last_used = ++c->tick;
            c->live_count++;
            lru_push_front(c, idx);
            return e;
        }
        if (e->live && key_eq(e, face_id, gid, rpx, phase)) {
            /* Duplicate put of a live key: refresh LRU and let the
             * caller overwrite metrics. */
            e->last_used = ++c->tick;
            lru_touch(c, idx);
            return e;
        }
        if (!e->live) {
            /* Tombstone — reuse it. */
            bool was_same_key = key_eq(e, face_id, gid, rpx, phase);
            memset(e, 0, sizeof *e);
            e->occupied = true;
            e->live = true;
            e->face_id = face_id;
            e->gid = gid;
            e->rpx = rpx;
            e->phase = phase;
            e->last_used = ++c->tick;
            c->live_count++;
            if (!was_same_key)
                c->tomb_count--;
            lru_push_front(c, idx);
            return e;
        }
        idx = (idx + 1u) & mask;
    }
    return NULL; /* table completely full — unreachable: the trigger
                  * above always either grows, evicts, or rehashes
                  * before we walk a full chain. */
}

/* ------------------------------------------------------------------ */
/*  Invalidation                                                       */
/* ------------------------------------------------------------------ */

void glyph_cache_invalidate(glyph_cache *c, glyph_entry *e) {
    if (!c || !e || !e->occupied || !e->live)
        return;
    lru_unlink(c, (uint32_t)(e - c->slots));
    e->live = false;
    /* The key fields are left intact so a later put() of the same key
     * can short-circuit on the still-present tombstone (the `was_same_key`
     * branch in put() above leaves tomb_count unchanged in that case). */
    c->live_count--;
    c->tomb_count++;
    c->stats.invalidations++;
}

void glyph_cache_clear(glyph_cache *c) {
    if (!c)
        return;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *e = &c->slots[i];
        if (e->occupied && e->live)
            c->stats.invalidations++;
        e->occupied = false;
        e->live = false;
        e->lru_prev = e->lru_next = LRU_NONE;
    }
    c->live_count = 0;
    c->tomb_count = 0;
    c->lru_head = c->lru_tail = LRU_NONE;
    /* tick is monotonic across the cache's lifetime — not reset here,
     * so post-clear entries sort after pre-clear ones in LRU order. */
}

/* ------------------------------------------------------------------ */
/*  Stats + iteration                                                  */
/* ------------------------------------------------------------------ */

void glyph_cache_get_stats(const glyph_cache *c, glyph_cache_stats *out) {
    if (!out)
        return;
    if (!c) {
        memset(out, 0, sizeof *out);
        return;
    }
    *out = c->stats;
    out->cap = c->cap;
    out->count = c->live_count;
    out->max_cap = c->max_cap;
}

void glyph_cache_visit(glyph_cache *c, glyph_cache_visit_fn visit, void *ctx) {
    if (!c || !visit)
        return;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *e = &c->slots[i];
        if (!e->occupied || !e->live)
            continue;
        /* The visitor may invalidate `e` (atlas reclaim does). Keep the
         * next slot up front: unlink sets this slot's links to NONE,
         * but iteration is by table index, not through the ring, so no
         * iterator state is lost either way. */
        if (!visit(e, ctx))
            break;
    }
}
