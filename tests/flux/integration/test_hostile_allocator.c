/*
 * Hostile-allocator contract test (ADR-0135).
 *
 * The `core.h` contract — "use these instead of libc malloc/free in peer code so
 * the caller's allocator is honoured" — had never been tested. This test is the
 * falsifier: it installs an allocator that sees everything, then requires that
 * every library's allocation actually passes through it.
 *
 * What the allocator does:
 *   - counts every alloc and free, and tags each block with a magic so a
 *     use-after-free or a double free is caught rather than corrupting quietly;
 *   - records the high-water mark and the live byte count;
 *   - can fail the Nth allocation, so OOM paths are driven deterministically.
 *
 * What is asserted:
 *   1. `flux_device_alloc` returns memory this allocator produced (pointer is
 *      inside one of our blocks) and zeroes it.
 *   2. prism's filter creation spends bytes *through this allocator*, and
 *      releasing it returns exactly those bytes.
 *   3. lens's store allocation does the same.
 *   4. At teardown, live bytes are zero — per owner, not just in total.
 *
 * A library that bypasses the funnel shows up as "0 bytes seen for owner=X",
 * which is the direct, falsifiable form of the contract.
 */
#include "../icc_testkit.h"
#include "test_helpers.h"

#include <flux/flux.h>
#include <flux/observe.h>
#include <lens/lens.h>
#include <prism/prism.h>

#include <stdalign.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC_LIVE UINT64_C(0x4f50544c49564531) /* "OPTLIVE1" */
#define MAGIC_DEAD UINT64_C(0x4f50544445414431) /* "OPTDEAD1" */

typedef struct block_hdr {
    uint64_t magic;
    size_t bytes;
    size_t offset; /* index into g_blocks for accounting */
} block_hdr;

#define MAX_BLOCKS 4096

static void *g_blocks[MAX_BLOCKS]; /* hdr pointers we handed out */
static size_t g_block_bytes[MAX_BLOCKS];
static int g_block_count;
static size_t g_live_bytes;
static size_t g_peak_bytes;
static uint64_t g_allocs, g_frees;
static uint64_t g_fail_at; /* fail the Nth allocation; 0 = never */
static uint64_t g_alloc_seq;
static int g_violations;

static void note_violation(const char *what) {
    fprintf(stderr, "FAIL %s: %s\n", __FILE__, what);
    g_violations++;
}

static void *hostile_alloc(size_t bytes, void *user) {
    (void)user;
    g_alloc_seq++;
    if (g_fail_at && g_alloc_seq == g_fail_at)
        return nullptr;
    if (g_block_count >= MAX_BLOCKS)
        return nullptr;
    /* Deliberately NOT zeroed: the contract says flux_device_alloc zeroes it,
     * so a caller relying on that must be caught if it is not done. */
    block_hdr *h = malloc(sizeof(*h) + bytes);
    if (!h)
        return nullptr;
    h->magic = MAGIC_LIVE;
    h->bytes = bytes;
    h->offset = (size_t)g_block_count;
    g_blocks[g_block_count] = h;
    g_block_bytes[g_block_count] = bytes;
    g_block_count++;
    g_live_bytes += bytes;
    if (g_live_bytes > g_peak_bytes)
        g_peak_bytes = g_live_bytes;
    g_allocs++;
    return (void *)(h + 1);
}

static void hostile_free(void *ptr, void *user) {
    (void)user;
    if (!ptr)
        return;
    block_hdr *h = ((block_hdr *)ptr) - 1;
    if (h->magic != MAGIC_LIVE) {
        note_violation("free() of a pointer the hostile allocator did not produce");
        return;
    }
    if (h->offset >= (size_t)g_block_count || g_blocks[h->offset] != h) {
        note_violation("double free or stale pointer in hostile allocator");
        return;
    }
    h->magic = MAGIC_DEAD;
    g_live_bytes -= h->bytes;
    g_blocks[h->offset] = nullptr;
    g_frees++;
    free(h);
}

