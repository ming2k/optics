/*
 * compute_fill — headless compute: dispatch a kernel, read results back.
 *
 * Creates a device in headless mode (no window or surface), builds a
 * compute pipeline from the embedded fill.comp SPIR-V, dispatches it to
 * fill a host-visible buffer with i * 0.01, and verifies the readback.
 *
 * Teaches:
 *   - headless device creation (no swapchain)
 *   - building and dispatching a compute pipeline
 *   - buffer-device-address + push-constant marshalling to the kernel
 * Key flux APIs:  flux_compute_pipeline_create, flux_compute_dispatch,
 *                 flux_compute_pipeline_release
 * Plumbing (raw Vulkan, not flux): buffer/memory allocation, the
 *   one-shot command buffer, the compute->host barrier, the fence wait.
 */
#include <flux/compute.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char fill_spv[] = {
#embed "fill.comp.spv"
};

#define COUNT 4096u
#define WORKGROUP 64u
#define BYTES (COUNT * sizeof(float))

typedef struct push_constants {
    uint64_t out_buffer_address;
    uint32_t count;
    uint32_t _pad;
} push_constants;

static uint32_t find_mt(flux_device *d, uint32_t filter, VkMemoryPropertyFlags want) {
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

int main(void) {
    int major, minor, patch;
    flux_version(&major, &minor, &patch);
    printf("flux %d.%d.%d (%s)\n", major, minor, patch, flux_version_string());

    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .headless = true,
        .frames_in_flight = 1,
    };
    flux_device *device = nullptr;
    flux_result r = flux_device_create(&ddesc, &device);
    if (r != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_device_create -> %s\n  %s\n", flux_result_string(r),
                ei.message ? ei.message : "(no info)");
        return (int)r;
    }
    VkDevice vk = flux_device_vk_device(device);
    VkQueue queue = flux_device_vk_graphics_queue(device);
    uint32_t qfam = flux_device_vk_graphics_family(device);

    /* ---- output buffer ---- */
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = nullptr;
    {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = BYTES,
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(vk, &bci, nullptr, &buffer) != VK_SUCCESS)
            goto cleanup;

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk, buffer, &mr);
        uint32_t mt =
            find_mt(device, mr.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt == UINT32_MAX)
            goto cleanup;

        VkMemoryAllocateFlagsInfo afi = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
            .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT,
        };
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = &afi,
            .allocationSize = mr.size,
            .memoryTypeIndex = mt,
        };
        if (vkAllocateMemory(vk, &mai, nullptr, &memory) != VK_SUCCESS)
            goto cleanup;
        vkBindBufferMemory(vk, buffer, memory, 0);
        vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        memset(mapped, 0xCD, BYTES); /* poison so we can see compute write happen */
    }

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    uint64_t buffer_addr = vkGetBufferDeviceAddress(vk, &bdai);
    printf("buffer device address = 0x%lx, %u floats\n", (unsigned long)buffer_addr, COUNT);

    /* ---- pipeline ---- */
    flux_compute_pipeline_desc pdesc = {
        .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
        .spirv = (const uint32_t *)fill_spv,
        .spirv_word_count = sizeof(fill_spv) / sizeof(uint32_t),
        .entry_point = "main",
        .push_constant_bytes = sizeof(push_constants),
    };
    flux_compute_pipeline *pipe = nullptr;
    r = flux_compute_pipeline_create(device, &pdesc, &pipe);
    if (r != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "compute_pipeline_create -> %s\n  %s\n", flux_result_string(r),
                ei.message ? ei.message : "(no info)");
        goto cleanup;
    }
    printf("compute pipeline ready (%zu SPIR-V bytes)\n", sizeof(fill_spv));

    /* ---- one-shot command buffer ---- */
    VkCommandPool pool = VK_NULL_HANDLE;
    {
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = qfam,
            .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
        };
        vkCreateCommandPool(vk, &pci, nullptr, &pool);
    }
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    {
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        vkAllocateCommandBuffers(vk, &cbai, &cmd);
    }
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vkBeginCommandBuffer(cmd, &cbbi);

    push_constants pc = {
        .out_buffer_address = buffer_addr,
        .count = COUNT,
    };
    flux_compute_dispatch(cmd, pipe, &pc, sizeof(pc), (COUNT + WORKGROUP - 1) / WORKGROUP, 1, 1);

    /* Make the host-visible write available to the CPU. */
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
        .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT,
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &di);

    vkEndCommandBuffer(cmd);

    VkCommandBufferSubmitInfo cbsi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cbsi,
    };
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(vk, &fci, nullptr, &fence);
    vkQueueSubmit2(queue, 1, &si, fence);
    vkWaitForFences(vk, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(vk, fence, nullptr);

    /* ---- read back ---- */
    const float *out = mapped;
    bool ok = true;
    for (uint32_t i = 0; i < COUNT; ++i) {
        float want = (float)i * 0.01f;
        float got = out[i];
        if (got < want - 1e-3f || got > want + 1e-3f) {
            fprintf(stderr, "mismatch at %u: got %g, want %g\n", i, got, want);
            ok = false;
            break;
        }
    }
    if (ok) {
        printf("compute output verified — first %g  last %g\n", out[0], out[COUNT - 1]);
    }

    vkDestroyCommandPool(vk, pool, nullptr);
    flux_compute_pipeline_release(pipe);

cleanup:
    if (mapped)
        vkUnmapMemory(vk, memory);
    if (memory)
        vkFreeMemory(vk, memory, nullptr);
    if (buffer)
        vkDestroyBuffer(vk, buffer, nullptr);
    flux_device_release(device);
    return 0;
}
