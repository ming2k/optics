/*
 * Vulkan instance + physical device + logical device + queues +
 * pipeline cache. Stage 2a: no swapchain, no memory allocator
 * (Stage 2b adds VMA-or-equivalent), no bindless heap (Stage 2b).
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Allocator + logger plumbing                                       */
/* ------------------------------------------------------------------ */

/* Always returns zeroed memory. If the caller supplied an allocator,
 * we zero the result ourselves because a user `alloc` may not. The
 * library treats `flux_internal_alloc` as zalloc throughout, so the
 * contract is consistent regardless of allocator origin. */
void *flux_internal_alloc(flux_device *d, size_t bytes) {
    void *p;
    if (d && d->allocator.alloc) {
        p = d->allocator.alloc(bytes, d->allocator.user);
        if (p)
            memset(p, 0, bytes);
    } else {
        p = calloc(1, bytes);
    }
    return p;
}

void flux_internal_free(flux_device *d, void *ptr) {
    if (!ptr)
        return;
    if (d && d->allocator.free)
        d->allocator.free(ptr, d->allocator.user);
    else
        free(ptr);
}

/* ------------------------------------------------------------------ */
/*  Validation layer wiring                                           */
/* ------------------------------------------------------------------ */

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    flux_device *d = (flux_device *)user;
    flux_log_level level = FLUX_LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        level = FLUX_LOG_ERROR;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        level = FLUX_LOG_WARN;
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        level = FLUX_LOG_INFO;
    else
        level = FLUX_LOG_DEBUG;
    (void)type;
    if (d && d->log && data && data->pMessage) {
        d->log(level, "vulkan-validation", 0, "%s", data->pMessage, d->log_user);
    }
    return VK_FALSE;
}

static bool has_layer(const VkLayerProperties *layers, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(layers[i].layerName, name) == 0)
            return true;
    }
    return false;
}

static bool has_extension(const VkExtensionProperties *exts, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(exts[i].extensionName, name) == 0)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Instance                                                          */
/* ------------------------------------------------------------------ */

#define MAX_EXT 32
#define MAX_LAYER 4

static flux_result create_instance(flux_device *d, const flux_device_desc *desc) {
    /* Required instance extensions: caller list + debug-utils if validating. */
    const char *exts[MAX_EXT];
    uint32_t ext_count = 0;

    for (uint32_t i = 0; i < desc->required_instance_extension_count; ++i) {
        if (ext_count >= MAX_EXT) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "too many instance extensions");
            return FLUX_ERROR_INVALID_ARGUMENT;
        }
        exts[ext_count++] = desc->required_instance_extensions[i];
    }

    /* Validation: enable VK_LAYER_KHRONOS_validation + debug-utils ext. */
    const char *layers[MAX_LAYER];
    uint32_t layer_count = 0;
    bool want_validation = false;

    switch (desc->validation) {
    case FLUX_VALIDATION_ON:
        want_validation = true;
        break;
    case FLUX_VALIDATION_OFF:
        want_validation = false;
        break;
    case FLUX_VALIDATION_AUTO:
#ifdef NDEBUG
        want_validation = false;
#else
        want_validation = true;
#endif
        break;
    }

    if (want_validation) {
        uint32_t avail = 0;
        VkResult lr = vkEnumerateInstanceLayerProperties(&avail, nullptr);
        VkLayerProperties *layer_props = nullptr;
        if (lr == VK_SUCCESS && avail > 0) {
            layer_props = calloc(avail, sizeof(*layer_props));
            if (layer_props) {
                lr = vkEnumerateInstanceLayerProperties(&avail, layer_props);
            }
        }
        if (lr == VK_SUCCESS && layer_props &&
            has_layer(layer_props, avail, "VK_LAYER_KHRONOS_validation")) {
            layers[layer_count++] = "VK_LAYER_KHRONOS_validation";
            d->validation_enabled = true;
        }
        free(layer_props);

        if (d->validation_enabled) {
            if (ext_count < MAX_EXT)
                exts[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        }
    }

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "flux app",
        .applicationVersion = 0,
        .pEngineName = "flux",
        .engineVersion = FLUX_VERSION_NUMBER,
        .apiVersion = VK_API_VERSION_1_3,
    };

    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledLayerCount = layer_count,
        .ppEnabledLayerNames = layers,
        .enabledExtensionCount = ext_count,
        .ppEnabledExtensionNames = exts,
    };

    VkResult vr = vkCreateInstance(&ici, nullptr, &d->instance);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateInstance failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    if (d->validation_enabled) {
        VkDebugUtilsMessengerCreateInfoEXT dmci = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_callback,
            .pUserData = d,
        };
        PFN_vkCreateDebugUtilsMessengerEXT create_dum =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                d->instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_dum) {
            create_dum(d->instance, &dmci, nullptr, &d->debug_messenger);
        }
    }

    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Physical device selection                                         */
