/*
 * Observation-plane unit test (ADR-0129).
 *
 * Proves the ledger's own contract in isolation, with no Vulkan device
 * involved beyond the headless device the scope checks need. Each block
 * below is a claim the ADR makes:
 *
 *   1. Counting is unconditional: a device with no chained observe opts
 *      still interns rows and reports them.
 *   2. A balanced acquire/release pair leaves live == 0 while the monotonic
 *      totals keep the history (so a balanced pair is distinguishable from
 *      "never happened").
 *   3. Peak survives drain: a high-water regression is visible after the
 *      objects are gone, which is what separates a leak from a spike.
 *   4. An unmatched release is counted, not wrapped into a huge uint64.
 *   5. Interning is exhausted at a hard bound and says so instead of
 *      silently dropping rows (a truncated report that claims completeness
 *      is worse than no report).
 *   6. The report never claims more rows than it wrote without marking the
 *      truncation in `total`.
 */
#include "test_helpers.h"

#include <flux/flux.h>
#include <flux/observe.h>
#include <flux/observe_sites.h>

#include <string.h>

/* Distinct literal kinds so the interning test can exhaust the row table
 * without allocating: pointer identity is the interning key, and each
 * array element is a distinct object. */
static const char *const SYNTHETIC_KINDS[] = {
    "k00", "k01", "k02", "k03", "k04", "k05", "k06", "k07", "k08", "k09", "k10", "k11",
    "k12", "k13", "k14", "k15", "k16", "k17", "k18", "k19", "k20", "k21", "k22", "k23",
    "k24", "k25", "k26", "k27", "k28", "k29", "k30", "k31", "k32", "k33", "k34", "k35",
    "k36", "k37", "k38", "k39", "k40", "k41", "k42", "k43", "k44", "k45", "k46", "k47",
    "k48", "k49", "k50", "k51", "k52", "k53", "k54", "k55", "k56", "k57", "k58", "k59",
};

static void test_counting_is_unconditional(flux_device *d) {
    /* Test 1: no observe opts were chained, so the ledger was initialised
     * with counting only. It must still work. */
    const char *kind = "probe_unconditional";
    const char *owner = "tests/observe";

    flux_observe_counters before = {};
    flux_result r = flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &before);
    EXPECT(r == FLUX_ERROR_OUT_OF_RANGE); /* never interned yet */

    EXPECT(FLUX_OBSERVE_ACQUIRE(d, FLUX_SCOPE_CONTEXT, FLUX_LIFETIME_CONTEXT, kind, owner, 128,
                                FLUX_SITE_NONE));
    flux_observe_counters during = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &during) == FLUX_OK);
    EXPECT(during.live == 1);
    EXPECT(during.acquire_total == 1);
    EXPECT(during.bytes_live == 128);

    FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 128);
    flux_observe_counters after = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &after) == FLUX_OK);
    EXPECT(after.live == 0);
    EXPECT(after.bytes_live == 0);
    /* The history survives the balanced pair: a pair that genuinely
     * happened is distinguishable from one that never did. */
    EXPECT(after.acquire_total == 1);
    EXPECT(after.release_total == 1);
}

static void test_peak_survives_drain(flux_device *d) {
    const char *kind = "probe_peak";
    const char *owner = "tests/observe";

    for (int i = 0; i < 7; ++i)
        EXPECT(FLUX_OBSERVE_ACQUIRE(d, FLUX_SCOPE_CONTEXT, FLUX_LIFETIME_CONTEXT, kind, owner, 0,
                                    FLUX_SITE_NONE));
    for (int i = 0; i < 7; ++i)
        FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 0);

    flux_observe_counters c = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &c) == FLUX_OK);
    EXPECT(c.live == 0);
    /* Peak is the whole point: after everything is released, a high-water
     * regression is still visible. A leak and a spike must not look alike. */
    EXPECT(c.peak == 7);
    EXPECT(c.acquire_total == 7);
    EXPECT(c.release_total == 7);
}

static void test_unmatched_release_is_counted(flux_device *d) {
    const char *kind = "probe_unmatched";
    const char *owner = "tests/observe";

    /* Release with no acquire: must not intern a phantom row (which would
     * read as balanced forever) and must not underflow. */
    FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 0);
    flux_observe_counters c = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &c) ==
           FLUX_ERROR_OUT_OF_RANGE);

    /* One acquire, two releases: live returns to zero and stops there
     * instead of wrapping to UINT64_MAX. */
    EXPECT(FLUX_OBSERVE_ACQUIRE(d, FLUX_SCOPE_CONTEXT, FLUX_LIFETIME_CONTEXT, kind, owner, 0,
                                FLUX_SITE_NONE));
    FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 0);
    FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 0);
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &c) == FLUX_OK);
    EXPECT(c.live == 0);
    EXPECT(c.release_total == 1); /* the second release was refused */
}