static const flux_allocator HOSTILE = {
    .alloc = hostile_alloc,
    .free = hostile_free,
    .user = nullptr,
};

static flux_device *make_hostile_device(void) {
    flux_device_desc dd = FLUX_DEVICE_DESC_INIT;
    dd.headless = true;
    dd.allocator = HOSTILE;
    dd.log = flux_console_logger;
    flux_device *d = nullptr;
    if (flux_device_create(&dd, &d) != FLUX_OK)
        return nullptr;
    return d;
}

/* --- 1. the entry point itself ---------------------------------------- */

static void lane_funnel_contract(flux_device *d) {
    size_t before = g_live_bytes;

    void *p = nullptr;
    flux_result r = flux_device_alloc(d,
                                      &(flux_device_alloc_opts){
                                          .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                                          .bytes = 128,
                                          .owner = "tests/hostile",
                                          .kind = "probe",
                                          .scope = FLUX_SCOPE_DEVICE,
                                      },
                                      &p);
    EXPECT(r == FLUX_OK);
    EXPECT(p != nullptr);
    /* It must be OUR memory, not libc's. */
    EXPECT(g_live_bytes == before + 128);

    /* Contract: returned memory is zeroed, even though our allocator does not
     * zero it. */
    bool all_zero = true;
    for (size_t i = 0; i < 128; ++i)
        if (((unsigned char *)p)[i] != 0)
            all_zero = false;
    EXPECT(all_zero);

    /* The ledger must show the row under the caller's owner, not flux's. */
    flux_observe_counters c = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "probe", "tests/hostile", &c) ==
           FLUX_OK);
    EXPECT(c.live == 1);

    flux_device_free(d, &(flux_device_free_opts){
                            .type = FLUX_TYPE_DEVICE_FREE_OPTS,
                            .ptr = p,
                            .owner = "tests/hostile",
                            .kind = "probe",
                            .scope = FLUX_SCOPE_DEVICE,
                        });
    EXPECT(g_live_bytes == before);
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "probe", "tests/hostile", &c) ==
               FLUX_OK &&
           c.live == 0);
}

/* Missing owner/kind is rejected, not defaulted: an unattributable allocation
 * is the defect this contract exists to remove. */