/* ------------------------------------------------------------------ */

static const char *required_device_ext_baseline[] = {
    /* Vulkan 1.3 promoted features cover sync2, dynamic rendering,
     * timeline semaphores, buffer device address. Descriptor indexing
     * is promoted to core 1.2 features. We don't need to enable these
     * as extensions on a 1.3 device — they're in PhysicalDeviceFeatures2
     * chain instead. */
    nullptr,
};

static int score_device(VkPhysicalDevice pd) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    if (props.apiVersion < VK_API_VERSION_1_3)
        return -1;
    switch (props.deviceType) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 1000;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 500;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 200;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 100;
    default:
        return 0;
    }
}

static flux_result pick_physical_device(flux_device *d, const flux_device_desc *desc) {
    (void)desc;
    uint32_t count = 0;
    VkResult vr = vkEnumeratePhysicalDevices(d->instance, &count, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEnumeratePhysicalDevices failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    if (count == 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no Vulkan-capable GPU found");
        return FLUX_ERROR_UNSUPPORTED;
    }

    VkPhysicalDevice *pds = calloc(count, sizeof(*pds));
    if (!pds)
        return FLUX_ERROR_OUT_OF_MEMORY;
    vr = vkEnumeratePhysicalDevices(d->instance, &count, pds);
    if (vr != VK_SUCCESS) {
        free(pds);
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEnumeratePhysicalDevices failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkPhysicalDevice best = VK_NULL_HANDLE;
    int best_score = -1;
    for (uint32_t i = 0; i < count; ++i) {
        int s = score_device(pds[i]);
        if (s > best_score) {
            best_score = s;
            best = pds[i];
        }
    }
    free(pds);

    if (best == VK_NULL_HANDLE || best_score < 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no Vulkan 1.3 capable GPU available");
        return FLUX_ERROR_UNSUPPORTED;
    }

    d->physical_device = best;
    vkGetPhysicalDeviceProperties(d->physical_device, &d->props);
    vkGetPhysicalDeviceMemoryProperties(d->physical_device, &d->mem_props);

    d->vendor_id = d->props.vendorID;
    d->device_id = d->props.deviceID;
    d->is_nvidia = (d->props.vendorID == 0x10DE);
    d->is_amd = (d->props.vendorID == 0x1002 || d->props.vendorID == 0x1022);
    d->is_intel = (d->props.vendorID == 0x8086);
    d->is_apple = (d->props.vendorID == 0x106B);
    d->buffer_image_granularity = d->props.limits.bufferImageGranularity;

    /* Cache the descriptor-indexing properties for the bindless heap. */
    d->descriptor_indexing_props = (VkPhysicalDeviceDescriptorIndexingProperties){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 p2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &d->descriptor_indexing_props,
    };
    vkGetPhysicalDeviceProperties2(d->physical_device, &p2);
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Queue family selection                                            */
/* ------------------------------------------------------------------ */

#define INVALID_FAMILY UINT32_C(0xffffffff)

static flux_result pick_queue_families(flux_device *d) {
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical_device, &count, nullptr);
    if (count == 0) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "physical device exposes no queue families");
        return FLUX_ERROR_UNSUPPORTED;
    }
    VkQueueFamilyProperties *families = calloc(count, sizeof(*families));
    if (!families)
        return FLUX_ERROR_OUT_OF_MEMORY;
    vkGetPhysicalDeviceQueueFamilyProperties(d->physical_device, &count, families);
    /* vkGetPhysicalDeviceQueueFamilyProperties is void-returning; no
     * VkResult to check (Vulkan 1.0 guarantee). */

    d->graphics_family = INVALID_FAMILY;
    d->transfer_family = INVALID_FAMILY;
    d->transfer_dedicated = false;

    /* Graphics: first family with GRAPHICS bit. */
    for (uint32_t i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            d->graphics_family = i;
            break;
        }
    }

    /* Transfer: prefer a dedicated family (TRANSFER but not GRAPHICS or COMPUTE).
     * If none exists, fall back to a COMPUTE+TRANSFER family. Then to graphics. */
    for (uint32_t i = 0; i < count; ++i) {
        VkQueueFlags f = families[i].queueFlags;
        if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT) &&
            !(f & VK_QUEUE_COMPUTE_BIT)) {
            d->transfer_family = i;
            d->transfer_dedicated = true;
            break;
        }
    }
    if (d->transfer_family == INVALID_FAMILY) {
        for (uint32_t i = 0; i < count; ++i) {
            VkQueueFlags f = families[i].queueFlags;
            if ((f & VK_QUEUE_TRANSFER_BIT) && !(f & VK_QUEUE_GRAPHICS_BIT)) {
                d->transfer_family = i;
                d->transfer_dedicated = true;
                break;
            }
        }
    }
    if (d->transfer_family == INVALID_FAMILY) {
        d->transfer_family = d->graphics_family;
        d->transfer_dedicated = false;
    }

    free(families);

    if (d->graphics_family == INVALID_FAMILY) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "no graphics-capable queue family");
        return FLUX_ERROR_UNSUPPORTED;
    }
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Logical device                                                    */
/* ------------------------------------------------------------------ */

