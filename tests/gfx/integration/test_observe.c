/*
 * Observation-plane integration test (ADR-0129).
 *
 * Proves the ledger's contract against a live headless device: the rows are
 * device-scoped even though the assertions themselves are deterministic.
 * Each block below is a claim the ADR makes:
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
#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx.h>
#include <optics/gfx/observe.h>
#include <optics/gfx/observe_sites.h>

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

static void test_counting_is_unconditional(optics_gfx_device *d) {
    /* Test 1: no observe opts were chained, so the ledger was initialised
     * with counting only. It must still work. */
    const char *kind = "probe_unconditional";
    const char *owner = "tests/observe";

    optics_gfx_observe_counters before = {};
    optics_gfx_result r = optics_gfx_observe_read_counters(
        d, &(optics_gfx_observe_read_counters_opts){
               .scope = OPTICS_GFX_SCOPE_CONTEXT, .kind = kind, .owner = owner, .out = &before});
    OT_EXPECT(r == OPTICS_GFX_ERROR_OUT_OF_RANGE); /* never interned yet */

    OT_EXPECT(OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_CONTEXT, OPTICS_GFX_LIFETIME_CONTEXT,
                                         kind, owner, 128, OPTICS_GFX_SITE_NONE));
    optics_gfx_observe_counters during = {};
    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = kind,
                                                              .owner = owner,
                                                              .out = &during}) == OPTICS_GFX_OK);
    OT_EXPECT(during.live == 1);
    OT_EXPECT(during.acquire_total == 1);
    OT_EXPECT(during.bytes_live == 128);

    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 128);
    optics_gfx_observe_counters after = {};
    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = kind,
                                                              .owner = owner,
                                                              .out = &after}) == OPTICS_GFX_OK);
    OT_EXPECT(after.live == 0);
    OT_EXPECT(after.bytes_live == 0);
    /* The history survives the balanced pair: a pair that genuinely
     * happened is distinguishable from one that never did. */
    OT_EXPECT(after.acquire_total == 1);
    OT_EXPECT(after.release_total == 1);
}

static void test_peak_survives_drain(optics_gfx_device *d) {
    const char *kind = "probe_peak";
    const char *owner = "tests/observe";

    for (int i = 0; i < 7; ++i)
        OT_EXPECT(OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_CONTEXT,
                                             OPTICS_GFX_LIFETIME_CONTEXT, kind, owner, 0,
                                             OPTICS_GFX_SITE_NONE));
    for (int i = 0; i < 7; ++i)
        OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 0);

    optics_gfx_observe_counters c = {};
    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = kind,
                                                              .owner = owner,
                                                              .out = &c}) == OPTICS_GFX_OK);
    OT_EXPECT(c.live == 0);
    /* Peak is the whole point: after everything is released, a high-water
     * regression is still visible. A leak and a spike must not look alike. */
    OT_EXPECT(c.peak == 7);
    OT_EXPECT(c.acquire_total == 7);
    OT_EXPECT(c.release_total == 7);
}

static void test_unmatched_release_is_counted(optics_gfx_device *d) {
    const char *kind = "probe_unmatched";
    const char *owner = "tests/observe";

    /* Release with no acquire: must not intern a phantom row (which would
     * read as balanced forever) and must not underflow. */
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 0);
    optics_gfx_observe_counters c = {};
    OT_EXPECT(optics_gfx_observe_read_counters(d, &(optics_gfx_observe_read_counters_opts){
                                                      .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                      .kind = kind,
                                                      .owner = owner,
                                                      .out = &c}) == OPTICS_GFX_ERROR_OUT_OF_RANGE);

    /* One acquire, two releases: live returns to zero and stops there
     * instead of wrapping to UINT64_MAX. */
    OT_EXPECT(OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_CONTEXT, OPTICS_GFX_LIFETIME_CONTEXT,
                                         kind, owner, 0, OPTICS_GFX_SITE_NONE));
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 0);
    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 0);
    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = kind,
                                                              .owner = owner,
                                                              .out = &c}) == OPTICS_GFX_OK);
    OT_EXPECT(c.live == 0);
    OT_EXPECT(c.release_total == 1); /* the second release was refused */
}

