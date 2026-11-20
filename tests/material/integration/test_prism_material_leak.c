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
#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx/observe.h>
#include <optics/material.h>

#include <string.h>

#define OWNER "prism/material"
#define SCOPE OPTICS_GFX_SCOPE_DEVICE

static optics_gfx_observe_counters row(optics_gfx_device *d, const char *kind) {
    optics_gfx_observe_counters c = {};
    if (optics_gfx_observe_read_counters(
            d, &(optics_gfx_observe_read_counters_opts){
                   .scope = SCOPE, .kind = kind, .owner = OWNER, .out = &c}) != OPTICS_GFX_OK)
        c = (optics_gfx_observe_counters){};
    return c;
}

static optics_gfx_observe_counters optics_gfx_row(optics_gfx_device *d, const char *kind) {
    optics_gfx_observe_counters c = {};
    if (optics_gfx_observe_read_counters(
            d, &(optics_gfx_observe_read_counters_opts){
                   .scope = SCOPE, .kind = kind, .owner = "flux/core", .out = &c}) != OPTICS_GFX_OK)
        c = (optics_gfx_observe_counters){};
    return c;
}

static void expect_back(optics_gfx_device *d, const char *kind, const char *what,
                        const optics_gfx_observe_counters *before) {
    optics_gfx_observe_counters after = row(d, kind);
    if (after.live != before->live) {
        fprintf(stderr, "FAIL %s: %s leaked in prism's own row (live %llu -> %llu)\n", __FILE__,
                what, (unsigned long long)before->live, (unsigned long long)after.live);
        OT_MANUAL_FAIL();
    }
    OT_MANUAL_CHECK();
}

typedef optics_gfx_result (*create_fn)(optics_gfx_device *, void **);
typedef void (*release_fn)(void *);

static void lane(optics_gfx_device *d, const char *kind, const char *what, create_fn create,
                 release_fn release) {
    /* Warm-up: absorb the one-time pipeline-cache and slab growth. */
    void *warm = nullptr;
    if (create(d, &warm) != OPTICS_GFX_OK) {
        fprintf(stderr, "info: %s unavailable on this device; lane skipped\n", what);
        return;
    }
    release(warm);
    optics_gfx_device_wait_idle(d);

    optics_gfx_observe_counters before = row(d, kind);
    optics_gfx_observe_counters img_before = optics_gfx_row(d, "vk_image");
    optics_gfx_observe_counters pipe_before = optics_gfx_row(d, "vk_pipeline");

    void *obj = nullptr;
    OT_EXPECT(create(d, &obj) == OPTICS_GFX_OK);
    optics_gfx_observe_counters during = row(d, kind);
    OT_EXPECT(during.live == before.live + 1);

    release(obj);
    optics_gfx_device_wait_idle(d);

    expect_back(d, kind, what, &before);
    /* And it must not have left flux-owned objects behind either: that is the
     * half that was previously the only half visible. */

    optics_gfx_observe_counters img_after = optics_gfx_row(d, "vk_image");
    optics_gfx_observe_counters pipe_after = optics_gfx_row(d, "vk_pipeline");
    if (img_after.live != img_before.live) {
        fprintf(stderr, "FAIL %s: %s left vk_image live (%llu -> %llu)\n", __FILE__, what,
                (unsigned long long)img_before.live, (unsigned long long)img_after.live);
        OT_MANUAL_FAIL();
    }
    OT_MANUAL_CHECK();
    if (pipe_after.live != pipe_before.live) {
        fprintf(stderr, "FAIL %s: %s left vk_pipeline live (%llu -> %llu)\n", __FILE__, what,
                (unsigned long long)pipe_before.live, (unsigned long long)pipe_after.live);
        OT_MANUAL_FAIL();
    }
    OT_MANUAL_CHECK();
}

/* Adapters: the prism entry points take typed out-parameters. */
static optics_gfx_result make_mica(optics_gfx_device *d, void **out) {
    return optics_material_mica_filter_create(d, (optics_material_mica_filter **)out);
}
static void drop_mica(void *p) {
    optics_material_mica_filter_release(p);
}
static optics_gfx_result make_frosted(optics_gfx_device *d, void **out) {
    return optics_material_frosted_filter_create(d, (optics_material_frosted_filter **)out);
}
static void drop_frosted(void *p) {
    optics_material_frosted_filter_release(p);
}
static optics_gfx_result make_acrylic(optics_gfx_device *d, void **out) {
    return optics_material_acrylic_filter_create(d, (optics_material_acrylic_filter **)out);
}
static void drop_acrylic(void *p) {
    optics_material_acrylic_filter_release(p);
}
static optics_gfx_result make_glass(optics_gfx_device *d, void **out) {
    return optics_material_liquid_glass_filter_create(d,
                                                      (optics_material_liquid_glass_filter **)out);
}
static void drop_glass(void *p) {
    optics_material_liquid_glass_filter_release(p);
}

/* Positive control: a detector that cannot fire on a known leak is decoration.
 * This deliberately leaks one filter and requires the ledger to report it under
 * prism's own owner — the exact attribution ADR-0134 exists to provide. */
static void lane_detects_a_real_prism_leak(optics_gfx_device *d) {
    void *leaked = nullptr;
    if (optics_material_mica_filter_create(d, (optics_material_mica_filter **)&leaked) !=
        OPTICS_GFX_OK)
        return;
    /* Not released on purpose. */

    optics_gfx_observe_entry rows[32] = {};
    optics_gfx_observe_response rep =
        optics_gfx_observe_report(d, &(optics_gfx_observe_report_opts){
                                         .type = OPTICS_GFX_TYPE_OBSERVE_REPORT_OPTS,
                                         .entries = rows,
                                         .capacity = 32,
                                         .scope = SCOPE,
                                     });
    OT_EXPECT(rep.result == OPTICS_GFX_OK);

    bool saw_prism_row = false;
    for (uint32_t i = 0; i < rep.count; ++i) {
        if (rows[i].owner && strcmp(rows[i].owner, OWNER) == 0) {
            saw_prism_row = true;
            OT_EXPECT(rows[i].live >= 1);
            /* The whole point of ADR-0134: it must NOT be attributed to flux. */
            OT_EXPECT(strcmp(rows[i].owner, "flux/core") != 0);
        }
    }
    OT_EXPECT(saw_prism_row);

    optics_material_mica_filter_release((optics_material_mica_filter *)leaked);
    optics_gfx_device_wait_idle(d);
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_prism_material_leak: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    lane(d, "filter_mica", "prism_mica_filter", make_mica, drop_mica);
    lane(d, "filter_frosted", "prism_frosted_filter", make_frosted, drop_frosted);
    lane(d, "filter_acrylic", "prism_acrylic_filter", make_acrylic, drop_acrylic);
    lane(d, "filter_liquid_glass", "prism_liquid_glass_filter", make_glass, drop_glass);
    lane_detects_a_real_prism_leak(d);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