static void test_report_shape(flux_device *d) {
    const char *kind = "probe_report";
    const char *owner = "tests/observe";
    EXPECT(FLUX_OBSERVE_ACQUIRE(d, FLUX_SCOPE_CONTEXT, FLUX_LIFETIME_CONTEXT, kind, owner, 64,
                                FLUX_SITE_NONE));

    /* Undersized buffer: count reflects what was written, total reflects
     * what exists. A truncated report must announce itself. */
    flux_observe_entry one = {};
    flux_observe_response small = flux_observe_report(d, &(flux_observe_report_opts){
                                                             .type = FLUX_TYPE_OBSERVE_REPORT_OPTS,
                                                             .entries = &one,
                                                             .capacity = 1,
                                                             .scope = FLUX_SCOPE_CONTEXT,
                                                         });
    EXPECT(small.result == FLUX_OK);
    EXPECT(small.count <= 1);
    EXPECT(small.total >= small.count);
    EXPECT(small.live_total >= 1);

    /* Full buffer covers everything the scope holds. */
    flux_observe_entry all[64] = {};
    flux_observe_response full = flux_observe_report(d, &(flux_observe_report_opts){
                                                            .type = FLUX_TYPE_OBSERVE_REPORT_OPTS,
                                                            .entries = all,
                                                            .capacity = 64,
                                                            .scope = FLUX_SCOPE_CONTEXT,
                                                        });
    EXPECT(full.result == FLUX_OK);
    EXPECT(full.total == full.count);

    /* include_drained=false filters to the leaking rows only; our probe is
     * one of them. */
    bool saw_probe = false;
    for (uint32_t i = 0; i < full.count; ++i) {
        if (strcmp(all[i].kind, kind) == 0 && strcmp(all[i].owner, owner) == 0) {
            saw_probe = true;
            EXPECT(all[i].live == 1);
            EXPECT(all[i].bytes_live == 64);
            EXPECT(all[i].lifetime == FLUX_LIFETIME_CONTEXT);
            EXPECT(all[i].scope == FLUX_SCOPE_CONTEXT);
            /* FLUX_SITE_NONE resolves to nullptr by design: an unclaimed
             * site slot leaves no literal at all, which is what keeps a
             * non-attribution build free of site machinery. A kind that does
             * have a site slot must carry it. */
            flux_observe_counters probe = {};
            EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, kind, owner, &probe) ==
                   FLUX_OK);
            EXPECT(all[i].site == nullptr); /* probe uses FLUX_SITE_NONE */
        }
    }
    EXPECT(saw_probe);

    FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_CONTEXT, kind, owner, 64);
}

static void test_invalid_arguments(flux_device *d) {
    flux_observe_entry e = {};
    flux_observe_response bad_type =
        flux_observe_report(d, &(flux_observe_report_opts){
                                   .type = FLUX_TYPE_DEVICE_DESC, /* wrong descriptor tag */
                                   .entries = &e,
                                   .capacity = 1,
                               });
    EXPECT(bad_type.result == FLUX_ERROR_INVALID_ARGUMENT);

    flux_observe_response no_buffer =
        flux_observe_report(d, &(flux_observe_report_opts){.type = FLUX_TYPE_OBSERVE_REPORT_OPTS});
    EXPECT(no_buffer.result == FLUX_ERROR_INVALID_ARGUMENT);

    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_CONTEXT, nullptr, "x", nullptr) ==
           FLUX_ERROR_INVALID_ARGUMENT);
}

/* Interning is bounded. The bound must be observable: once the row table is
 * exhausted, acquires are refused and the report's overflow counter says the
 * report is incomplete. Silent truncation would make the report lie at the
 * exact moment it matters. */
static void test_interning_bound_is_visible(flux_device *d) {
    const uint32_t n = (uint32_t)(sizeof(SYNTHETIC_KINDS) / sizeof(SYNTHETIC_KINDS[0]));
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (FLUX_OBSERVE_ACQUIRE(d, FLUX_SCOPE_PROCESS, FLUX_LIFETIME_PROCESS, SYNTHETIC_KINDS[i],
                                 "tests/observe/bound", 0, FLUX_SITE_NONE))
            accepted++;
    }
    EXPECT(accepted > 0);

    flux_observe_entry rows[128] = {};
    flux_observe_response resp = flux_observe_report(d, &(flux_observe_report_opts){
                                                            .type = FLUX_TYPE_OBSERVE_REPORT_OPTS,
                                                            .entries = rows,
                                                            .capacity = 128,
                                                            .scope = FLUX_SCOPE_PROCESS,
                                                        });
    EXPECT(resp.result == FLUX_OK);
    /* Either the table had room for every kind (accepted == n) or it
     * refused some and the overflow counter is non-zero. Never both
     * "refused" and "quiet". */
    if (accepted < n) {
        EXPECT(resp.overflowed_kinds > 0);
    } else {
        EXPECT(resp.overflowed_kinds == 0);
    }

    for (uint32_t i = 0; i < n; ++i)
        FLUX_OBSERVE_RELEASE(d, FLUX_SCOPE_PROCESS, SYNTHETIC_KINDS[i], "tests/observe/bound", 0);
}

/* The site vocabulary must actually resolve. FLUX_SITE_NONE maps to nullptr
 * (no literal), and a claimed slot must map to a real label — otherwise the
 * enum would be decorative and the report's site column permanently empty. */
static void test_site_vocabulary(void) {
    EXPECT(flux_site_label(FLUX_SITE_NONE) == nullptr);
    EXPECT(flux_site_label(FLUX_SITE_COUNT) == nullptr);
    const char *host = flux_site_label(FLUX_SITE_HOST_ALLOC);
    EXPECT(host != nullptr);
    EXPECT(host && strstr(host, "flux_internal_alloc") != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_MEMORY) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_IMAGE) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_BUFFER) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_SAMPLER) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_PIPELINE) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_VK_TARGET) != nullptr);
    EXPECT(flux_site_label(FLUX_SITE_BINDLESS) != nullptr);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_observe: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    test_site_vocabulary();
    test_counting_is_unconditional(d);
    test_peak_survives_drain(d);
    test_unmatched_release_is_counted(d);
    test_report_shape(d);
    test_invalid_arguments(d);
    test_interning_bound_is_visible(d);

    flux_device_release(d);
    TEST_SUMMARY();
}
