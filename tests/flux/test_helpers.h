#ifndef FLUX_TEST_HELPERS_H
#define FLUX_TEST_HELPERS_H

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Per-binary counters backing EXPECT / EXPECT_NEAR / TEST_SUMMARY.
 * Defined once in test_helpers.c, linked into every test executable
 * via the test_helpers static library. */
extern int g_test_failed;
extern int g_test_count;

#define EXPECT(cond)                                                                               \
    do {                                                                                           \
        g_test_count++;                                                                            \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_test_failed++;                                                                       \
        }                                                                                          \
    } while (0)

#define EXPECT_NEAR(a, b, eps)                                                                     \
    do {                                                                                           \
        g_test_count++;                                                                            \
        double _da = (double)(a), _db = (double)(b);                                               \
        if (!(fabs(_da - _db) <= (eps))) {                                                         \
            fprintf(stderr, "FAIL %s:%d: %s (%.6g) ~= %s (%.6g) eps=%g\n", __FILE__, __LINE__, #a, \
                    _da, #b, _db, (double)(eps));                                                  \
            g_test_failed++;                                                                       \
        }                                                                                          \
    } while (0)

#define TEST_SUMMARY()                                                                             \
    do {                                                                                           \
        fprintf(stderr, "%s: %d/%d passed\n", __FILE__, g_test_count - g_test_failed,              \
                g_test_count);                                                                     \
        return g_test_failed ? 1 : 0;                                                              \
    } while (0)

/*
 * Capability probe: returns true iff a Vulkan device can be created
 * on this host. Cached after the first call so a test binary that
 * issues several device probes does not pay repeated instance /
 * device creation. Used by tests to decide between "run device-bound
 * checks" and "skip with PASS" without an #ifdef.
 */
bool test_helpers_have_vulkan(void);

/*
 * Create a headless test device (no surface), validation off, single
 * frame in flight. Returns nullptr if Vulkan is unavailable on this
 * host; callers should treat that as a graceful skip.
 *
 * Caller owns the returned pointer; release with flux_device_release().
 */
flux_device *test_helpers_make_headless_device(void);

/*
 * Create a device that requires the dma-buf external-memory
 * extensions (KHR_external_memory_fd, EXT_external_memory_dma_buf,
 * EXT_image_drm_format_modifier, EXT_queue_family_foreign). Returns
 * nullptr if any of those is missing on the host.
 */
flux_device *test_helpers_make_dmabuf_device(void);

/*
 * Create a device with the Vulkan validation layers ON and `log` as
 * the receive callback for every layer / driver diagnostic. Returns
 * nullptr if validation layers are not installed on the host.
 *
 * Used by test_validation; the supplied callback should be able to
 * count and (optionally) echo the messages it receives.
 */
flux_device *test_helpers_make_validation_device(flux_log_fn log);

/*
 * Pick a memory type index that satisfies both the `filter`
 * bitmask (VkMemoryRequirements::memoryTypeBits) and the `want`
 * VkMemoryPropertyFlags. Returns UINT32_MAX if no type matches
 * (caller treats as OOM and bails). Used by tests that need to
 * allocate a one-shot VkBuffer with specific visibility.
 */
uint32_t test_helpers_find_memory_type(flux_device *d, uint32_t filter, VkMemoryPropertyFlags want);

#endif