static flux_result create_logical_device(flux_device *d, const flux_device_desc *desc) {
    /* Device extensions: caller-required only. Vulkan 1.3 features
     * (sync2, dynamic rendering, timeline semaphores, buffer device
     * address, descriptor indexing) come from the feature chain. */
    const char *device_exts[MAX_EXT];
    uint32_t device_ext_count = 0;
    for (uint32_t i = 0; i < desc->required_device_extension_count; ++i) {
        if (device_ext_count >= MAX_EXT) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "too many device extensions");
            return FLUX_ERROR_INVALID_ARGUMENT;
        }
        device_exts[device_ext_count++] = desc->required_device_extensions[i];
    }

    /* Validate that the physical device advertises every required ext. */
    if (device_ext_count > 0) {
        uint32_t avail = 0;
        VkResult er =
            vkEnumerateDeviceExtensionProperties(d->physical_device, nullptr, &avail, nullptr);
        if (er != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEnumerateDeviceExtensionProperties failed",
                         er);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        if (avail == 0) {
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                      "device advertises no extensions but caller required some");
            return FLUX_ERROR_UNSUPPORTED;
        }
        VkExtensionProperties *available = calloc(avail, sizeof(*available));
        if (!available)
            return FLUX_ERROR_OUT_OF_MEMORY;
        er = vkEnumerateDeviceExtensionProperties(d->physical_device, nullptr, &avail, available);
        if (er != VK_SUCCESS) {
            free(available);
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEnumerateDeviceExtensionProperties failed",
                         er);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        for (uint32_t i = 0; i < device_ext_count; ++i) {
            if (!has_extension(available, avail, device_exts[i])) {
                FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "required device extension not supported");
                free(available);
                return FLUX_ERROR_UNSUPPORTED;
            }
        }
        free(available);
    }
    (void)required_device_ext_baseline;

    for (uint32_t i = 0; i < device_ext_count; ++i) {
        if (strcmp(device_exts[i], VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0) {
            d->has_external_memory_fd = true;
        } else if (strcmp(device_exts[i], VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) == 0) {
            d->has_external_memory_dma_buf = true;
        } else if (strcmp(device_exts[i], VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) == 0) {
            d->has_image_drm_format_modifier = true;
        } else if (strcmp(device_exts[i], VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) == 0) {
            d->has_external_semaphore_fd = true;
        } else if (strcmp(device_exts[i], VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME) == 0) {
            d->has_queue_family_foreign = true;
        }
    }

    /* Feature chain: enable Vulkan 1.3 features we rely on globally. */
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &feat13,
        .timelineSemaphore = VK_TRUE,
        .bufferDeviceAddress = VK_TRUE,
        .descriptorIndexing = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
        .hostQueryReset = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat12,
        .features = {0},
    };

    /* Confirm the device actually supports every flag we set. */
    VkPhysicalDeviceVulkan13Features have13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features have12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &have13};
    VkPhysicalDeviceFeatures2 have2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                       .pNext = &have12};
    vkGetPhysicalDeviceFeatures2(d->physical_device, &have2);

    if (!have13.dynamicRendering || !have13.synchronization2 || !have12.timelineSemaphore ||
        !have12.bufferDeviceAddress || !have12.descriptorIndexing || !have12.hostQueryReset) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "physical device missing required Vulkan 1.3 features "
                  "(dynamicRendering, sync2, timelineSemaphore, bufferDeviceAddress, "
                  "descriptorIndexing, hostQueryReset)");
        return FLUX_ERROR_UNSUPPORTED;
    }

    /* Queue create infos: graphics; transfer iff dedicated. */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qcis[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = d->graphics_family,
            .queueCount = 1,
            .pQueuePriorities = &prio,
        },
    };
    uint32_t qci_count = 1;
    if (d->transfer_dedicated && d->transfer_family != d->graphics_family) {
        qcis[1] = (VkDeviceQueueCreateInfo){
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = d->transfer_family,
            .queueCount = 1,
            .pQueuePriorities = &prio,
        };
        qci_count = 2;
    }

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feat2,
        .queueCreateInfoCount = qci_count,
        .pQueueCreateInfos = qcis,
        .enabledExtensionCount = device_ext_count,
        .ppEnabledExtensionNames = device_exts,
    };

    VkResult vr = vkCreateDevice(d->physical_device, &dci, nullptr, &d->device);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateDevice failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    vkGetDeviceQueue(d->device, d->graphics_family, 0, &d->graphics_queue);
    vkGetDeviceQueue(d->device, d->transfer_family, 0, &d->transfer_queue);

    /* Pipeline cache — seeded from the consumer's storage if a
     * persistence hook was wired into flux_device_desc (Skia
     * PersistentCache model). With no hook the cache starts cold
     * and lives only for the device lifetime. The library never
     * touches the filesystem itself. */
    void *seed_data = nullptr;
    size_t seed_size = 0;
    if (d->pipeline_cache_load) {
        seed_data = d->pipeline_cache_load(d->pipeline_cache_userdata, &seed_size);
    }

    VkPipelineCacheCreateInfo pcci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .initialDataSize = seed_size,
        .pInitialData = seed_data,
    };
    vr = vkCreatePipelineCache(d->device, &pcci, nullptr, &d->pipeline_cache);
    free(seed_data);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreatePipelineCache failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Pipeline cache flush (consumer-driven)                            */
