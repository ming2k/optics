/*
 * GPU integration: dispatch a compute pipeline against a host-visible
 * storage buffer, read back, assert every output value matches the
 * known pattern. Closest thing to a render-to-image golden test
 * without a swapchain.
 */
#include "test_helpers.h"
#include <flux/compute.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

#include <stdalign.h>
#include <string.h>

alignas(uint32_t) static const unsigned char fill_spv[] = {
#embed "fill_pattern.comp.spv"
};

#define COUNT 1024u
#define WORKGROUP 64u
#define BYTES (COUNT * sizeof(float))

typedef struct push_constants {
    uint64_t out_buffer_address;
    uint32_t count;
    uint32_t _pad;
} push_constants;

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_compute_golden: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    VkDevice vk = flux_device_vk_device(d);
    VkQueue queue = flux_device_vk_graphics_queue(d);
    uint32_t qfam = flux_device_vk_graphics_family(d);

    /* --- buffer with device address --- */
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
        EXPECT(vkCreateBuffer(vk, &bci, nullptr, &buffer) == VK_SUCCESS);

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(vk, buffer, &mr);
        uint32_t mt = test_helpers_find_memory_type(d, mr.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        EXPECT(mt != UINT32_MAX);

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
        EXPECT(vkAllocateMemory(vk, &mai, nullptr, &memory) == VK_SUCCESS);
        vkBindBufferMemory(vk, buffer, memory, 0);
        vkMapMemory(vk, memory, 0, VK_WHOLE_SIZE, 0, &mapped);
        memset(mapped, 0xCD, BYTES); /* poison so the dispatch writes are observable */
    }

    VkBufferDeviceAddressInfo bdai = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .buffer = buffer,
    };
    uint64_t buffer_addr = vkGetBufferDeviceAddress(vk, &bdai);
    EXPECT(buffer_addr != 0);

    /* --- pipeline --- */
    flux_compute_pipeline_desc pdesc = FLUX_COMPUTE_PIPELINE_DESC_INIT;
    pdesc.spirv = (const uint32_t *)fill_spv;
    pdesc.spirv_word_count = sizeof(fill_spv) / sizeof(uint32_t);
    pdesc.entry_point = "main";
    pdesc.push_constant_bytes = sizeof(push_constants);
    flux_compute_pipeline *pipe = nullptr;
    EXPECT(flux_compute_pipeline_create(d, &pdesc, &pipe) == FLUX_OK);
    EXPECT(pipe != nullptr);

    /* --- one-shot command buffer --- */
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfam,
        .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
    };
    vkCreateCommandPool(vk, &pci, nullptr, &pool);

    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    vkAllocateCommandBuffers(vk, &cbai, &cmd);

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

    /* --- assert golden output --- */
    const float *out = mapped;
    for (uint32_t i = 0; i < COUNT; ++i) {
        float want = (float)i * 0.01f;
        EXPECT_NEAR(out[i], want, 1e-3f);
    }

    vkDestroyCommandPool(vk, pool, nullptr);
    flux_compute_pipeline_release(pipe);
    if (mapped)
        vkUnmapMemory(vk, memory);
    if (memory)
        vkFreeMemory(vk, memory, nullptr);
    if (buffer)
        vkDestroyBuffer(vk, buffer, nullptr);
    flux_device_release(d);
    TEST_SUMMARY();
}
