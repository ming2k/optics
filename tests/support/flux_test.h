/*
 * flux_test.h — Vulkan device fixtures shared by flux, prism and vista tests.
 *
 * This header carries *fixtures only*, no assertions. Every test binary uses
 * the one assertion harness from <optics_test.h>; a device fixture is not a
 * second harness.
 *
 * Every function returns nullptr when the host cannot satisfy the request.
 * Tests turn that into OT_SKIP with a reason, never into a silent pass.
 */

#ifndef FLUX_TEST_H
#define FLUX_TEST_H

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

/* True if a headless device can be created on this host. Cached after the
 * first probe. */
bool flux_test_have_vulkan(void);

/* Headless device, validation off. Caller releases with flux_device_release. */
flux_device *flux_test_make_headless_device(void);
flux_device *flux_test_make_headless_device_in_flight(uint32_t frames_in_flight);

/* Device requiring the dma-buf external-memory feature set. */
flux_device *flux_test_make_dmabuf_device(void);

/* Validation-enabled device. nullptr when the Khronos validation layer is not
 * installed, so a "zero validation errors" assertion never passes vacuously. */
flux_device *flux_test_make_validation_device(flux_log_fn log);

/* Validation-enabled device that also requires dma-buf. */
flux_device *flux_test_make_validation_dmabuf_device(flux_log_fn log);

/* First memory type index matching both the requirements bitmask and the
 * wanted property flags; UINT32_MAX when no type matches. */
uint32_t flux_test_find_memory_type(flux_device *device, uint32_t filter,
                                    VkMemoryPropertyFlags want);

#endif /* FLUX_TEST_H */