/*                                                                    */
/*  If the consumer wired a save hook into flux_device_desc, hand the */
/*  current VkPipelineCache blob to it. The library owns no storage   */
/*  strategy; this just bridges Vulkan's blob out to the callback.    */
/* ------------------------------------------------------------------ */

static void flush_pipeline_cache(const flux_device *d) {
    if (!d->pipeline_cache_save || !d->pipeline_cache)
        return;

    size_t size = 0;
    if (vkGetPipelineCacheData(d->device, d->pipeline_cache, &size, nullptr) != VK_SUCCESS)
        return;
    if (size == 0)
        return;
    void *buf = malloc(size);
    if (!buf)
        return;
    if (vkGetPipelineCacheData(d->device, d->pipeline_cache, &size, buf) != VK_SUCCESS) {
        free(buf);
        return;
    }
    d->pipeline_cache_save(d->pipeline_cache_userdata, buf, size);
    free(buf);
}

/* ------------------------------------------------------------------ */
/*  Public lifecycle                                                  */
/* ------------------------------------------------------------------ */

flux_result flux_device_create(const flux_device_desc *desc, flux_device **out) {
    if (!desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_DEVICE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_DEVICE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_device *d = calloc(1, sizeof(*d));
    if (!d) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "device alloc");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    atomic_init(&d->ref_count, 1u);
    d->allocator = desc->allocator;
    d->log = desc->log;
    d->log_user = desc->log_user;
    d->pipeline_cache_load = desc->pipeline_cache_load;
    d->pipeline_cache_save = desc->pipeline_cache_save;
    d->pipeline_cache_userdata = desc->pipeline_cache_userdata;
    d->frames_in_flight = desc->frames_in_flight ? desc->frames_in_flight : 2;
    d->headless = desc->headless;

    /* The per-surface frame slot array is fixed at
     * FLUX_MAX_FRAMES_IN_FLIGHT entries; values above are silently
     * clamped in flux_surface_create. Surface the clamp here so the
     * caller learns at device-bringup time, not later when a surface
     * ends up with fewer frames than requested. */
    if (desc->frames_in_flight > FLUX_MAX_FRAMES_IN_FLIGHT) {
        d->frames_in_flight = FLUX_MAX_FRAMES_IN_FLIGHT;
        if (d->log) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "flux_device_create: desc->frames_in_flight=%u exceeds "
                     "FLUX_MAX_FRAMES_IN_FLIGHT=%u; clamped (per-surface frame "
                     "slot array is fixed-size)",
                     desc->frames_in_flight, FLUX_MAX_FRAMES_IN_FLIGHT);
            d->log(FLUX_LOG_WARN, "flux_device_create", 0, "%s", buf, d->log_user);
        }
    }

    flux_result r;
    r = create_instance(d, desc);
    if (r != FLUX_OK)
        goto fail;
    r = pick_physical_device(d, desc);
    if (r != FLUX_OK)
        goto fail;
    r = pick_queue_families(d);
    if (r != FLUX_OK)
        goto fail;
    /* Reject hosts that can't satisfy the library's push-constant
     * budget before we waste time creating the logical device. */
    if (d->props.limits.maxPushConstantsSize < FLUX_DEVICE_REQUIRED_PUSH_BYTES) {
        FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                  "device.limits.maxPushConstantsSize below FLUX_DEVICE_REQUIRED_PUSH_BYTES");
        r = FLUX_ERROR_UNSUPPORTED;
        goto fail;
    }
    r = create_logical_device(d, desc);
    if (r != FLUX_OK)
        goto fail;
    if (pthread_mutex_init(&d->queue_lock, nullptr) != 0) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "queue lock init failed");
        r = FLUX_ERROR_BACKEND_FAILURE;
        goto fail;
    }
    d->queue_lock_initialized = true;
    r = flux_vk_allocator_init(d);
    if (r != FLUX_OK)
        goto fail;
    r = flux_bindless_heap_init(d);
    if (r != FLUX_OK)
        goto fail;

    if (d->log) {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "device created: %s (gfx q=%u, xfer q=%u%s, validation=%s, headless=%s)",
                 d->props.deviceName, d->graphics_family, d->transfer_family,
                 d->transfer_dedicated ? " dedicated" : "", d->validation_enabled ? "on" : "off",
                 d->headless ? "yes" : "no");
        d->log(FLUX_LOG_INFO, "flux", 0, "%s", buf, d->log_user);
    }

    *out = d;
    return FLUX_OK;

