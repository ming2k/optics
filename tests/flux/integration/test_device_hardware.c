/*
 * Real-driver hardware assertion (ADR-0120, Phase B0 gate).
 *
 * Assert that the headless Vulkan device is an actual hardware GPU and NOT a
 * software fallback (e.g. Lavapipe / SwiftShader / llvmpipe). Every Phase B gate
 * relies on hardware execution semantics (shared memory atomics, barriers,
 * subgroup operations, and deterministic memory visibility) that software
 * emulators cannot prove correct.
 */
#include "test_helpers.h"

#include <flux/flux.h>
#include <flux/vulkan.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_device_hardware: no Vulkan device available; skipping\n");
        return 0;
    }

    VkPhysicalDevice pd = flux_device_vk_physical_device(d);
    EXPECT(pd != VK_NULL_HANDLE);
    if (pd == VK_NULL_HANDLE) {
        flux_device_release(d);
        TEST_SUMMARY();
    }

    VkPhysicalDeviceDriverProperties driver_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceSubgroupProperties subgroup_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        .pNext = &driver_props,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup_props,
    };
    vkGetPhysicalDeviceProperties2(pd, &props2);

    const VkPhysicalDeviceProperties *props = &props2.properties;
    fprintf(stderr, "Vulkan physical device:\n");
    fprintf(stderr, "  deviceName:   %s\n", props->deviceName);
    fprintf(stderr, "  vendorID:     0x%04x\n", props->vendorID);
    fprintf(stderr, "  deviceID:     0x%04x\n", props->deviceID);
    fprintf(stderr, "  deviceType:   %d\n", props->deviceType);
    fprintf(stderr, "  driverID:     %d\n", driver_props.driverID);
    fprintf(stderr, "  driverName:   %s\n", driver_props.driverName);
    fprintf(stderr, "  driverInfo:   %s\n", driver_props.driverInfo);
    fprintf(stderr, "  subgroupSize: %u\n", subgroup_props.subgroupSize);
    fprintf(stderr, "  maxComputeInvocations: %u\n", props->limits.maxComputeWorkGroupInvocations);

    /* Phase B0 Gate: device must be hardware (discrete or integrated GPU).
     * If running in an environment where FLUX_ALLOW_SOFTWARE_DEVICE is explicitly set,
     * we report diagnostic information but skip hardware enforcement; otherwise,
     * Phase B0 treats a software fallback as a hard test failure. */
    const char *allow_sw = getenv("FLUX_ALLOW_SOFTWARE_DEVICE");
    bool software_permitted = (allow_sw && strcmp(allow_sw, "1") == 0);

    if (software_permitted) {
        fprintf(stderr, "  [NOTE] Software Vulkan device explicitly permitted by environment\n");
    } else {
        /* Hardware GPU assertion: neither CPU device type nor llvmpipe driver */
        EXPECT(props->deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU);
        EXPECT(driver_props.driverID != VK_DRIVER_ID_MESA_LLVMPIPE);
        EXPECT(props->deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
               props->deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU);
    }

    /* Compute prerequisites for Phase B (16x16 tile = 256 invocations): */
    EXPECT(props->limits.maxComputeWorkGroupInvocations >= 256);
    EXPECT(props->limits.maxComputeWorkGroupSize[0] >= 16);
    EXPECT(props->limits.maxComputeWorkGroupSize[1] >= 16);
    EXPECT(subgroup_props.subgroupSize >= 4);

    flux_device_release(d);
    TEST_SUMMARY();
}