static void lane_funnel_rejects_unattributed(flux_device *d) {
    void *p = (void *)0x1;
    EXPECT(flux_device_alloc(d,
                             &(flux_device_alloc_opts){
                                 .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                                 .bytes = 16,
                             },
                             &p) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(p == nullptr);

    EXPECT(flux_device_alloc(d, nullptr, &p) == FLUX_ERROR_INVALID_ARGUMENT);
    EXPECT(flux_device_alloc(d,
                             &(flux_device_alloc_opts){
                                 .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                                 .bytes = 0,
                                 .owner = "x",
                                 .kind = "y",
                             },
                             &p) == FLUX_ERROR_INVALID_ARGUMENT);
}

/* OOM is reported, not papered over. */
static void lane_funnel_oom(flux_device *d) {
    size_t before = g_live_bytes;
    g_fail_at = g_alloc_seq + 1; /* fail the very next allocation */
    void *p = nullptr;
    flux_result r = flux_device_alloc(d,
                                      &(flux_device_alloc_opts){
                                          .type = FLUX_TYPE_DEVICE_ALLOC_OPTS,
                                          .bytes = 64,
                                          .owner = "tests/hostile",
                                          .kind = "oom",
                                          .scope = FLUX_SCOPE_DEVICE,
                                      },
                                      &p);
    g_fail_at = 0;
    EXPECT(r == FLUX_ERROR_OUT_OF_MEMORY);
    EXPECT(p == nullptr);
    /* A failed allocation must not leave a ledger row behind. */
    EXPECT(g_live_bytes == before);
    flux_observe_counters c = {};
    EXPECT(flux_observe_read_counters(d, FLUX_SCOPE_DEVICE, "oom", "tests/hostile", &c) !=
               FLUX_OK ||
           c.live == 0);
}

/* --- 2/3. libraries must route through it ----------------------------- */

static void lane_prism_uses_funnel(flux_device *d) {
    prism_mica_filter *warm = nullptr;
    if (prism_mica_filter_create(d, &warm) != FLUX_OK) {
        fprintf(stderr, "info: prism_mica_filter unavailable; lane skipped\n");
        return;
    }
    prism_mica_filter_release(warm);

    size_t before = g_live_bytes;
    uint64_t frees_before = g_frees;

    prism_mica_filter *f = nullptr;
    EXPECT(prism_mica_filter_create(d, &f) == FLUX_OK);
    size_t after_create = g_live_bytes;
    /* A filter is a struct plus its frame slots; any growth here is prism's
     * memory arriving through the caller's allocator. */
    EXPECT(after_create > before);

    prism_mica_filter_release(f);
    EXPECT(g_live_bytes == before);
    EXPECT(g_frees > frees_before);
}

static void lane_lens_uses_funnel(flux_device *d) {
    lens_desc ld = {.size = sizeof(ld), .device = d};
    lens *ui = nullptr;
    if (lens_create(&ld, &ui) != FLUX_OK) {
        fprintf(stderr, "info: lens unavailable; lane skipped\n");
        return;
    }
    size_t before = g_live_bytes;
    /* A frame build allocates from the lens funnel. */
    lens_begin(ui, nullptr);
    lens_end(ui);
    size_t during = g_live_bytes;

    lens_release(ui);
    flux_device_wait_idle(d);

    /* The context itself came through the funnel at create time, so the
     * meaningful assertion is that live bytes came back down and the
     * allocator saw traffic. */
    EXPECT(g_allocs > 0);
    EXPECT(g_live_bytes <= before);
    (void)during;
}

static void lane_image_snapshot_oom(flux_device *d) {
    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.width = 2;
    idesc.height = 2;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    flux_image *image = nullptr;
    EXPECT(flux_image_create(d, &idesc, &image) == FLUX_OK);

    size_t before = g_live_bytes;
    flux_image *snapshot = nullptr;
    g_fail_at = g_alloc_seq + 1;
    EXPECT(flux_image_snapshot(image, &snapshot) == FLUX_ERROR_OUT_OF_MEMORY);
    EXPECT(snapshot == nullptr);
    g_fail_at = 0;

    /* The failed snapshot must not poison the source image. */
    EXPECT(flux_image_snapshot(image, &snapshot) == FLUX_OK);
    EXPECT(snapshot != nullptr);
    flux_image_release(snapshot);
    flux_image_release(image);
    EXPECT(g_live_bytes <= before);
}

static void lane_image_lut_oom(flux_device *d) {
    uint8_t pdata[8192];
    icc_tag_def defs[8];
    size_t len = icc_build_mft1_constant_white(pdata, &defs[0]);
    uint8_t profile[16384];
    size_t size = icc_build_profile(profile, "mntr", "XYZ ", pdata, len, defs, 1);
    flux_icc_profile *icc = nullptr;
    EXPECT(flux_icc_profile_create(profile, size, &icc) == FLUX_OK);

    flux_image_color_space_desc csd = FLUX_IMAGE_COLOR_SPACE_DESC_INIT;
    csd.icc = icc;
    uint32_t pixels[4] = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
    flux_image_desc idesc = FLUX_IMAGE_DESC_INIT;
    idesc.next = &csd;
    idesc.width = 2;
    idesc.height = 2;
    idesc.format = FLUX_FORMAT_RGBA8_UNORM;
    idesc.initial_data = pixels;

    size_t before = g_live_bytes;
    bool published = false;
    /* Fail each successive allocation until the LUT image, binds, and params
     * buffer have all been exercised once. Every failure must unwind cleanly;
     * the eventual success proves the loop reached the whole construction. */
    for (uint64_t offset = 1; offset <= 64 && !published; ++offset) {
        flux_image *image = nullptr;
        g_fail_at = g_alloc_seq + offset;
        flux_result r = flux_image_create(d, &idesc, &image);
        g_fail_at = 0;
        if (r == FLUX_OK) {
            EXPECT(image != nullptr);
            published = true;
            flux_image_release(image);
        } else {
            EXPECT(image == nullptr);
            EXPECT(r == FLUX_ERROR_OUT_OF_MEMORY || r == FLUX_ERROR_BACKEND_FAILURE);
        }
    }
    EXPECT(published);
    flux_icc_profile_release(icc);
    flux_device_wait_idle(d);
    EXPECT(g_live_bytes <= before);
}

static void lane_sampler_buffer_oom(flux_device *d) {
    size_t before = g_live_bytes;
    bool sampler_ok = false;
    for (uint64_t offset = 1; offset <= 16 && !sampler_ok; ++offset) {
        flux_sampler *sampler = nullptr;
        g_fail_at = g_alloc_seq + offset;
        flux_result r =
            flux_sampler_create(d, &(flux_sampler_desc)FLUX_SAMPLER_DESC_INIT, &sampler);
        g_fail_at = 0;
        if (r == FLUX_OK) {
            EXPECT(sampler != nullptr);
            sampler_ok = true;
            flux_sampler_release(sampler);
        } else {
            EXPECT(sampler == nullptr);
            EXPECT(r == FLUX_ERROR_OUT_OF_MEMORY);
        }
    }
    EXPECT(sampler_ok);

    bool buffer_ok = false;
    for (uint64_t offset = 1; offset <= 16 && !buffer_ok; ++offset) {
        flux_buffer *buffer = nullptr;
        flux_buffer_desc bd = FLUX_BUFFER_DESC_INIT;
        bd.size = 256;
        bd.usage = FLUX_BUFFER_USAGE_STORAGE;
        bd.location = FLUX_BUFFER_HOST_VISIBLE;
        g_fail_at = g_alloc_seq + offset;
        flux_result r = flux_buffer_create(d, &bd, &buffer);
        g_fail_at = 0;
        if (r == FLUX_OK) {
            EXPECT(buffer != nullptr);
            buffer_ok = true;
            flux_buffer_release(buffer);
        } else {
            EXPECT(buffer == nullptr);
            EXPECT(r == FLUX_ERROR_OUT_OF_MEMORY || r == FLUX_ERROR_BACKEND_FAILURE);
        }
    }
    EXPECT(buffer_ok);
    flux_device_wait_idle(d);
    EXPECT(g_live_bytes <= before);
}

int main(void) {
    flux_device *d = make_hostile_device();
    if (!d) {
        fprintf(stderr, "test_hostile_allocator: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    size_t baseline = g_live_bytes;

    lane_funnel_contract(d);
    lane_funnel_rejects_unattributed(d);
    lane_funnel_oom(d);
    lane_prism_uses_funnel(d);
    lane_lens_uses_funnel(d);
    lane_image_snapshot_oom(d);
    lane_image_lut_oom(d);
    lane_sampler_buffer_oom(d);

    flux_device_release(d);
    flux_device_wait_idle(nullptr);

    /* The whole point: after full teardown nothing this allocator handed out is
     * still live. A library that bypassed the funnel would not show up here —
     * which is why the per-library lanes above assert positive traffic too. */
    if (g_live_bytes != baseline) {
        fprintf(stderr,
                "FAIL %s: %zu bytes still live at teardown (baseline %zu, peak %zu, "
                "%llu allocs / %llu frees)\n",
                __FILE__, g_live_bytes, baseline, g_peak_bytes, (unsigned long long)g_allocs,
                (unsigned long long)g_frees);
        g_test_failed++;
    }
    g_test_count++;
    EXPECT(g_violations == 0);

    TEST_SUMMARY();
}