fail:
    /* Tear down partial state. */
    flux_bindless_heap_destroy(d);
    flux_vk_allocator_destroy(d);
    if (d->queue_lock_initialized) {
        pthread_mutex_destroy(&d->queue_lock);
        d->queue_lock_initialized = false;
    }
    if (d->pipeline_cache)
        vkDestroyPipelineCache(d->device, d->pipeline_cache, nullptr);
    if (d->device)
        vkDestroyDevice(d->device, nullptr);
    if (d->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                d->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(d->instance, d->debug_messenger, nullptr);
    }
    if (d->instance)
        vkDestroyInstance(d->instance, nullptr);
    free(d);
    return r;
}

flux_device *flux_device_retain(flux_device *d) {
    if (d)
        atomic_fetch_add_explicit(&d->ref_count, 1u, memory_order_relaxed);
    return d;
}

void flux_device_release(flux_device *d) {
    if (!d)
        return;
    if (atomic_fetch_sub_explicit(&d->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    if (d->device)
        vkDeviceWaitIdle(d->device);

    /* Per-module teardown before we destroy the VkDevice. The effect
     * module is torn down first because its transient images may
     * have been registered into the bindless heap and must be
     * released before the heap goes away. */
    if (d->effect_state_destroy)
        d->effect_state_destroy(d);
    if (d->canvas_state_destroy)
        d->canvas_state_destroy(d);

    if (d->default_sampler)
        vkDestroySampler(d->device, d->default_sampler, nullptr);
    flux_bindless_heap_destroy(d);
    flux_vk_allocator_destroy(d);
    flush_pipeline_cache(d);
    if (d->queue_lock_initialized) {
        pthread_mutex_destroy(&d->queue_lock);
        d->queue_lock_initialized = false;
    }
    if (d->pipeline_cache)
        vkDestroyPipelineCache(d->device, d->pipeline_cache, nullptr);
    if (d->device)
        vkDestroyDevice(d->device, nullptr);

    if (d->debug_messenger) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
                d->instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(d->instance, d->debug_messenger, nullptr);
    }
    if (d->instance)
        vkDestroyInstance(d->instance, nullptr);
    free(d);
}

void flux_device_wait_idle(const flux_device *d) {
    if (d && d->device)
        vkDeviceWaitIdle(d->device);
}

void *flux_device_alloc(flux_device *d, size_t bytes) {
    return flux_internal_alloc(d, bytes);
}

void flux_device_free(flux_device *d, void *ptr) {
    flux_internal_free(d, ptr);
}

void flux_device_memory_budget(const flux_device *d, flux_memory_budget *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!d || !d->device)
        return;

    out->heap_count = d->mem_props.memoryHeapCount;
    if (out->heap_count > FLUX_MAX_MEMORY_HEAPS)
        out->heap_count = FLUX_MAX_MEMORY_HEAPS;

    for (uint32_t i = 0; i < out->heap_count; ++i) {
        out->heap_bytes_total[i] = d->mem_props.memoryHeaps[i].size;
    }

    out->has_budget_extension = d->mem_allocator.has_memory_budget;
    if (!d->mem_allocator.has_memory_budget)
        return;

    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT,
    };
    VkPhysicalDeviceMemoryProperties2 mem_props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
        .pNext = &budget_props,
    };
    vkGetPhysicalDeviceMemoryProperties2(d->physical_device, &mem_props2);

    for (uint32_t i = 0; i < out->heap_count && i < VK_MAX_MEMORY_HEAPS; ++i) {
        out->heap_bytes_used[i] = budget_props.heapUsage[i];
        out->heap_budget[i] = budget_props.heapBudget[i];
    }
}

/* ------------------------------------------------------------------ */
/*  Raw Vulkan accessors (vulkan.h)                                   */
/* ------------------------------------------------------------------ */

