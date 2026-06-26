/*
 * glyph_cache: open-addressing hash table + LRU eviction + accurate
 * live-count bookkeeping.
 *
 * This is a regression test for the bug that motivated the module
 * split: dead entries were marked `used = false` during atlas reclaim
 * without decrementing the live count, so once the overstated count
 * crossed the grow trigger at the table cap, every subsequent put()
 * returned NULL and every visible glyph was re-rasterised via FreeType
 * on every frame.
 *
 * The cache is a pure data structure with no Vulkan or FreeType
 * dependencies, so this test compiles glyph_cache.c directly (matching
 * the test_canvas_tess pattern) and runs on any host.
 */
#include "../../../libs/flux/text/src/glyph_cache.c" /* direct compile */
#include <stdint.h>
#include <stdio.h>

static int failed = 0;
static int count = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        count++;                                                                                   \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

/* Insert keys (1, 0, 0, 0)..(1, N-1, 0, 0) and look them back up. */
static void test_basic_insert_lookup(void) {
    glyph_cache *c = glyph_cache_new(16, 256);
    CHECK(c != NULL);

    for (uint32_t i = 0; i < 32; i++) {
        glyph_entry *e = glyph_cache_put(c, 1, i, 0, 0);
        CHECK(e != NULL);
        CHECK(e->gid == i);
    }

    glyph_cache_stats s;
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == 32);
    CHECK(s.evictions == 0);
    CHECK(s.misses == 0);
    CHECK(s.hits == 0);

    /* All 32 must be present; lookup bumps hit counter. */
    for (uint32_t i = 0; i < 32; i++) {
        glyph_entry *e = glyph_cache_lookup(c, 1, i, 0, 0);
        CHECK(e != NULL);
        CHECK(e->gid == i);
    }
    glyph_cache_get_stats(c, &s);
    CHECK(s.hits == 32);

    /* A miss returns NULL and bumps the miss counter. */
    CHECK(glyph_cache_lookup(c, 1, 9999, 0, 0) == NULL);
    glyph_cache_get_stats(c, &s);
    CHECK(s.misses == 1);

    glyph_cache_destroy(c);
}

/* Insert past max_cap: the LRU tail must be evicted, the live count
 * must stay bounded, and put() must never return NULL.
 *
 * Policy: 50 % load factor throughout (count*2 >= cap is the trigger).
 * Below max_cap the trigger grows the table; at max_cap the trigger
 * evicts LRU. So with init_cap == max_cap the cache saturates at
 * cap/2 entries and every insert past that evicts one. */
static void test_eviction_at_cap(void) {
    const uint32_t cap = 64; /* init_cap == max_cap */
    glyph_cache *c = glyph_cache_new(cap, cap);
    CHECK(c != NULL);

    /* Fill to the 50 % load trigger. No eviction yet. */
    for (uint32_t i = 0; i < cap / 2; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }
    glyph_cache_stats s;
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == cap / 2);
    CHECK(s.evictions == 0);

    /* Putting one more evicts the LRU (gid 0, never looked up). */
    glyph_entry *e = glyph_cache_put(c, 1, 10000, 0, 0);
    CHECK(e != NULL);
    CHECK(e->gid == 10000);
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == cap / 2);
    CHECK(s.evictions == 1);

    /* gid 0 was the LRU victim — it must be gone. */
    CHECK(glyph_cache_lookup(c, 1, 0, 0, 0) == NULL);

    /* gid 1..cap/2-1 and gid 10000 must all still be present. */
    for (uint32_t i = 1; i < cap / 2; i++) {
        glyph_entry *g = glyph_cache_lookup(c, 1, i, 0, 0);
        CHECK(g != NULL);
        CHECK(g->gid == i);
    }
    CHECK(glyph_cache_lookup(c, 1, 10000, 0, 0) != NULL);

    /* Inserting many more never fails; count stays bounded at cap/2. */
    for (uint32_t i = 10001; i < 10000u + cap * 4; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == cap / 2);
    CHECK(s.evictions > 1);

    glyph_cache_destroy(c);
}

/* LRU recency is updated on lookup, not just insertion. */
static void test_lru_recency_on_lookup(void) {
    const uint32_t cap = 16; /* init_cap == max_cap; fills to cap/2 = 8 */
    glyph_cache *c = glyph_cache_new(cap, cap);
    CHECK(c != NULL);

    /* Fill to the 50 % load trigger. */
    for (uint32_t i = 0; i < cap / 2; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }

    /* Touch gid 0 so it is now the MRU. */
    CHECK(glyph_cache_lookup(c, 1, 0, 0, 0) != NULL);

    /* Inserting one more evicts the LRU — which should now be gid 1,
     * not gid 0. */
    CHECK(glyph_cache_put(c, 1, cap, 0, 0) != NULL);

    CHECK(glyph_cache_lookup(c, 1, 0, 0, 0) != NULL); /* still alive */
    CHECK(glyph_cache_lookup(c, 1, 1, 0, 0) == NULL); /* evicted */

    glyph_cache_destroy(c);
}

/* The bookkeeping bug: invalidate() must decrement the count, so a
 * subsequent put() at the freshly-freed capacity does not return NULL.
 * Before the fix this is exactly what stalled the panel — invalidate
 * was open-coded with `used = false` and no count update, so the
 * table falsely reported permanent saturation. */