static void test_report_shape(optics_gfx_device *d) {
    const char *kind = "probe_report";
    const char *owner = "tests/observe";
    OT_EXPECT(OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_CONTEXT, OPTICS_GFX_LIFETIME_CONTEXT,
                                         kind, owner, 64, OPTICS_GFX_SITE_NONE));

    /* Undersized buffer: count reflects what was written, total reflects
     * what exists. A truncated report must announce itself. */
    optics_gfx_observe_entry one = {};
    optics_gfx_observe_response small =
        optics_gfx_observe_report(d, &(optics_gfx_observe_report_opts){
                                         .type = OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS,
                                         .entries = &one,
                                         .capacity = 1,
                                         .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                     });
    OT_EXPECT(small.result == OPTICS_GFX_OK);
    OT_EXPECT(small.count <= 1);
    OT_EXPECT(small.total >= small.count);
    OT_EXPECT(small.live_total >= 1);

    /* Full buffer covers everything the scope holds. */
    optics_gfx_observe_entry all[64] = {};
    optics_gfx_observe_response full =
        optics_gfx_observe_report(d, &(optics_gfx_observe_report_opts){
                                         .type = OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS,
                                         .entries = all,
                                         .capacity = 64,
                                         .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                     });
    OT_EXPECT(full.result == OPTICS_GFX_OK);
    OT_EXPECT(full.total == full.count);

    /* include_drained=false filters to the leaking rows only; our probe is
     * one of them. */
    bool saw_probe = false;
    for (uint32_t i = 0; i < full.count; ++i) {
        if (strcmp(all[i].kind, kind) == 0 && strcmp(all[i].owner, owner) == 0) {
            saw_probe = true;
            OT_EXPECT(all[i].live == 1);
            OT_EXPECT(all[i].bytes_live == 64);
            OT_EXPECT(all[i].lifetime == OPTICS_GFX_LIFETIME_CONTEXT);
            OT_EXPECT(all[i].scope == OPTICS_GFX_SCOPE_CONTEXT);
            /* OPTICS_GFX_SITE_NONE resolves to nullptr by design: an unclaimed
             * site slot leaves no literal at all, which is what keeps a
             * non-attribution build free of site machinery. A kind that does
             * have a site slot must carry it. */
            optics_gfx_observe_counters probe = {};
            OT_EXPECT(optics_gfx_observe_read_counters(d, &(optics_gfx_observe_read_counters_opts){
                                                              .scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = kind,
                                                              .owner = owner,
                                                              .out = &probe}) == OPTICS_GFX_OK);
            OT_EXPECT(all[i].site == nullptr); /* probe uses OPTICS_GFX_SITE_NONE */
        }
    }
    OT_EXPECT(saw_probe);

    OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_CONTEXT, kind, owner, 64);
}

static void test_invalid_arguments(optics_gfx_device *d) {
    optics_gfx_observe_entry e = {};
    optics_gfx_observe_response bad_type = optics_gfx_observe_report(
        d, &(optics_gfx_observe_report_opts){
               .type = OPTICS_GFX_TYPE_DEVICE_DESC, /* wrong descriptor tag */
               .entries = &e,
               .capacity = 1,
           });
    OT_EXPECT(bad_type.result == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_observe_response no_buffer = optics_gfx_observe_report(
        d, &(optics_gfx_observe_report_opts){.type = OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS});
    OT_EXPECT(no_buffer.result == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    OT_EXPECT(optics_gfx_observe_read_counters(
                  d, &(optics_gfx_observe_read_counters_opts){.scope = OPTICS_GFX_SCOPE_CONTEXT,
                                                              .kind = nullptr,
                                                              .owner = "x",
                                                              .out = nullptr}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
}

/* Interning is bounded. The bound must be observable: once the row table is
 * exhausted, acquires are refused and the report's overflow counter says the
 * report is incomplete. Silent truncation would make the report lie at the
 * exact moment it matters. */
static void test_interning_bound_is_visible(optics_gfx_device *d) {
    const uint32_t n = (uint32_t)(sizeof(SYNTHETIC_KINDS) / sizeof(SYNTHETIC_KINDS[0]));
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (OPTICS_GFX_OBSERVE_ACQUIRE(d, OPTICS_GFX_SCOPE_PROCESS, OPTICS_GFX_LIFETIME_PROCESS,
                                       SYNTHETIC_KINDS[i], "tests/observe/bound", 0,
                                       OPTICS_GFX_SITE_NONE))
            accepted++;
    }
    OT_EXPECT(accepted > 0);

    optics_gfx_observe_entry rows[128] = {};
    optics_gfx_observe_response resp =
        optics_gfx_observe_report(d, &(optics_gfx_observe_report_opts){
                                         .type = OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS,
                                         .entries = rows,
                                         .capacity = 128,
                                         .scope = OPTICS_GFX_SCOPE_PROCESS,
                                     });
    OT_EXPECT(resp.result == OPTICS_GFX_OK);
    /* Either the table had room for every kind (accepted == n) or it
     * refused some and the overflow counter is non-zero. Never both
     * "refused" and "quiet". */
    if (accepted < n) {
        OT_EXPECT(resp.overflowed_kinds > 0);
    } else {
        OT_EXPECT(resp.overflowed_kinds == 0);
    }

    for (uint32_t i = 0; i < n; ++i)
        OPTICS_GFX_OBSERVE_RELEASE(d, OPTICS_GFX_SCOPE_PROCESS, SYNTHETIC_KINDS[i],
                                   "tests/observe/bound", 0);
}

/* The site vocabulary must actually resolve. OPTICS_GFX_SITE_NONE maps to nullptr
 * (no literal), and a claimed slot must map to a real label — otherwise the
 * enum would be decorative and the report's site column permanently empty. */
static void test_site_vocabulary(void) {
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_NONE) == nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_COUNT) == nullptr);
    const char *host = optics_gfx_site_label(OPTICS_GFX_SITE_HOST_ALLOC);
    OT_EXPECT(host != nullptr);
    OT_EXPECT(host && strstr(host, "optics_gfx_internal_alloc") != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_MEMORY) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_IMAGE) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_BUFFER) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_SAMPLER) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_PIPELINE) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_VK_TARGET) != nullptr);
    OT_EXPECT(optics_gfx_site_label(OPTICS_GFX_SITE_BINDLESS) != nullptr);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_observe: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    test_site_vocabulary();
    test_counting_is_unconditional(d);
    test_peak_survives_drain(d);
    test_unmatched_release_is_counted(d);
    test_report_shape(d);
    test_invalid_arguments(d);
    test_interning_bound_is_visible(d);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