VkInstance flux_device_vk_instance(const flux_device *d) {
    return d ? d->instance : VK_NULL_HANDLE;
}
VkPhysicalDevice flux_device_vk_physical_device(const flux_device *d) {
    return d ? d->physical_device : VK_NULL_HANDLE;
}
VkDevice flux_device_vk_device(const flux_device *d) {
    return d ? d->device : VK_NULL_HANDLE;
}
VkQueue flux_device_vk_graphics_queue(const flux_device *d) {
    return d ? d->graphics_queue : VK_NULL_HANDLE;
}
uint32_t flux_device_vk_graphics_family(const flux_device *d) {
    return d ? d->graphics_family : 0;
}
VkQueue flux_device_vk_transfer_queue(const flux_device *d) {
    return d ? d->transfer_queue : VK_NULL_HANDLE;
}
uint32_t flux_device_vk_transfer_family(const flux_device *d) {
    return d ? d->transfer_family : 0;
}
VkPipelineCache flux_device_vk_pipeline_cache(const flux_device *d) {
    return d ? d->pipeline_cache : VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ */
/*  Bindless descriptor heap                                          */
/*                                                                    */
/*  Single VkDescriptorSet at slot 0 with four bindings:              */
/*    0  SAMPLED_IMAGE   (default 16 384 slots, capped by device)     */
/*    1  STORAGE_IMAGE   (default  4 096)                             */
/*    2  SAMPLER         (default    256)                             */
/*    3  STORAGE_BUFFER  (default  4 096)                             */
/*  All bindings carry UPDATE_AFTER_BIND + PARTIALLY_BOUND +          */
/*  VARIABLE_DESCRIPTOR_COUNT, so registration after draw recording   */
/*  is legal and unused slots cost nothing.                           */
/*                                                                    */
/*  Slot allocator: per-binding free-list stack populated with        */
/*  0..capacity-1 at init. Registration pops; release pushes.         */
/* ------------------------------------------------------------------ */

static uint32_t slot_alloc(flux_bindless_pool *p) {
    if (p->free_top == 0)
        return FLUX_BINDLESS_INVALID;
    return p->free_stack[--p->free_top];
}

static void slot_free(flux_bindless_pool *p, uint32_t slot) {
    if (slot >= p->capacity)
        return;
    if (p->free_top >= p->capacity)
        return; /* double-free guard */
    p->free_stack[p->free_top++] = slot;
}

static flux_result init_pool(flux_bindless_pool *p, uint32_t capacity) {
    p->capacity = capacity;
    p->free_stack = calloc(capacity, sizeof(uint32_t));
    if (!p->free_stack)
        return FLUX_ERROR_OUT_OF_MEMORY;
    /* Push 0..capacity-1 so slot 0 is allocated first. */
    for (uint32_t i = 0; i < capacity; ++i)
        p->free_stack[capacity - 1 - i] = i;
    p->free_top = capacity;
    return FLUX_OK;
}

static uint32_t cap_for_binding(flux_device *d, uint32_t binding, uint32_t want) {
    const VkPhysicalDeviceDescriptorIndexingProperties *di = &d->descriptor_indexing_props;
    uint32_t hw = 0;
    switch (binding) {
    case FLUX_BINDLESS_BIND_SAMPLED_IMAGE:
        hw = di->maxPerStageDescriptorUpdateAfterBindSampledImages;
        break;
    case FLUX_BINDLESS_BIND_STORAGE_IMAGE:
        hw = di->maxPerStageDescriptorUpdateAfterBindStorageImages;
        break;
    case FLUX_BINDLESS_BIND_SAMPLER:
        hw = di->maxPerStageDescriptorUpdateAfterBindSamplers;
        break;
    case FLUX_BINDLESS_BIND_STORAGE_BUFFER:
        hw = di->maxPerStageDescriptorUpdateAfterBindStorageBuffers;
        break;
    }
    if (hw == 0)
        hw = want;
    return want < hw ? want : hw;
}

/* Handle layout: top 4 bits = binding index (0..15), low 28 bits = slot index. */
#define FLUX_BL_BIND_SHIFT 28u
#define FLUX_BL_SLOT_MASK ((1u << FLUX_BL_BIND_SHIFT) - 1u)

static flux_bindless_handle pack_handle(uint32_t binding, uint32_t slot) {
    if (slot > FLUX_BL_SLOT_MASK)
        return FLUX_BINDLESS_INVALID;
    return (binding << FLUX_BL_BIND_SHIFT) | slot;
}
static uint32_t handle_binding(flux_bindless_handle h) {
    return h >> FLUX_BL_BIND_SHIFT;
}
static uint32_t handle_slot(flux_bindless_handle h) {
    return h & FLUX_BL_SLOT_MASK;
}

flux_result flux_bindless_heap_init(flux_device *d) {
    flux_bindless_heap *h = &d->bindless;

    const uint32_t defaults[FLUX_BINDLESS_BINDINGS] = {
        [FLUX_BINDLESS_BIND_SAMPLED_IMAGE] = 16384,
        [FLUX_BINDLESS_BIND_STORAGE_IMAGE] = 4096,
        [FLUX_BINDLESS_BIND_SAMPLER] = 256,
        [FLUX_BINDLESS_BIND_STORAGE_BUFFER] = 4096,
    };
    uint32_t caps[FLUX_BINDLESS_BINDINGS];
    for (uint32_t i = 0; i < FLUX_BINDLESS_BINDINGS; ++i)
        caps[i] = cap_for_binding(d, i, defaults[i]);

    VkDescriptorType types[FLUX_BINDLESS_BINDINGS] = {
        [FLUX_BINDLESS_BIND_SAMPLED_IMAGE] = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
        [FLUX_BINDLESS_BIND_STORAGE_IMAGE] = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        [FLUX_BINDLESS_BIND_SAMPLER] = VK_DESCRIPTOR_TYPE_SAMPLER,
        [FLUX_BINDLESS_BIND_STORAGE_BUFFER] = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    };

    VkDescriptorSetLayoutBinding bindings[FLUX_BINDLESS_BINDINGS];
    VkDescriptorBindingFlags flags[FLUX_BINDLESS_BINDINGS];
    VkDescriptorPoolSize pool_sz[FLUX_BINDLESS_BINDINGS];
    for (uint32_t i = 0; i < FLUX_BINDLESS_BINDINGS; ++i) {
        bindings[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = types[i],
            .descriptorCount = caps[i],
            .stageFlags = VK_SHADER_STAGE_ALL,
        };
        flags[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                   VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        pool_sz[i] = (VkDescriptorPoolSize){.type = types[i], .descriptorCount = caps[i]};
    }
    /* Only the last binding may carry VARIABLE_DESCRIPTOR_COUNT;
     * leaving it off all bindings still works — the set has fixed
     * capacity that we allocate from. */

    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_ci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
        .bindingCount = FLUX_BINDLESS_BINDINGS,
        .pBindingFlags = flags,
    };
    VkDescriptorSetLayoutCreateInfo dsli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = &flags_ci,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = FLUX_BINDLESS_BINDINGS,
        .pBindings = bindings,
    };
    VkResult vr = vkCreateDescriptorSetLayout(d->device, &dsli, nullptr, &h->layout);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "bindless DescriptorSetLayout failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
        .maxSets = 1,
        .poolSizeCount = FLUX_BINDLESS_BINDINGS,
        .pPoolSizes = pool_sz,
    };
    vr = vkCreateDescriptorPool(d->device, &dpci, nullptr, &h->pool);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "bindless DescriptorPool failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = h->pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &h->layout,
    };
    vr = vkAllocateDescriptorSets(d->device, &dsai, &h->set);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "bindless AllocateDescriptorSets failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    for (uint32_t i = 0; i < FLUX_BINDLESS_BINDINGS; ++i) {
        flux_result r = init_pool(&h->pools[i], caps[i]);
        if (r != FLUX_OK)
            return r;
    }
    pthread_mutex_init(&h->lock, nullptr);
    h->lock_initialized = true;
    return FLUX_OK;
}

