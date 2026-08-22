/*
 * flux/compute.h — compute pipelines and dispatch.
 *
 * Pipelines bind the device-wide bindless descriptor set at set=0
 * automatically; per-dispatch state rides
 * push constants or a buffer slice referenced by 64-bit device
 * address.
 *
 * Dispatch takes a raw VkCommandBuffer rather than a flux_frame:
 * compute is useful both inside the frame loop (recorded into the
 * frame's command buffer) and outside it (one-shot transfers,
 * headless workloads). Pull the frame's command buffer via
 * flux_frame_vk_command_buffer() when calling from a frame.
 */

#ifndef FLUX_COMPUTE_H
#define FLUX_COMPUTE_H

#include <flux/core.h>
#include <flux/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct flux_compute_pipeline flux_compute_pipeline;

typedef struct flux_compute_pipeline_desc {
    flux_struct_type type; /* FLUX_TYPE_COMPUTE_PIPELINE_DESC */
    const void *next;
    const uint32_t *spirv; /* SPIR-V code (uint32 words) */
    size_t spirv_word_count;
    const char *entry_point; /* default: "main" */
    uint32_t push_constant_bytes;
} flux_compute_pipeline_desc;

#define FLUX_COMPUTE_PIPELINE_DESC_INIT {.type = FLUX_TYPE_COMPUTE_PIPELINE_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_compute_pipeline_create(
    flux_device *d, const flux_compute_pipeline_desc *desc, flux_compute_pipeline **out);

FLUX_NODISCARD FLUX_API flux_compute_pipeline *
flux_compute_pipeline_retain(flux_compute_pipeline *p);
/* DESTROY-INLINE SEMANTICS — different from every retire-queued resource
 * (image/buffer/mesh/sampler/target), where *_release is safe at any
 * time. Pipelines are NOT deferred: release destroys the VkPipeline
 * inline, and destroying a pipeline a batch in flight still executes is
 * a VUID-vkDestroyPipeline-pipeline-00765 violation. Use
 * flux_pipeline_release_deferred() (below) to express "retire this once
 * in-flight use completes" with the same safety the retire queue gives
 * images — the recommended spelling whenever the pipeline was ever
 * recorded into a frame. */
FLUX_API void flux_compute_pipeline_release(flux_compute_pipeline *p);

/* Deferred-release counterpart: parks the pipeline on the device retire
 * queue and destroys it once every submitted batch has completed. Safe
 * at any time, exactly like flux_image_release. Takes a reference (so
 * caller-owned copies stay valid); the release happens on the device's
 * timeline. `d` must be the owning device. */
FLUX_API void flux_pipeline_release_deferred(flux_device *d, flux_compute_pipeline *p);

FLUX_API VkPipeline flux_compute_pipeline_vk_pipeline(flux_compute_pipeline *p);
FLUX_API VkPipelineLayout flux_compute_pipeline_vk_layout(flux_compute_pipeline *p);

/* Record a compute dispatch into `cmd`. Binds the pipeline, pushes
 * `push_bytes` from `push_constants` to the compute stage at offset 0
 * (caller must have declared a matching push_constant block in the
 * shader), binds the device bindless descriptor set at set=0, and
 * issues vkCmdDispatch. */
FLUX_API void flux_compute_dispatch(VkCommandBuffer cmd, flux_compute_pipeline *pipeline,
                                    const void *push_constants, uint32_t push_bytes,
                                    uint32_t group_x, uint32_t group_y, uint32_t group_z);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_COMPUTE_H */
