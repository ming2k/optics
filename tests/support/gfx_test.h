/*
 * gfx_test.h — Vulkan device fixtures shared by gfx, material and scene tests.
 *
 * This header carries *fixtures only*, no assertions. Every test binary uses
 * the one assertion harness from <optics_test.h>; a device fixture is not a
 * second harness.
 *
 * Every function returns nullptr when the host cannot satisfy the request.
 * Tests turn that into OT_SKIP with a reason, never into a silent pass.
 */

#ifndef OPTICS_GFX_TEST_H
#define OPTICS_GFX_TEST_H

#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <stdbool.h>
#include <stdint.h>

/* True if a headless device can be created on this host. Cached after the
 * first probe. */
bool optics_gfx_test_have_vulkan(void);

/* Headless device, validation off. Caller releases with optics_gfx_device_release. */
optics_gfx_device *optics_gfx_test_make_headless_device(void);
optics_gfx_device *optics_gfx_test_make_headless_device_in_flight(uint32_t frames_in_flight);

/* Device requiring the dma-buf external-memory feature set. */
optics_gfx_device *optics_gfx_test_make_dmabuf_device(void);

/* Validation-enabled device. nullptr when the Khronos validation layer is not
 * installed, so a "zero validation errors" assertion never passes vacuously. */
optics_gfx_device *optics_gfx_test_make_validation_device(optics_gfx_log_fn log);

/* Validation-enabled device that also requires dma-buf. */
optics_gfx_device *optics_gfx_test_make_validation_dmabuf_device(optics_gfx_log_fn log);

/* First memory type index matching both the requirements bitmask and the
 * wanted property flags; UINT32_MAX when no type matches. */
uint32_t optics_gfx_test_find_memory_type(optics_gfx_device *device, uint32_t filter,
                                          VkMemoryPropertyFlags want);

#endif /* OPTICS_GFX_TEST_H */