void flux_bindless_heap_destroy(flux_device *d) {
    flux_bindless_heap *h = &d->bindless;
    if (!d->device)
        return;
    if (h->lock_initialized)
        pthread_mutex_destroy(&h->lock);
    if (h->pool)
        vkDestroyDescriptorPool(d->device, h->pool, nullptr);
    if (h->layout)
        vkDestroyDescriptorSetLayout(d->device, h->layout, nullptr);
    for (uint32_t i = 0; i < FLUX_BINDLESS_BINDINGS; ++i)
        free(h->pools[i].free_stack);
    memset(h, 0, sizeof(*h));
}

static flux_result write_image(flux_device *d, uint32_t binding, VkImageView view,
                               VkImageLayout layout, VkSampler sampler, flux_bindless_handle *out) {
    if (!d || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    /* Writing a null handle into a descriptor is undefined behavior
     * per spec (VUID-VkWriteDescriptorSet-descriptorType-00325/-00326)
     * — ANV happens to tolerate it; lavapipe crashes. Reject it here
     * so the error is defined and driver-independent. */
    if (binding == FLUX_BINDLESS_BIND_SAMPLER ? sampler == VK_NULL_HANDLE
                                              : view == VK_NULL_HANDLE) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "bindless register: null handle");
        *out = FLUX_BINDLESS_INVALID;
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&d->bindless.lock);

    flux_bindless_pool *p = &d->bindless.pools[binding];
    uint32_t slot = slot_alloc(p);
    if (slot == FLUX_BINDLESS_INVALID) {
        pthread_mutex_unlock(&d->bindless.lock);
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "bindless heap binding exhausted");
        *out = FLUX_BINDLESS_INVALID;
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    VkDescriptorImageInfo dii = {.imageView = view, .imageLayout = layout, .sampler = sampler};
    VkDescriptorType dtype =
        (binding == FLUX_BINDLESS_BIND_SAMPLED_IMAGE)   ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
        : (binding == FLUX_BINDLESS_BIND_STORAGE_IMAGE) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                        : VK_DESCRIPTOR_TYPE_SAMPLER;
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = d->bindless.set,
        .dstBinding = binding,
        .dstArrayElement = slot,
        .descriptorCount = 1,
        .descriptorType = dtype,
        .pImageInfo = &dii,
    };
    /* Vulkan requires external synchronisation on writes to a single
     * descriptor set; the same lock covers our slot allocator and
     * the descriptor write. */
    vkUpdateDescriptorSets(d->device, 1, &w, 0, nullptr);
    pthread_mutex_unlock(&d->bindless.lock);
    *out = pack_handle(binding, slot);
    return FLUX_OK;
}

