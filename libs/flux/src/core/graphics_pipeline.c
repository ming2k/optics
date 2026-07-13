/*
 * Graphics pipeline. Wraps vkCreateGraphicsPipelines + the boilerplate
 * the user otherwise has to write by hand: shader modules, vertex
 * input layout, blend preset selection, depth/stencil preset
 * selection, dynamic viewport+scissor (set per-draw), dynamic-
 * rendering format info via pNext.
 *
 * Pipeline layout includes the device bindless set at slot 0
 * (matching every other flux pipeline) plus the caller's push range.
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <stdatomic.h>
#include <string.h>

struct flux_graphics_pipeline {
    atomic_uint ref_count;
    flux_device *device;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    uint32_t push_bytes;
};

static VkPrimitiveTopology to_vk_topology(flux_primitive_topology t) {
    switch (t) {
    case FLUX_TOPOLOGY_TRIANGLE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    case FLUX_TOPOLOGY_TRIANGLE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case FLUX_TOPOLOGY_LINE_LIST:
        return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case FLUX_TOPOLOGY_LINE_STRIP:
        return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case FLUX_TOPOLOGY_POINT_LIST:
        return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    }
    return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

static VkCullModeFlags to_vk_cull(flux_cull_mode c) {
    switch (c) {
    case FLUX_CULL_NONE:
        return VK_CULL_MODE_NONE;
    case FLUX_CULL_BACK:
        return VK_CULL_MODE_BACK_BIT;
    case FLUX_CULL_FRONT:
        return VK_CULL_MODE_FRONT_BIT;
    }
    return VK_CULL_MODE_NONE;
}

static VkPipelineColorBlendAttachmentState blend_for(flux_blend_preset p) {
    VkPipelineColorBlendAttachmentState b = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    switch (p) {
    case FLUX_BLEND_PRESET_NONE:
        b.blendEnable = VK_FALSE;
        break;
    case FLUX_BLEND_PRESET_PREMUL:
        /* Porter-Duff source-over, src already premultiplied. */
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.colorBlendOp = VK_BLEND_OP_ADD;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    case FLUX_BLEND_PRESET_ADDITIVE:
        b.blendEnable = VK_TRUE;
        b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        b.colorBlendOp = VK_BLEND_OP_ADD;
        b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        b.alphaBlendOp = VK_BLEND_OP_ADD;
        break;
    }
    return b;
}

