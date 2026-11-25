/* test_store_model.c — model-based stress test of the ui retained store.
 *
 * Drives libs/ui/src/store/store.c (compiled into this binary) through
 * store_model.h, which checks every frame against a reference model and
 * the store's structural invariants. Deterministic: fixed seeds, so a
 * failure reproduces exactly.
 *
 * Scenarios:
 *   1. The v0.0.86 freeze: a 600-node spike, then 100 survivors held past
 *      the shrink dwell. The inverted shrink rehashed them into 64 slots
 *      and spun forever.
 *   2. Shrink band sweep: for every capacity from 128 to 16384, survivors
 *      just inside, in the middle of, and at the edge of the shrink band
 *      (cap/16, cap/8], held for two dwells.
 *   3. Gradual decline: a population that shrinks by a few nodes per
 *      frame, so shrinks fire at many different loads.
 *   4. Threshold wobble: load crossing 1/8 every half dwell never
 *      rehashes (the hysteresis restarts on each high-load frame).
 *   5. Grace-window re-entry: nodes that leave and come back within the
 *      grace window keep their node; one frame later they are reaped.
 *   6. Seeded random walk: spikes, collapses, plateaus, and churn for
 *      tens of thousands of frames.
 *
 * CPU only; no ui context beyond the store. */
#include "optics_test.h"
#include "store_model.h"

