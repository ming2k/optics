/*
 * prism material leak lane (ADR-0130, ADR-0134).
 *
 * Before prism filters were counted, a leaked filter was reported as
 * `kind=vk_image owner=flux/core` — the wrong library. This lane asserts the
 * opposite: after creating and releasing each material filter, its own row is
 * back to exactly where it started *and* the flux rows it borrows are too.
 *
 * Each material gets a warm-up iteration first. That is not a tolerance: the
 * first Mica filter legitimately populates the device's pipeline cache and
 * slab, and counting that one-time growth as a leak would force a slack
 * constant back in. The assertion is exact between two identical iterations.
 */
#include "../../flux/test_helpers.h"

#include <flux/observe.h>
#include <prism/prism.h>

#include <string.h>

#define OWNER "prism/material"
#define SCOPE FLUX_SCOPE_DEVICE

static flux_observe_counters row(flux_device *d, const char *kind) {
    flux_observe_counters c = {};
    if (flux_observe_read_counters(d, SCOPE, kind, OWNER, &c) != FLUX_OK)
        c = (flux_observe_counters){};
    return c;
}

static flux_observe_counters flux_row(flux_device *d, const char *kind) {
    flux_observe_counters c = {};
    if (flux_observe_read_counters(d, SCOPE, kind, "flux/core", &c) != FLUX_OK)
        c = (flux_observe_counters){};
    return c;
}

static void expect_back(flux_device *d, const char *kind, const char *what,
                        const flux_observe_counters *before) {
    flux_observe_counters after = row(d, kind);
    if (after.live != before->live) {
        fprintf(stderr, "FAIL %s: %s leaked in prism's own row (live %llu -> %llu)\n", __FILE__,
                what, (unsigned long long)before->live, (unsigned long long)after.live);
        g_test_failed++;
    }
    g_test_count++;
}

typedef flux_result (*create_fn)(flux_device *, void **);
typedef void (*release_fn)(void *);

static void lane(flux_device *d, const char *kind, const char *what, create_fn create,
                 release_fn release) {
    /* Warm-up: absorb the one-time pipeline-cache and slab growth. */
    void *warm = nullptr;
    if (create(d, &warm) != FLUX_OK) {
        fprintf(stderr, "info: %s unavailable on this device; lane skipped\n", what);
        return;
    }
    release(warm);
    flux_device_wait_idle(d);

    flux_observe_counters before = row(d, kind);
    flux_observe_counters img_before = flux_row(d, "vk_image");
    flux_observe_counters pipe_before = flux_row(d, "vk_pipeline");

    void *obj = nullptr;
    EXPECT(create(d, &obj) == FLUX_OK);
    flux_observe_counters during = row(d, kind);
    EXPECT(during.live == before.live + 1);

    release(obj);
    flux_device_wait_idle(d);

    expect_back(d, kind, what, &before);
    /* And it must not have left flux-owned objects behind either: that is the
     * half that was previously the only half visible. */

    flux_observe_counters img_after = flux_row(d, "vk_image");
    flux_observe_counters pipe_after = flux_row(d, "vk_pipeline");
    if (img_after.live != img_before.live) {
        fprintf(stderr, "FAIL %s: %s left vk_image live (%llu -> %llu)\n", __FILE__, what,
                (unsigned long long)img_before.live, (unsigned long long)img_after.live);
        g_test_failed++;
    }
    g_test_count++;
    if (pipe_after.live != pipe_before.live) {
        fprintf(stderr, "FAIL %s: %s left vk_pipeline live (%llu -> %llu)\n", __FILE__, what,
                (unsigned long long)pipe_before.live, (unsigned long long)pipe_after.live);
        g_test_failed++;
    }
    g_test_count++;
}

/* Adapters: the prism entry points take typed out-parameters. */
static flux_result make_mica(flux_device *d, void **out) {
    return prism_mica_filter_create(d, (prism_mica_filter **)out);
}
static void drop_mica(void *p) {
    prism_mica_filter_release(p);
}
static flux_result make_frosted(flux_device *d, void **out) {
    return prism_frosted_filter_create(d, (prism_frosted_filter **)out);
}
static void drop_frosted(void *p) {
    prism_frosted_filter_release(p);
}
static flux_result make_acrylic(flux_device *d, void **out) {
    return prism_acrylic_filter_create(d, (prism_acrylic_filter **)out);
}
static void drop_acrylic(void *p) {
    prism_acrylic_filter_release(p);
}
static flux_result make_glass(flux_device *d, void **out) {
    return prism_liquid_glass_filter_create(d, (prism_liquid_glass_filter **)out);
}
static void drop_glass(void *p) {
    prism_liquid_glass_filter_release(p);
}

/* Positive control: a detector that cannot fire on a known leak is decoration.
 * This deliberately leaks one filter and requires the ledger to report it under
 * prism's own owner — the exact attribution ADR-0134 exists to provide. */
static void lane_detects_a_real_prism_leak(flux_device *d) {
    void *leaked = nullptr;
    if (prism_mica_filter_create(d, (prism_mica_filter **)&leaked) != FLUX_OK)
        return;
    /* Not released on purpose. */

    flux_observe_entry rows[32] = {};
    flux_observe_response rep = flux_observe_report(d, &(flux_observe_report_opts){
                                                           .type = FLUX_TYPE_OBSERVE_REPORT_OPTS,
                                                           .entries = rows,
                                                           .capacity = 32,
                                                           .scope = SCOPE,
                                                       });
    EXPECT(rep.result == FLUX_OK);

    bool saw_prism_row = false;
    for (uint32_t i = 0; i < rep.count; ++i) {
        if (rows[i].owner && strcmp(rows[i].owner, OWNER) == 0) {
            saw_prism_row = true;
            EXPECT(rows[i].live >= 1);
            /* The whole point of ADR-0134: it must NOT be attributed to flux. */
            EXPECT(strcmp(rows[i].owner, "flux/core") != 0);
        }
    }
    EXPECT(saw_prism_row);

    prism_mica_filter_release((prism_mica_filter *)leaked);
    flux_device_wait_idle(d);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_prism_material_leak: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    lane(d, "filter_mica", "prism_mica_filter", make_mica, drop_mica);
    lane(d, "filter_frosted", "prism_frosted_filter", make_frosted, drop_frosted);
    lane(d, "filter_acrylic", "prism_acrylic_filter", make_acrylic, drop_acrylic);
    lane(d, "filter_liquid_glass", "prism_liquid_glass_filter", make_glass, drop_glass);
    lane_detects_a_real_prism_leak(d);

    flux_device_release(d);
    TEST_SUMMARY();
}