static void test_invalidate_decrements_count(void) {
    /* Use init_cap < max_cap so the table can grow past the first
     * load-factor trigger without evicting; that lets us fill it to
     * a known count and then exercise invalidate explicitly. */
    const uint32_t max = 64;
    glyph_cache *c = glyph_cache_new(16, max);
    CHECK(c != NULL);

    /* Insert 16 entries (fits comfortably under max_cap/2 = 32). */
    for (uint32_t i = 0; i < 16; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }

    /* Invalidate half of them directly. */
    uint32_t invalidated = 0;
    for (uint32_t i = 0; i < 16; i += 2) {
        glyph_entry *e = glyph_cache_lookup(c, 1, i, 0, 0);
        CHECK(e != NULL);
        glyph_cache_invalidate(c, e);
        invalidated++;
    }

    glyph_cache_stats s;
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == 16 - invalidated);
    CHECK(s.invalidations == invalidated);

    /* The cache must accept `invalidated` new puts before any further
     * eviction. */
    uint64_t evictions_before = s.evictions;
    for (uint32_t i = 0; i < invalidated; i++) {
        glyph_entry *e = glyph_cache_put(c, 1, 1000 + i, 0, 0);
        CHECK(e != NULL);
        CHECK(e->gid == 1000 + i);
    }
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == 16);
    CHECK(s.evictions == evictions_before); /* no eviction needed */

    glyph_cache_destroy(c);
}

/* clear() must reset the count to 0; tick is preserved (so post-clear
 * entries sort after pre-clear ones in LRU order). */
static void test_clear_resets_count(void) {
    /* init_cap < max_cap so a grow has happened by the time we clear;
     * verifies clear() does not depend on table size. */
    glyph_cache *c = glyph_cache_new(16, 256);
    CHECK(c != NULL);

    for (uint32_t i = 0; i < 24; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }
    glyph_cache_clear(c);

    glyph_cache_stats s;
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == 0);
    CHECK(s.invalidations == 24);

    /* Cache must be usable again immediately. */
    CHECK(glyph_cache_put(c, 2, 5, 0, 0) != NULL);
    CHECK(glyph_cache_lookup(c, 2, 5, 0, 0) != NULL);

    glyph_cache_destroy(c);
}

/* Visitor must skip dead entries and tolerate invalidate-during-iter.
 * Defined at file scope because C lacks closures; the visitor context
 * lives in a file-scope struct. */
typedef struct {
    glyph_cache *c;
    int visited;
    int invalidated;
} visit_ctx;
static visit_ctx g_visit_ctx;
static bool invalidate_even(glyph_entry *e, void *p) {
    (void)p;
    g_visit_ctx.visited++;
    if ((e->gid & 1u) == 0) {
        glyph_cache_invalidate(g_visit_ctx.c, e);
        g_visit_ctx.invalidated++;
    }
    return true;
}

static void test_visit_with_invalidate_even(void) {
    /* Use a wide max_cap so the table never saturates during this test
     * — we are checking iteration + invalidate, not eviction. */
    glyph_cache *c = glyph_cache_new(16, 256);
    CHECK(c != NULL);
    for (uint32_t i = 0; i < 16; i++) {
        CHECK(glyph_cache_put(c, 1, i, 0, 0) != NULL);
    }

    g_visit_ctx = (visit_ctx){c, 0, 0};
    glyph_cache_visit(c, invalidate_even, NULL);
    CHECK(g_visit_ctx.visited == 16);
    CHECK(g_visit_ctx.invalidated == 8);

    glyph_cache_stats s;
    glyph_cache_get_stats(c, &s);
    CHECK(s.count == 8);

    /* Odd gids survive; even ones are gone. */
    for (uint32_t i = 0; i < 16; i++) {
        glyph_entry *e = glyph_cache_lookup(c, 1, i, 0, 0);
        if (i & 1u)
            CHECK(e != NULL);
        else
            CHECK(e == NULL);
    }

    glyph_cache_destroy(c);
}

/* Invalid args are null-safe, never crash. */
static void test_null_safety(void) {
    CHECK(glyph_cache_new(0, 0) == NULL);   /* zero max_cap */
    CHECK(glyph_cache_new(16, 24) == NULL); /* non-power-of-two max */
    CHECK(glyph_cache_new(16, 0) == NULL);

    glyph_cache_destroy(NULL);
    glyph_cache_clear(NULL);
    glyph_cache_invalidate(NULL, NULL);

    glyph_cache_stats s;
    glyph_cache_get_stats(NULL, &s);
    CHECK(s.cap == 0 && s.count == 0);

    CHECK(glyph_cache_lookup(NULL, 0, 0, 0, 0) == NULL);
    CHECK(glyph_cache_put(NULL, 0, 0, 0, 0) == NULL);

    /* Negative init_cap clamps to MIN_CAP, then succeeds. */
    glyph_cache *c = glyph_cache_new(2, 16);
    CHECK(c != NULL);
    glyph_cache_destroy(c);
}

int main(void) {
    test_basic_insert_lookup();
    test_eviction_at_cap();
    test_lru_recency_on_lookup();
    test_invalidate_decrements_count();
    test_clear_resets_count();
    test_visit_with_invalidate_even();
    test_null_safety();

    fprintf(stderr, "%s: %d/%d passed\n", __FILE__, count - failed, count);
    return failed ? 1 : 0;
}
