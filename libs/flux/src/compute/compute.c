/*
 * Compute pipelines + dispatch.
 *
 * Pipeline layout includes the device bindless set at descriptor
 * slot 0; pipelines that don't reach for any descriptors still
 * bind it (cheap). Push constants are declared exactly as the
 * caller asked, so the SPIR-V matches.
 */
#include "../core/internal.h"
#include "internal.h"
#include <flux/compute.h>

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

struct flux_compute_pipeline {
    atomic_uint ref_count;
    flux_device *device; /* retained unless device_weak */
    bool device_weak;    /* see flux_compute_pipeline_make_device_weak */

    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t push_bytes;
};

flux_result flux_compute_pipeline_create(flux_device *d, const flux_compute_pipeline_desc *desc,
                                         flux_compute_pipeline **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_COMPUTE_PIPELINE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_COMPUTE_PIPELINE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->spirv || desc->spirv_word_count == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "spirv missing");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    /* Caller-supplied push-constant range must fit on the host. The
     * device-wide check at device creation already covered the
     * library's own pipelines; this guards user-defined sizes. */
    if (desc->push_constant_bytes > d->props.limits.maxPushConstantsSize) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "compute push_constant_bytes exceeds device maxPushConstantsSize");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_compute_pipeline *p = flux_internal_alloc(d, sizeof(*p));
    if (!p)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&p->ref_count, 1u);
    p->device = flux_device_retain(d);
    p->push_bytes = desc->push_constant_bytes;

    /* Pipeline layout: bindless set + caller's push constants. */
    VkDescriptorSetLayout bindless_layout = d->bindless.layout;
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = desc->push_constant_bytes,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &bindless_layout,
        .pushConstantRangeCount = desc->push_constant_bytes > 0 ? 1u : 0u,
        .pPushConstantRanges = desc->push_constant_bytes > 0 ? &push_range : nullptr,
    };
    VkResult vr = vkCreatePipelineLayout(d->device, &plci, nullptr, &p->layout);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreatePipelineLayout failed", vr);
        goto fail;
    }

    /* Shader module. */
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = desc->spirv_word_count * sizeof(uint32_t),
        .pCode = desc->spirv,
    };
    VkShaderModule module = VK_NULL_HANDLE;
    vr = vkCreateShaderModule(d->device, &smci, nullptr, &module);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateShaderModule failed", vr);
        goto fail;
    }

    /* Pipeline. The shader module is owned only for the lifetime of
     * the create call — after vkCreateComputePipelines, the pipeline
     * is the only consumer and the module can be destroyed. */
    VkPipelineShaderStageCreateInfo stage = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = module,
        .pName = desc->entry_point ? desc->entry_point : "main",
    };
    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = stage,
        .layout = p->layout,
    };
    vr = vkCreateComputePipelines(d->device, d->pipeline_cache, 1, &cpci, nullptr, &p->pipeline);
    vkDestroyShaderModule(d->device, module, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateComputePipelines failed", vr);
        goto fail;
    }

    *out = p;
    return FLUX_OK;

fail:
    if (p->layout)
        vkDestroyPipelineLayout(d->device, p->layout, nullptr);
    flux_device_release(p->device);
    flux_internal_free(d, p);
    return FLUX_ERROR_BACKEND_FAILURE;
}

flux_compute_pipeline *flux_compute_pipeline_retain(flux_compute_pipeline *p) {
    if (p)
        atomic_fetch_add_explicit(&p->ref_count, 1u, memory_order_relaxed);
    return p;
}

void flux_compute_pipeline_release(flux_compute_pipeline *p) {
    if (!p)
        return;
    if (atomic_fetch_sub_explicit(&p->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    flux_device *d = p->device;
    bool weak = p->device_weak;
    if (p->pipeline)
        vkDestroyPipeline(d->device, p->pipeline, nullptr);
    if (p->layout)
        vkDestroyPipelineLayout(d->device, p->layout, nullptr);
    flux_internal_free(d, p);
    if (!weak)
        flux_device_release(d);
}

void flux_compute_pipeline_make_device_weak(flux_compute_pipeline *p) {
    if (!p || p->device_weak)
        return;
    p->device_weak = true;
    flux_device_release(p->device);
}

VkPipeline flux_compute_pipeline_vk_pipeline(flux_compute_pipeline *p) {
    return p ? p->pipeline : VK_NULL_HANDLE;
}
VkPipelineLayout flux_compute_pipeline_vk_layout(flux_compute_pipeline *p) {
    return p ? p->layout : VK_NULL_HANDLE;
}

void flux_compute_dispatch(VkCommandBuffer cmd, flux_compute_pipeline *pipeline,
                           const void *push_constants, uint32_t push_bytes, uint32_t group_x,
                           uint32_t group_y, uint32_t group_z) {
    if (!cmd || !pipeline || !pipeline->pipeline)
        return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->pipeline);

    /* Bind the device bindless set. Even pipelines that don't reach
     * for any descriptors still bind it — pipelines that DO reach for
     * it must use set=0, the convention of the heap layout. */
    VkDescriptorSet bindless = pipeline->device->bindless.set;
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline->layout, 0, 1,
                                &bindless, 0, nullptr);
    }

    if (push_constants && push_bytes > 0) {
        if (push_bytes > pipeline->push_bytes) {
            /* Refuse to write past the pipeline's declared range — the
             * shader would read garbage beyond what its push block
             * declared, masking a mismatched build silently. Skip the
             * dispatch entirely and surface the error. */
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                      "flux_compute_dispatch push_bytes exceeds pipeline range");
            return;
        }
        vkCmdPushConstants(cmd, pipeline->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes,
                           push_constants);
    }

    vkCmdDispatch(cmd, group_x, group_y, group_z);
}
