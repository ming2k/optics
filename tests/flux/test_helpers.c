/*
 * Shared definitions backing test_helpers.h. One definition of
 * g_test_failed / g_test_count per test binary (each test is its
 * own executable, linked against this static lib).
 *
 * The Vulkan probe helpers here are the single source of truth for
 * "can I create a device on this host?" Every test that needs a
 * device goes through these; no test should open-code the
 * FLUX_DEVICE_DESC_INIT + flux_device_create sequence.
 */
#include "test_helpers.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int g_test_failed = 0;
int g_test_count = 0;

/* LeakSanitizer: Mesa ICDs leak a handful of driver-internal blocks
 * at vkDestroyDevice (e.g. ANV's companion RCS command buffer used
 * by the gfx125 BLORP copy path). Those blocks are allocated and
 * owned entirely inside the driver; flux cannot free them. Suppress
 * by ICD module so the suites stay signal-bearing: a flux-side leak
 * still reports, because every flux object also owns a host block
 * allocated in libflux.so (flux_internal_alloc), which no entry
 * below matches. */
const char *__lsan_default_suppressions(void) {
    return "leak:libvulkan_intel\n"
           "leak:libvulkan_lvp\n"
           "leak:libvulkan_radeon\n"
           "leak:libvulkan_nouveau\n";
}

/* The Vulkan loader dlcloses ICDs at vkDestroyInstance; by the time
 * LeakSanitizer symbolises its report the driver mapping is gone and
 * the suppressions above can never match. Re-open every loaded ICD
 * with RTLD_NODELETE so it stays mapped through process exit.
 * RTLD_NOLOAD only promotes libraries that are already resident —
 * nothing new is loaded. */
static void pin_loaded_vulkan_drivers(void) {
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps)
        return;
    char line[512];
    char last[512] = "";
    while (fgets(line, sizeof(line), maps)) {
        char *path = strchr(line, '/');
        if (!path || !strstr(path, "libvulkan_"))
            continue;
        path[strcspn(path, "\n")] = '\0';
        if (strcmp(path, last) == 0)
            continue; /* one dlopen per lib */
        strcpy(last, path);
        dlopen(path, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
    }
    fclose(maps);
}

/* Cached so the dozen-odd device-needing tests don't each pay the
 * instance+device-create cost just to learn "no Vulkan here". */
static int g_have_vulkan_cached = -1;

bool test_helpers_have_vulkan(void) {
    if (g_have_vulkan_cached == -1) {
        flux_device *d = test_helpers_make_headless_device();
        g_have_vulkan_cached = (d != nullptr) ? 1 : 0;
        if (d)
            flux_device_release(d);
    }
    return g_have_vulkan_cached == 1;
}

flux_device *test_helpers_make_headless_device(void) {
    flux_device_desc d = FLUX_DEVICE_DESC_INIT;
    d.headless = true;
    d.frames_in_flight = 1;
    d.validation = FLUX_VALIDATION_OFF;
    flux_device *out = nullptr;
    if (flux_device_create(&d, &out) != FLUX_OK)
        return nullptr;
    pin_loaded_vulkan_drivers();
    return out;
}

flux_device *test_helpers_make_dmabuf_device(void) {
    flux_device_features_desc features = FLUX_DEVICE_FEATURES_DESC_INIT;
    features.required = FLUX_DEVICE_FEATURE_DMABUF | FLUX_DEVICE_FEATURE_DMABUF_SYNC_FILE;
    flux_device_desc d = FLUX_DEVICE_DESC_INIT;
    d.next = &features;
    d.headless = true;
    d.frames_in_flight = 1;
    d.validation = FLUX_VALIDATION_OFF;
    flux_device *out = nullptr;
    if (flux_device_create(&d, &out) != FLUX_OK)
        return nullptr;
    pin_loaded_vulkan_drivers();
    return out;
}

flux_device *test_helpers_make_validation_device(flux_log_fn log) {
    flux_device_desc d = FLUX_DEVICE_DESC_INIT;
    d.headless = true;
    d.frames_in_flight = 1;
    d.validation = FLUX_VALIDATION_ON;
    d.log = log;
    flux_device *out = nullptr;
    if (flux_device_create(&d, &out) != FLUX_OK)
        return nullptr;
    pin_loaded_vulkan_drivers();
    return out;
}

uint32_t test_helpers_find_memory_type(flux_device *d, uint32_t filter,
                                       VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(flux_device_vk_physical_device(d), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(filter & (1u << i)))
            continue;
        if ((mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    }
    return UINT32_MAX;
}
