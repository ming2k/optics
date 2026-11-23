/*
 * gfx/compute.h — compute pipelines and dispatch.
 *
 * Pipelines bind the device-wide bindless descriptor set at set=0
 * automatically; per-dispatch state rides
 * push constants or a buffer slice referenced by 64-bit device
 * address.
 *
 * Dispatch takes a raw VkCommandBuffer rather than a optics_gfx_frame:
 * compute is useful both inside the frame loop (recorded into the
 * frame's command buffer) and outside it (one-shot transfers,
 * headless workloads). Pull the frame's command buffer via
 * optics_gfx_frame_vk_command_buffer() when calling from a frame.
 */

#ifndef OPTICS_GFX_COMPUTE_H
#define OPTICS_GFX_COMPUTE_H

#include <optics/gfx/core.h>
#include <optics/gfx/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct optics_gfx_compute_pipeline optics_gfx_compute_pipeline;

typedef struct optics_gfx_compute_pipeline_desc {
    optics_gfx_struct_type type; /* OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC */
    const void *next;
    const uint32_t *spirv; /* SPIR-V code (uint32 words) */
    size_t spirv_word_count;
    const char *entry_point; /* default: "main" */
    uint32_t push_constant_bytes;
} optics_gfx_compute_pipeline_desc;

#define OPTICS_GFX_COMPUTE_PIPELINE_DESC_INIT {.type = OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_compute_pipeline_create(
    optics_gfx_device *d, const optics_gfx_compute_pipeline_desc *desc,
    optics_gfx_compute_pipeline **out);

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_compute_pipeline *
optics_gfx_compute_pipeline_retain(optics_gfx_compute_pipeline *p);
/* Drop one owned reference. GPU objects retire after submitted use completes.
 * Keep resources alive through submission; raw command buffers follow the
 * same rule. No caller-side device-idle wait is required after submission. */
OPTICS_GFX_API void optics_gfx_compute_pipeline_release(optics_gfx_compute_pipeline *p);

OPTICS_GFX_API VkPipeline optics_gfx_compute_pipeline_vk_pipeline(optics_gfx_compute_pipeline *p);
OPTICS_GFX_API VkPipelineLayout
optics_gfx_compute_pipeline_vk_layout(optics_gfx_compute_pipeline *p);

/* Record a compute dispatch into `cmd`. Binds the pipeline, pushes
 * `push_bytes` from `push_constants` to the compute stage at offset 0
 * (caller must have declared a matching push_constant block in the
 * shader), binds the device bindless descriptor set at set=0, and
 * issues vkCmdDispatch. */
typedef struct optics_gfx_compute_dispatch_opts {
    VkCommandBuffer cmd;
    optics_gfx_compute_pipeline *pipeline;
    const void *push_constants;
    uint32_t push_bytes;
    uint32_t group_x;
    uint32_t group_y;
    uint32_t group_z;
} optics_gfx_compute_dispatch_opts;

OPTICS_GFX_API void optics_gfx_compute_dispatch(const optics_gfx_compute_dispatch_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_GFX_COMPUTE_H */