flux_result flux_graphics_pipeline_create(flux_device *d, const flux_graphics_pipeline_desc *desc,
                                          flux_graphics_pipeline **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_GRAPHICS_PIPELINE_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_GRAPHICS_PIPELINE_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->vertex_spirv || desc->vertex_spirv_word_count == 0 || !desc->fragment_spirv ||
        desc->fragment_spirv_word_count == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "vertex and fragment SPIR-V both required");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_format == FLUX_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "color_format is required");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->push_constant_bytes > d->props.limits.maxPushConstantsSize) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "graphics push_constant_bytes exceeds device maxPushConstantsSize");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_graphics_pipeline *p = flux_internal_alloc(d, sizeof(*p));
    if (!p)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&p->ref_count, 1u);
    p->device = flux_device_retain(d);
    p->push_bytes = desc->push_constant_bytes;

    /* Pipeline layout: bindless set + caller's push range. */
    VkDescriptorSetLayout bindless_layout = d->bindless.layout;
    VkPushConstantRange push_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = desc->push_constant_bytes,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = bindless_layout != VK_NULL_HANDLE ? 1u : 0u,
        .pSetLayouts = bindless_layout != VK_NULL_HANDLE ? &bindless_layout : nullptr,
        .pushConstantRangeCount = desc->push_constant_bytes > 0 ? 1u : 0u,
        .pPushConstantRanges = desc->push_constant_bytes > 0 ? &push_range : nullptr,
    };
    VkResult vr = vkCreatePipelineLayout(d->device, &plci, nullptr, &p->layout);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreatePipelineLayout failed", vr);
        goto fail;
    }

    /* Shader modules — destroyed after pipeline creation completes. */
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    {
        VkShaderModuleCreateInfo smv = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = desc->vertex_spirv_word_count * sizeof(uint32_t),
            .pCode = desc->vertex_spirv,
        };
        vr = vkCreateShaderModule(d->device, &smv, nullptr, &vs);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vertex vkCreateShaderModule failed", vr);
            goto fail;
        }
        VkShaderModuleCreateInfo smf = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = desc->fragment_spirv_word_count * sizeof(uint32_t),
            .pCode = desc->fragment_spirv,
        };
        vr = vkCreateShaderModule(d->device, &smf, nullptr, &fs);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "fragment vkCreateShaderModule failed", vr);
            vkDestroyShaderModule(d->device, vs, nullptr);
            goto fail;
        }
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = desc->vertex_entry_point ? desc->vertex_entry_point : "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = desc->fragment_entry_point ? desc->fragment_entry_point : "main"},
    };

    /* Vertex input — caller-supplied or empty (BDA-style pulling). */
    VkVertexInputBindingDescription vk_binding = {0};
    VkVertexInputAttributeDescription vk_attrs[16] = {0};
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    if (desc->vertex_binding) {
        const flux_vertex_binding *vb = desc->vertex_binding;
        if (vb->attribute_count > 16) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                      "vertex_binding.attribute_count exceeds internal cap of 16");
            vkDestroyShaderModule(d->device, vs, nullptr);
            vkDestroyShaderModule(d->device, fs, nullptr);
            goto fail;
        }
        vk_binding.binding = 0;
        vk_binding.stride = vb->stride;
        vk_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        for (uint32_t i = 0; i < vb->attribute_count; ++i) {
            vk_attrs[i].location = vb->attributes[i].location;
            vk_attrs[i].binding = 0;
            vk_attrs[i].format = flux_format_to_vk(vb->attributes[i].format);
            vk_attrs[i].offset = vb->attributes[i].offset;
        }
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &vk_binding;
        vi.vertexAttributeDescriptionCount = vb->attribute_count;
        vi.pVertexAttributeDescriptions = vk_attrs;
    }

    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = to_vk_topology(desc->topology),
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = to_vk_cull(desc->cull),
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc->depth != FLUX_DEPTH_NONE,
        .depthWriteEnable = desc->depth == FLUX_DEPTH_TEST_AND_WRITE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState ba = blend_for(desc->blend);
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &ba,
    };
    VkDynamicState dyn_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = sizeof(dyn_states) / sizeof(dyn_states[0]),
        .pDynamicStates = dyn_states,
    };
    VkFormat color_fmt = flux_format_to_vk(desc->color_format);
    VkFormat depth_fmt = flux_format_to_vk(desc->depth_format);
    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_fmt,
        .depthAttachmentFormat = depth_fmt,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &prci,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = p->layout,
    };
    flux_device_vk_pipeline_cache_lock(d);
    vr = vkCreateGraphicsPipelines(d->device, d->pipeline_cache, 1, &gpci, nullptr, &p->pipeline);
    flux_device_vk_pipeline_cache_unlock(d);
    vkDestroyShaderModule(d->device, vs, nullptr);
    vkDestroyShaderModule(d->device, fs, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkCreateGraphicsPipelines failed", vr);
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

flux_graphics_pipeline *flux_graphics_pipeline_retain(flux_graphics_pipeline *p) {
    if (p)
        atomic_fetch_add_explicit(&p->ref_count, 1u, memory_order_relaxed);
    return p;
}

void flux_graphics_pipeline_release(flux_graphics_pipeline *p) {
    if (!p)
        return;
    if (atomic_fetch_sub_explicit(&p->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = p->device;
    if (p->pipeline)
        vkDestroyPipeline(d->device, p->pipeline, nullptr);
    if (p->layout)
        vkDestroyPipelineLayout(d->device, p->layout, nullptr);
    flux_internal_free(d, p);
    flux_device_release(d);
}

VkPipeline flux_graphics_pipeline_vk_pipeline(const flux_graphics_pipeline *p) {
    return p ? p->pipeline : VK_NULL_HANDLE;
}

VkPipelineLayout flux_graphics_pipeline_vk_layout(const flux_graphics_pipeline *p) {
    return p ? p->layout : VK_NULL_HANDLE;
}

void flux_graphics_pipeline_bind(flux_frame *f, flux_graphics_pipeline *p,
                                 const void *push_constants, uint32_t push_bytes) {
    if (!f || !p || !p->pipeline)
        return;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    if (!cmd)
        return;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);

    VkDescriptorSet bindless = p->device->bindless.set;
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &bindless, 0,
                                nullptr);
    }

    if (push_constants && push_bytes > 0) {
        if (push_bytes > p->push_bytes) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                      "flux_graphics_pipeline_bind push_bytes exceeds pipeline range");
            return;
        }
        vkCmdPushConstants(cmd, p->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes,
                           push_constants);
    }
}