static uint64_t rng_state;
static uint32_t rng(void) {
    /* xorshift64* — tiny, deterministic, good enough for workload shape. */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rng_below(uint32_t n) {
    return n ? rng() % n : 0;
}

#define DWELL (OPTICS_UI_STORE_SHRINK_FRAMES + OPTICS_UI_LEAVE_GRACE_FRAMES + 2)

static void regression_v0_0_86(void) {
    sm_ctx *c = sm_create(64);
    sm_frame_range(c, 0, 600); /* grows to 1024 */
    OT_EXPECT(c->ui->store.cap == 1024);
    sm_frame_range(c, 0, 100);
    sm_hold(c, DWELL, 1);               /* the old code never returned from here */
    OT_EXPECT(c->ui->store.cap < 1024); /* it did shrink */
    OT_EXPECT(c->ui->store.count == 100);
    sm_destroy(c);
}

static void shrink_band_sweep(void) {
    for (uint32_t cap = 128; cap <= 16384; cap <<= 1) {
        const uint32_t survivors[] = {cap / 16 + 1, cap / 12, cap / 8};
        for (size_t k = 0; k < sizeof survivors / sizeof survivors[0]; k++) {
            sm_ctx *c = sm_create(64);
            /* Fill to just under the growth threshold of `cap`. */
            sm_frame_range(c, 0, cap * 3 / 4 - 1);
            OT_EXPECT(c->ui->store.cap == cap);
            sm_frame_range(c, 0, survivors[k]);
            sm_hold(c, 2 * DWELL, 1);
            OT_EXPECT(c->ui->store.count == survivors[k]);
            /* After a shrink the table sits at <= 1/4 load... */
            OT_EXPECT((uint64_t)c->ui->store.count * 4 <= c->ui->store.cap);
            /* ...and the population can regrow without trouble. */
            sm_frame_range(c, 0, cap);
            sm_hold(c, 4, 1);
            OT_EXPECT(c->ui->store.count == cap);
            sm_destroy(c);
        }
    }
}

static void gradual_decline(void) {
    sm_ctx *c = sm_create(64);
    uint32_t population = 6000;
    sm_frame_range(c, 0, population);
    while (population > 10) {
        population -= population / 200 + 1;
        sm_frame_range(c, 0, population);
    }
    sm_hold(c, 6 * DWELL, 1);
    OT_EXPECT(c->ui->store.cap <= 16 * 64);
    OT_EXPECT(c->shrinks > 0);
    OT_EXPECT(c->ui->store.cap < c->peak_cap);
    sm_destroy(c);
}

/* Load wobbling across the 1/8 shrink threshold must never rehash: the
 * dwell restarts on every high-load frame. */
static void threshold_wobble(void) {
    sm_ctx *c = sm_create(64);
    sm_frame_range(c, 0, 700); /* cap 1024; 1/8 = 128 */
    for (int cycle = 0; cycle < 12; cycle++) {
        sm_frame_range(c, 0, 100); /* low load ... */
        sm_hold(c, OPTICS_UI_STORE_SHRINK_FRAMES / 2, 1);
        sm_frame_range(c, 0, 200); /* ... interrupted before the dwell ends */
        sm_hold(c, OPTICS_UI_LEAVE_GRACE_FRAMES + 2, 1);
    }
    OT_EXPECT(c->shrinks == 0);
    OT_EXPECT(c->ui->store.cap == 1024);
    sm_destroy(c);
}

static void grace_reentry(void) {
    sm_ctx *c = sm_create(64);
    sm_frame_range(c, 0, 40);
    optics_ui_node *first = c->node[7];
    /* Leave for exactly the grace window, then return: same node. */
    for (uint32_t f = 0; f < OPTICS_UI_LEAVE_GRACE_FRAMES; f++)
        sm_frame_range(c, 0, 7);
    OT_EXPECT(optics_ui__store_find(c->ui, sm_id(7)) == first);
    sm_frame_range(c, 0, 40); /* model asserts pointer stability */
    /* Leave one frame longer than the grace window: reaped (asserted by
     * the model at the frame it expires). */
    for (uint32_t f = 0; f <= OPTICS_UI_LEAVE_GRACE_FRAMES; f++)
        sm_frame_range(c, 0, 7);
    OT_EXPECT(optics_ui__store_find(c->ui, sm_id(7)) == nullptr);
    sm_destroy(c);
}

static void random_walk(uint64_t seed, uint32_t frames) {
    rng_state = seed;
    sm_ctx *c = sm_create(64);
    uint32_t base = 0, population = 50;
    while (c->frames < frames) {
        switch (rng_below(8)) {
        case 0: /* spike: a long listing appears for a few frames */
            population = 200 + rng_below(8000);
            for (uint32_t f = rng_below(4) + 1; f; f--)
                sm_frame_range(c, base, population);
            break;
        case 1: /* collapse to a small view, held long enough to shrink */
            population = 1 + rng_below(600);
            sm_frame_range(c, base, population);
            sm_hold(c, rng_below(2 * DWELL), 1);
            break;
        case 2: /* scroll: the visible window slides over the key space */
            base = (base + rng_below(population + 1)) % (SM_MAX_KEYS / 2);
            sm_frame_range(c, base, population);
            break;
        case 3: /* sparse churn: random subset of the working set */
            for (uint32_t i = 0; i < population; i++)
                if (rng_below(4))
                    sm_touch(c, base + rng_below(population * 2 + 1));
            sm_end_frame(c);
            break;
        case 4: /* thin out: keep every k-th node for a while */
            sm_hold(c, rng_below(DWELL), 2 + rng_below(6));
            break;
        case 5: /* idle frames: everything leaves */
            for (uint32_t f = rng_below(3 * OPTICS_UI_LEAVE_GRACE_FRAMES); f; f--)
                sm_end_frame(c);
            break;
        default: /* steady frames */
            sm_frame_range(c, base, population);
            sm_hold(c, rng_below(20), 1);
            break;
        }
    }
    sm_destroy(c);
}

int main(void) {
    regression_v0_0_86();
    shrink_band_sweep();
    gradual_decline();
    threshold_wobble();
    grace_reentry();
    static const uint64_t seeds[] = {0x0123456789abcdefull, 0xfeedfacecafebeefull,
                                     0x9e3779b97f4a7c15ull, 42};
    for (size_t k = 0; k < sizeof seeds / sizeof seeds[0]; k++)
        random_walk(seeds[k], 15000);
    OT_EXPECT(true); /* every scenario returned: no hang, no model abort */
    return OT_REPORT();
}
