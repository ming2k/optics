/*
 * Real-driver GPU tile-compute rasterizer execution (ADR-0120 Phase B hardware gate).
 *
 * Dispatches canvas_tile_raster.comp directly onto the physical Vulkan device,
 * evaluates exact analytic area coverage at 1x on a high-contrast oblique edge,
 * and asserts that the resulting coverage levels exceed the 16-level ceiling.
 */
#include "test_helpers.h"

#include "../../../libs/flux/src/canvas/canvas_tile.h"
#include <flux/compute.h>
#include <flux/core.h>
#include <flux/flux.h>
#include <flux/vulkan.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas_tile_raster_comp_spv.h"

#define W 128u
#define H 128u

typedef struct push_constants {
    uint64_t seg_buf;
    uint64_t tile_buf;
    uint64_t idx_buf;
    uint64_t cov_buf;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t width;
    uint32_t height;
    uint32_t fill_rule;
    float ox;
    float oy;
} push_constants;

static int count_distinct_levels(const float *cov, uint32_t w, uint32_t h) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    for (uint32_t y = 8; y < 30 && y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            float c = cov[(size_t)y * w + x];
            int b = (int)floorf(c * 255.0f + 0.5f);
            if (b >= 0 && b <= 255)
                seen[b] = true;
        }
    }
    int n = 0;
    for (int i = 0; i < 256; ++i)
        n += seen[i];
    return n;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_tile_gpu_compute: no Vulkan device; skipping\n");
        return 0;
    }

    VkDevice vk = flux_device_vk_device(d);
    VkQueue queue = flux_device_vk_graphics_queue(d);
    uint32_t qfam = flux_device_vk_graphics_family(d);

    /* 1. Oblique edge geometry (matching test_canvas_coverage_ceiling probe) */
    flux_point pts[4] = {
        {0.0f, 6.0f},
        {127.0f, 21.0f},
        {127.0f, 127.0f},
        {0.0f, 127.0f},
    };
    flux_canvas_contour con = {.start = 0, .count = 4, .closed = true};

    flux_tile_segment segs[4];
    flux_rect bounds;
    uint32_t n_segs = canvas_tile_extract_segments(pts, &con, 1, segs, 4, &bounds);
    EXPECT(n_segs == 4);

    uint32_t tx = 0, ty = 0;
    canvas_tile_grid_dims(W, H, &tx, &ty);
    uint32_t total_tiles = tx * ty;

    flux_tile_desc *tiles = calloc(total_tiles, sizeof(flux_tile_desc));
    uint32_t total_indices = 0;
    canvas_tile_bin_segments(segs, n_segs, W, H, tiles, nullptr, 0, &total_indices);

    uint32_t *indices = malloc((size_t)(total_indices + 1) * sizeof(uint32_t));
    EXPECT(tiles && indices);
    if (!tiles || !indices) {
        free(tiles);
        free(indices);
        flux_device_release(d);
        TEST_SUMMARY();
    }

    canvas_tile_bin_segments(segs, n_segs, W, H, tiles, indices, total_indices, &total_indices);
    canvas_tile_compute_backdrop(segs, n_segs, W, H, tiles);

    /* 2. Upload Segments SSBO */
    flux_buffer *seg_buf = nullptr;
    EXPECT(canvas_tile_upload_segments(d, segs, n_segs, FLUX_FILL_NON_ZERO, bounds, &seg_buf) ==
           FLUX_OK);
    uint64_t seg_addr = flux_buffer_device_address(seg_buf) + sizeof(flux_tile_segment_header);

    /* 3. Upload Tiles SSBO */
    flux_buffer_desc tile_bdesc = {
        .type = FLUX_TYPE_BUFFER_DESC,
        .size = total_tiles * sizeof(flux_tile_desc),
        .usage = FLUX_BUFFER_USAGE_STORAGE,
        .location = FLUX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = tiles,
    };
    flux_buffer *tile_buf = nullptr;
    EXPECT(flux_buffer_create(d, &tile_bdesc, &tile_buf) == FLUX_OK);
    uint64_t tile_addr = flux_buffer_device_address(tile_buf);

    /* 4. Upload Indices SSBO */
    flux_buffer_desc idx_bdesc = {
        .type = FLUX_TYPE_BUFFER_DESC,
        .size = (total_indices + 1) * sizeof(uint32_t),
        .usage = FLUX_BUFFER_USAGE_STORAGE,
        .location = FLUX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = indices,
    };
    flux_buffer *idx_buf = nullptr;
    EXPECT(flux_buffer_create(d, &idx_bdesc, &idx_buf) == FLUX_OK);
    uint64_t idx_addr = flux_buffer_device_address(idx_buf);

    /* 5. Destination Coverage SSBO */
    flux_buffer_desc cov_bdesc = {
        .type = FLUX_TYPE_BUFFER_DESC,
        .size = (size_t)W * H * sizeof(float),
        .usage = FLUX_BUFFER_USAGE_STORAGE | FLUX_BUFFER_USAGE_TRANSFER_SRC,
        .location = FLUX_BUFFER_HOST_VISIBLE,
        .device_address = true,
    };
    flux_buffer *cov_buf = nullptr;
    EXPECT(flux_buffer_create(d, &cov_bdesc, &cov_buf) == FLUX_OK);
    uint64_t cov_addr = flux_buffer_device_address(cov_buf);

    /* 6. Create Compute Pipeline */
    flux_compute_pipeline_desc pdesc = {
        .type = FLUX_TYPE_COMPUTE_PIPELINE_DESC,
        .spirv = (const uint32_t *)canvas_tile_raster_comp_spv,
        .spirv_word_count = sizeof(canvas_tile_raster_comp_spv) / sizeof(uint32_t),
        .entry_point = "main",
        .push_constant_bytes = sizeof(push_constants),
    };
    flux_compute_pipeline *pipe = nullptr;
    EXPECT(flux_compute_pipeline_create(d, &pdesc, &pipe) == FLUX_OK);
    EXPECT(pipe != nullptr);
    if (!pipe)
        goto cleanup;

    /* 7. Dispatch on the hardware GPU */
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
        .seg_buf = seg_addr,
        .tile_buf = tile_addr,
        .idx_buf = idx_addr,
        .cov_buf = cov_addr,
        .tiles_x = tx,
        .tiles_y = ty,
        .width = W,
        .height = H,
        .fill_rule = 0u,
        .ox = 0.0f,
        .oy = 0.0f,
    };

    flux_compute_dispatch(cmd, pipe, &pc, sizeof(pc), tx, ty, 1);

    /* Barrier: compute write -> host read */
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
    vkDestroyCommandPool(vk, pool, nullptr);

    /* 8. Read back GPU compute coverage */
    const float *gpu_cov = (const float *)flux_buffer_mapped(cov_buf);
    EXPECT(gpu_cov != nullptr);

    int gpu_levels = count_distinct_levels(gpu_cov, W, H);
    fprintf(stderr,
            "  Real-driver GPU Tile-Compute coverage: %d distinct levels along oblique edge\n",
            gpu_levels);

    /* CONTRACT ASSERTION: Must exceed the 16-level ceiling on actual hardware */
    EXPECT(gpu_levels > 16);
    EXPECT(gpu_levels >= 50);

cleanup:
    if (pipe)
        flux_compute_pipeline_release(pipe);
    if (seg_buf)
        flux_buffer_release(seg_buf);
    if (tile_buf)
        flux_buffer_release(tile_buf);
    if (idx_buf)
        flux_buffer_release(idx_buf);
    if (cov_buf)
        flux_buffer_release(cov_buf);
    free(tiles);
    free(indices);
    flux_device_release(d);
    TEST_SUMMARY();
}