flux_result flux_bindless_register_image(flux_device *d, VkImageView view, VkImageLayout layout,
                                         flux_bindless_handle *out) {
    return write_image(d, FLUX_BINDLESS_BIND_SAMPLED_IMAGE, view, layout, VK_NULL_HANDLE, out);
}

flux_result flux_bindless_register_storage_image(flux_device *d, VkImageView view,
                                                 VkImageLayout layout, flux_bindless_handle *out) {
    return write_image(d, FLUX_BINDLESS_BIND_STORAGE_IMAGE, view, layout, VK_NULL_HANDLE, out);
}

flux_result flux_bindless_register_sampler(flux_device *d, VkSampler sampler,
                                           flux_bindless_handle *out) {
    return write_image(d, FLUX_BINDLESS_BIND_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED,
                       sampler, out);
}

void flux_bindless_release(flux_device *d, flux_bindless_handle h) {
    if (!d || h == FLUX_BINDLESS_INVALID)
        return;
    uint32_t binding = handle_binding(h);
    uint32_t slot = handle_slot(h);
    if (binding >= FLUX_BINDLESS_BINDINGS)
        return;
    pthread_mutex_lock(&d->bindless.lock);
    slot_free(&d->bindless.pools[binding], slot);
    pthread_mutex_unlock(&d->bindless.lock);
    /* Note: the descriptor write is not cleared. PARTIALLY_BOUND +
     * UPDATE_UNUSED_WHILE_PENDING means the stale descriptor is
     * harmless as long as nothing indexes the freed slot. Callers
     * are responsible for not using a released handle in a shader. */
}

VkDescriptorSet flux_device_bindless_set(flux_device *d) {
    return d ? d->bindless.set : VK_NULL_HANDLE;
}
VkDescriptorSetLayout flux_device_bindless_layout(flux_device *d) {
    return d ? d->bindless.layout : VK_NULL_HANDLE;
}

flux_bindless_handle flux_device_default_sampler_handle(flux_device *d) {
    if (!d)
        return FLUX_BINDLESS_INVALID;
    pthread_mutex_lock(&d->bindless.lock);
    if (d->default_sampler != VK_NULL_HANDLE) {
        flux_bindless_handle h = d->default_sampler_handle;
        pthread_mutex_unlock(&d->bindless.lock);
        return h;
    }
    pthread_mutex_unlock(&d->bindless.lock);

    /* Build outside the lock to avoid blocking other allocations. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = VK_LOD_CLAMP_NONE,
    };
    VkSampler s = VK_NULL_HANDLE;
    if (vkCreateSampler(d->device, &sci, nullptr, &s) != VK_SUCCESS) {
        return FLUX_BINDLESS_INVALID;
    }
    flux_bindless_handle h = FLUX_BINDLESS_INVALID;
    if (flux_bindless_register_sampler(d, s, &h) != FLUX_OK) {
        vkDestroySampler(d->device, s, nullptr);
        return FLUX_BINDLESS_INVALID;
    }

    pthread_mutex_lock(&d->bindless.lock);
    if (d->default_sampler != VK_NULL_HANDLE) {
        /* Lost the race; another thread already built one. */
        pthread_mutex_unlock(&d->bindless.lock);
        flux_bindless_release(d, h);
        vkDestroySampler(d->device, s, nullptr);
        pthread_mutex_lock(&d->bindless.lock);
        h = d->default_sampler_handle;
        pthread_mutex_unlock(&d->bindless.lock);
        return h;
    }
    d->default_sampler = s;
    d->default_sampler_handle = h;
    pthread_mutex_unlock(&d->bindless.lock);
    return h;
}
