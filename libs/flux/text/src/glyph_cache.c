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
 * Drops all tombstones. Returns true on success; on failure the cache
 * is left untouched (caller can proceed against the old table). */
static bool rehash(glyph_cache *c, uint32_t new_cap) {
    glyph_entry *new_slots = calloc(new_cap, sizeof *new_slots);
    if (!new_slots)
        return false;

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

    free(c->slots);
    c->slots = new_slots;
    c->cap = new_cap;
    c->tomb_count = 0;
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

/* Linear scan for the live entry with the smallest last_used tick.
 *
 * This is O(cap) per eviction, which sounds bad but is fine in
 * practice: evictions only fire when the working set has actually
 * exceeded max_cap (typically 16 K). After that, the steady-state
 * eviction rate equals the rate at which genuinely new glyphs are
 * rasterised — a few per keystroke for CJK, not hundreds per frame.
 * At 16 K entries × ~64 B = 1 MB, the scan is cache-friendly and
 * takes a few tens of microseconds.
 *
 * We deliberately do not maintain an LRU linked list: that would add
 * two pointers to every glyph_entry (16 B × 16 K = 256 KB permanent
 * overhead) and complicate invalidate-during-iteration, which
 * atlas_reset relies on. */
static glyph_entry *find_lru_victim(glyph_cache *c) {
    glyph_entry *victim = NULL;
    uint64_t best = UINT64_MAX;
    for (uint32_t i = 0; i < c->cap; i++) {
        glyph_entry *e = &c->slots[i];
        if (!e->occupied || !e->live)
            continue;
        if (e->last_used < best) {
            best = e->last_used;
            victim = e;
        }
    }
    return victim;
}

/* Internal state-change for capacity-driven eviction. Identical to
 * glyph_cache_invalidate() but counted under `evictions` so the two
 * causes stay distinguishable in diagnostics. */
static void evict_for_capacity(glyph_cache *c, glyph_entry *e) {
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

    /* Admission control. The trigger counts live + tombstones because
     * tombstones inflate probe chains just like live entries.
     *
     *   below max_cap  → grow (double the table, drops tombstones)
     *   at max_cap     → evict LRU
     *
     * Independently, if tombstones alone exceed 25 % of cap, rehash
     * in place to collapse them. This matters for atlas_reset, which
     * can invalidate many entries in a single pass. */
    uint32_t used = c->live_count + c->tomb_count;
    if ((used << LOAD_FACTOR_SHIFT) >= c->cap) {
        if (c->cap < c->max_cap) {
            if (rehash(c, c->cap * 2u))
                c->stats.grows++;
        } else {
            glyph_entry *v = find_lru_victim(c);
            if (v)
                evict_for_capacity(c, v);
        }
    } else if ((c->tomb_count << TOMBSTONE_REHASH_SHIFT) >= c->cap && c->cap >= MIN_CAP * 2u) {
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
            return e;
        }
        if (e->live && key_eq(e, face_id, gid, rpx, phase)) {
            /* Duplicate put of a live key: refresh LRU and let the
             * caller overwrite metrics. */
            e->last_used = ++c->tick;
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
    }
    c->live_count = 0;
    c->tomb_count = 0;
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
        if (!visit(e, ctx))
            break;
    }
}
