/*
 * Canvas renderer — pipelines, shader modules, draw submission.
 */
#include "../compute/internal.h"
#include "backend.h"
#include "internal.h"

#include <stdalign.h>
#include <string.h>

/* Static guard so any growth of optics_gfx_canvas_push fails to compile if
 * it would exceed the device-wide budget. Bump
 * OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES (and re-evaluate Vulkan-minimum
 * compatibility) when this triggers. */
static_assert(sizeof(optics_gfx_canvas_push) <= OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES,
              "optics_gfx_canvas_push exceeds OPTICS_GFX_DEVICE_REQUIRED_PUSH_BYTES");

/* Shader bytecode embedded via native C23 #embed. */
alignas(uint32_t) static const unsigned char canvas_solid_vert_spv[] = {
#embed "canvas_solid.vert.spv"
};
alignas(uint32_t) static const unsigned char canvas_solid_frag_spv[] = {
#embed "canvas_solid.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_gradient_frag_spv[] = {
#embed "canvas_gradient.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_image_frag_spv[] = {
#embed "canvas_image.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_sdf_frag_spv[] = {
#embed "canvas_sdf.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_glyph_frag_spv[] = {
#embed "canvas_glyph.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_output_vert_spv[] = {
#embed "canvas_output.vert.spv"
};
alignas(uint32_t) static const unsigned char canvas_output_frag_spv[] = {
#embed "canvas_output.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_vector_frag_spv[] = {
#embed "canvas_vector.frag.spv"
};
alignas(uint32_t) static const unsigned char canvas_tile_bin_count_comp_spv[] = {
#embed "canvas_tile_bin_count.comp.spv"
};
alignas(uint32_t) static const unsigned char canvas_tile_bin_scatter_comp_spv[] = {
#embed "canvas_tile_bin_scatter.comp.spv"
};
alignas(uint32_t) static const unsigned char canvas_tile_scan_offsets_comp_spv[] = {
#embed "canvas_tile_scan_offsets.comp.spv"
};
alignas(uint32_t) static const unsigned char canvas_tile_scan_backdrop_comp_spv[] = {
#embed "canvas_tile_scan_backdrop.comp.spv"
};

/* ------------------------------------------------------------------ */
/*  Pipeline                                                          */
/* ------------------------------------------------------------------ */

static VkShaderModule make_module(VkDevice d, const void *bytes, size_t len) {
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = len,
        .pCode = (const uint32_t *)bytes,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(d, &smci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

/* ------------------------------------------------------------------ */
/*  Device-cached pipeline (format, samples, and stencil contract)    */
/*                                                                    */
/*  optics_gfx_canvas_create previously built a fresh VkPipeline per        */
/*  canvas (~ms each). With multiple short-lived canvases (typical    */
/*  for gfx-ui-style UIs) that cost added up. The pipeline depends   */
/*  only on (color_format, samples, stencil contract), so we cache it */
/*  and share it across canvases. Pipelines outlive any individual    */
/*  canvas; the device's canvas_state_destroy hook frees them.        */
/* ------------------------------------------------------------------ */

#define CANVAS_PIPELINE_CACHE_CAP 8
#define CANVAS_BLEND_VARIANT_COUNT 4 /* SRC_OVER, SRC, PLUS, MULTIPLY */

static int canvas_blend_variant(optics_gfx_blend_mode b) {
    if ((unsigned)b < CANVAS_BLEND_VARIANT_COUNT)
        return (int)b;
    return 0; /* unknown blends fall back to SRC_OVER */
}

typedef struct canvas_cache_entry {
    VkFormat color_format; /* VK_FORMAT_UNDEFINED = unused slot */
    VkPipelineLayout layout;
    VkPipeline pipelines[CANVAS_PIPE_COUNT][CANVAS_BLEND_VARIANT_COUNT];
} canvas_cache_entry;

typedef struct canvas_module_state {
    optics_gfx_platform_mutex lock;
    canvas_cache_entry entries[CANVAS_PIPELINE_CACHE_CAP];
    optics_gfx_compute_pipeline *tile_pipes[CANVAS_TILE_PIPE_COUNT];
} canvas_module_state;

static void canvas_state_destroy(optics_gfx_device *d) {
    canvas_module_state *st = d->canvas_state;
    if (!st)
        return;
    for (uint32_t i = 0; i < CANVAS_TILE_PIPE_COUNT; ++i) {
        if (st->tile_pipes[i])
            optics_gfx_compute_pipeline_release(st->tile_pipes[i]);
    }
    for (uint32_t i = 0; i < CANVAS_PIPELINE_CACHE_CAP; ++i) {
        canvas_cache_entry *e = &st->entries[i];
        for (uint32_t k = 0; k < CANVAS_PIPE_COUNT; ++k)
            for (uint32_t b = 0; b < CANVAS_BLEND_VARIANT_COUNT; ++b)
                if (e->pipelines[k][b])
                    vkDestroyPipeline(d->device, e->pipelines[k][b], nullptr);
        if (e->layout)
            vkDestroyPipelineLayout(d->device, e->layout, nullptr);
    }
    optics_gfx_platform_mutex_destroy(&st->lock);
    optics_gfx_internal_free(d, st);
    d->canvas_state = nullptr;
    d->canvas_state_destroy = nullptr;
}

void *canvas_state_get_or_init(optics_gfx_device *d) {
    optics_gfx_platform_mutex_lock(&d->module_state_lock);
    canvas_module_state *published = d->canvas_state;
    optics_gfx_platform_mutex_unlock(&d->module_state_lock);
    if (published)
        return published;

    canvas_module_state *candidate = optics_gfx_internal_alloc(d, sizeof(*candidate));
    if (!candidate)
        return nullptr;
    if (!optics_gfx_platform_mutex_init(&candidate->lock)) {
        optics_gfx_internal_free(d, candidate);
        return nullptr;
    }

    optics_gfx_platform_mutex_lock(&d->module_state_lock);
    if (!d->canvas_state) {
        d->canvas_state = candidate;
        d->canvas_state_destroy = canvas_state_destroy;
        published = candidate;
        candidate = nullptr;
    } else {
        published = d->canvas_state;
    }
    optics_gfx_platform_mutex_unlock(&d->module_state_lock);

    if (candidate) {
        optics_gfx_platform_mutex_destroy(&candidate->lock);
        optics_gfx_internal_free(d, candidate);
    }
    return published;
}

static optics_gfx_result build_canvas_pipeline(optics_gfx_device *device, VkFormat color_format,
                                               canvas_pipe_id id, optics_gfx_blend_mode blend,
                                               VkPipelineLayout layout, VkPipeline *out_pipeline);

static optics_gfx_result build_canvas_layout(optics_gfx_device *device,
                                             VkPipelineLayout *out_layout) {
    /* Pipeline layout shared by every canvas pipeline. Push constants
     * cover the worst case (gradient block — solid only reads the
     * first 24 bytes). Bindless set at slot 0. */
    VkDescriptorSetLayout bindless = device->bindless.layout;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(optics_gfx_canvas_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &bindless,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (vkCreatePipelineLayout(device->device, &plci, nullptr, out_layout) != VK_SUCCESS) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "canvas pipeline layout failed");
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    return OPTICS_GFX_OK;
}

/* Returns the cached layout for this format (always set) plus the
 * pipeline for the requested internal id + blend mode (built lazily).
 * Pipelines are owned by the device. */
optics_gfx_result get_canvas_pipeline_id(optics_gfx_device *device, VkFormat color_format,
                                         canvas_pipe_id id, optics_gfx_blend_mode blend,
                                         VkPipelineLayout *out_layout, VkPipeline *out_pipeline) {
    int blend_variant = canvas_blend_variant(blend);
    if (id < 0 || id >= CANVAS_PIPE_COUNT) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid canvas pipeline id");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    canvas_module_state *st = canvas_state_get_or_init(device);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    optics_gfx_platform_mutex_lock(&st->lock);
    canvas_cache_entry *slot = nullptr;

    /* Locate or open a slot for this format. */
    for (uint32_t i = 0; i < CANVAS_PIPELINE_CACHE_CAP; ++i) {
        if (st->entries[i].color_format == color_format) {
            slot = &st->entries[i];
            break;
        }
    }
    if (!slot) {
        for (uint32_t i = 0; i < CANVAS_PIPELINE_CACHE_CAP; ++i) {
            if (st->entries[i].color_format == VK_FORMAT_UNDEFINED) {
                slot = &st->entries[i];
                break;
            }
        }
        if (!slot) {
            optics_gfx_platform_mutex_unlock(&st->lock);
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE, "canvas pipeline cache full");
            return OPTICS_GFX_ERROR_OUT_OF_RANGE;
        }
        slot->color_format = color_format;
    }

    if (!slot->layout) {
        optics_gfx_result r = build_canvas_layout(device, &slot->layout);
        if (r != OPTICS_GFX_OK) {
            optics_gfx_platform_mutex_unlock(&st->lock);
            return r;
        }
    }
    *out_layout = slot->layout;

    if (!slot->pipelines[id][blend_variant]) {
        optics_gfx_result r = build_canvas_pipeline(device, color_format, id, blend, slot->layout,
                                                    &slot->pipelines[id][blend_variant]);
        if (r != OPTICS_GFX_OK) {
            optics_gfx_platform_mutex_unlock(&st->lock);
            return r;
        }
    }
    *out_pipeline = slot->pipelines[id][blend_variant];
    optics_gfx_platform_mutex_unlock(&st->lock);
    return OPTICS_GFX_OK;
}

optics_gfx_result
canvas_get_tile_compute_pipelines(optics_gfx_device *device,
                                  optics_gfx_compute_pipeline *out[CANVAS_TILE_PIPE_COUNT]) {
    if (!device || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    canvas_module_state *st = canvas_state_get_or_init(device);
    if (!st)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;

    struct {
        const void *spv;
        size_t bytes;
    } defs[CANVAS_TILE_PIPE_COUNT] = {
        [CANVAS_TILE_PIPE_BIN_COUNT] = {canvas_tile_bin_count_comp_spv,
                                        sizeof(canvas_tile_bin_count_comp_spv)},
        [CANVAS_TILE_PIPE_SCAN_OFFSETS] = {canvas_tile_scan_offsets_comp_spv,
                                           sizeof(canvas_tile_scan_offsets_comp_spv)},
        [CANVAS_TILE_PIPE_SCAN_BACKDROP] = {canvas_tile_scan_backdrop_comp_spv,
                                            sizeof(canvas_tile_scan_backdrop_comp_spv)},
        [CANVAS_TILE_PIPE_BIN_SCATTER] = {canvas_tile_bin_scatter_comp_spv,
                                          sizeof(canvas_tile_bin_scatter_comp_spv)},
    };

    optics_gfx_platform_mutex_lock(&st->lock);
    for (uint32_t i = 0; i < CANVAS_TILE_PIPE_COUNT; ++i) {
        if (!st->tile_pipes[i]) {
            optics_gfx_compute_pipeline_desc desc = {
                .type = OPTICS_GFX_TYPE_COMPUTE_PIPELINE_DESC,
                .spirv = (const uint32_t *)defs[i].spv,
                .spirv_word_count = defs[i].bytes / sizeof(uint32_t),
                .entry_point = "main",
                .push_constant_bytes = sizeof(uint64_t),
            };
            optics_gfx_result r =
                optics_gfx_compute_pipeline_create(device, &desc, &st->tile_pipes[i]);
            if (r != OPTICS_GFX_OK) {
                optics_gfx_platform_mutex_unlock(&st->lock);
                return r;
            }
            optics_gfx_compute_pipeline_make_device_weak(st->tile_pipes[i]);
        }
        out[i] = st->tile_pipes[i];
    }
    optics_gfx_platform_mutex_unlock(&st->lock);
    return OPTICS_GFX_OK;
}

static optics_gfx_result build_canvas_pipeline(optics_gfx_device *device, VkFormat color_format,
                                               canvas_pipe_id id, optics_gfx_blend_mode blend,
                                               VkPipelineLayout layout, VkPipeline *out_pipeline) {
    VkDevice d = device->device;

    const unsigned char *vert_spv = canvas_solid_vert_spv;
    size_t vert_spv_size = sizeof(canvas_solid_vert_spv);
    const unsigned char *frag_spv = canvas_solid_frag_spv;
    size_t frag_spv_size = sizeof(canvas_solid_frag_spv);
    switch (id) {
    case CANVAS_PIPE_GRADIENT:
        frag_spv = canvas_gradient_frag_spv;
        frag_spv_size = sizeof(canvas_gradient_frag_spv);
        break;
    case CANVAS_PIPE_IMAGE:
        frag_spv = canvas_image_frag_spv;
        frag_spv_size = sizeof(canvas_image_frag_spv);
        break;
    case CANVAS_PIPE_GLYPH:
        frag_spv = canvas_glyph_frag_spv;
        frag_spv_size = sizeof(canvas_glyph_frag_spv);
        break;
    case CANVAS_PIPE_SDF:
        frag_spv = canvas_sdf_frag_spv;
        frag_spv_size = sizeof(canvas_sdf_frag_spv);
        break;
    case CANVAS_PIPE_VECTOR:
        frag_spv = canvas_vector_frag_spv;
        frag_spv_size = sizeof(canvas_vector_frag_spv);
        break;
    case CANVAS_PIPE_OUTPUT:
        /* ADR-0069 output transform: fullscreen-triangle vertex stage,
         * no vertex pulling. */
        vert_spv = canvas_output_vert_spv;
        vert_spv_size = sizeof(canvas_output_vert_spv);
        frag_spv = canvas_output_frag_spv;
        frag_spv_size = sizeof(canvas_output_frag_spv);
        break;
    default:
        break; /* CANVAS_PIPE_SOLID uses the solid frag */
    }

    VkShaderModule vs = make_module(d, vert_spv, vert_spv_size);
    VkShaderModule fs = make_module(d, frag_spv, frag_spv_size);
    if (!vs || !fs) {
        if (vs)
            vkDestroyShaderModule(d, vs, nullptr);
        if (fs)
            vkDestroyShaderModule(d, fs, nullptr);
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "canvas shader module failed");
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main"},
    };

    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* Blend state. SRC_OVER is the default premultiplied-over composite;
     * the other modes are the standard canvas / PDF separable blend modes
     * expressed via fixed-function blend (per ADR-0008 / canvas blend).
     * OUTPUT (ADR-0069) blits the resolved intermediate verbatim; blending
     * must stay off or the destination would bleed through the encode. */
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = id == CANVAS_PIPE_OUTPUT ? VK_FALSE : VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    switch (blend) {
    default:
    case OPTICS_GFX_BLEND_SRC_OVER:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case OPTICS_GFX_BLEND_SRC:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case OPTICS_GFX_BLEND_PLUS:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case OPTICS_GFX_BLEND_MULTIPLY:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    }
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

    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
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
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = layout,
    };

    optics_gfx_device_vk_pipeline_cache_lock(device);
    VkResult vr =
        vkCreateGraphicsPipelines(d, device->pipeline_cache, 1, &gpci, nullptr, out_pipeline);
    optics_gfx_device_vk_pipeline_cache_unlock(device);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (vr != VK_SUCCESS) {
        OPTICS_GFX_FAIL_VK(OPTICS_GFX_ERROR_BACKEND_FAILURE, "canvas pipeline failed", vr);
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    return OPTICS_GFX_OK;
}

/* ------------------------------------------------------------------ */
/*  Draw helpers                                                      */
/* ------------------------------------------------------------------ */

void push_vertex(optics_gfx_canvas_vertex *v, optics_gfx_point p, optics_gfx_mat3x2 tx,
                 optics_gfx_color c) {
    optics_gfx_point t = optics_gfx_mat3x2_transform_point(tx, p);
    v->pos[0] = t.x;
    v->pos[1] = t.y;
    v->color = c;
    v->_pad = 0;
}

/* Fill a optics_gfx_canvas_push from the active paint. For solid paints
 * the gradient fields are zeroed (the solid shader ignores them).
 * For gradients, copies endpoint + stops, converting the parameters
 * into framebuffer pixel space with the current transform — the
 * shaders evaluate gradients per fragment in pixel space, matching
 * the SDF path's draw_sdf_rrect handling. */
void build_push(optics_gfx_canvas *c, const canvas_paint *paint, optics_gfx_canvas_push *out) {
    *out = (optics_gfx_canvas_push){
        .inv_window_size = {2.0f / (float)c->fb_width, 2.0f / (float)c->fb_height},
        .kind = (uint32_t)(paint ? paint->kind : CANVAS_PAINT_SOLID),
    };
    if (!paint)
        return;

    const optics_gfx_gradient_stops *stops = nullptr;
    optics_gfx_point from = {0, 0};
    optics_gfx_point to = {0, 0};
    float radius = 0.0f;
    const optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    const float pixel_scale = optics_gfx_canvas_mat3x2_pixel_scale(tx);
    switch (paint->kind) {
    case CANVAS_PAINT_SOLID:
        return;
    case CANVAS_PAINT_LINEAR_GRADIENT:
        from = optics_gfx_mat3x2_transform_point(tx, paint->gradient.linear.from);
        to = optics_gfx_mat3x2_transform_point(tx, paint->gradient.linear.to);
        stops = &paint->gradient.linear.stops;
        break;
    case CANVAS_PAINT_RADIAL_GRADIENT:
        from = optics_gfx_mat3x2_transform_point(tx, paint->gradient.radial.center);
        radius = paint->gradient.radial.radius * pixel_scale;
        stops = &paint->gradient.radial.stops;
        break;
    case CANVAS_PAINT_IMAGE: {
        optics_gfx_image *img = paint->gradient.image.image;
        optics_gfx_rect dst = paint->gradient.image.dst_rect;
        optics_gfx_rect src = paint->gradient.image.src_rect;
        uint32_t kind = paint->gradient.image.opaque_only ? 6u : 3u;
        out->color_params_address = (img && img->color) ? img->color->color_params_address : 0;
        if (img && img->color && img->color->color_params_address != 0)
            kind |= OPTICS_GFX_CANVAS_PUSH_HAS_COLOR_PARAMS;
        else if (img && (img->format == OPTICS_GFX_FORMAT_RGBA8_UNORM ||
                         img->format == OPTICS_GFX_FORMAT_BGRA8_UNORM ||
                         img->format == OPTICS_GFX_FORMAT_RGB10A2_UNORM))
            kind |= OPTICS_GFX_CANVAS_PUSH_DECODE_SRGB;
        out->kind = kind;
        out->vector.image_handle = img ? img->bindless : 0u;
        out->vector.sampler_handle = paint->gradient.image.sampler_handle;

        float det = tx.m[0] * tx.m[3] - tx.m[2] * tx.m[1];
        if (fabsf(det) > 1e-20f && dst.w > 0.0f && dst.h > 0.0f) {
            float inv_det = 1.0f / det;
            float im0 = tx.m[3] * inv_det;
            float im1 = -tx.m[1] * inv_det;
            float im2 = -tx.m[2] * inv_det;
            float im3 = tx.m[0] * inv_det;
            float im4 = -(im0 * tx.m[4] + im2 * tx.m[5]);
            float im5 = -(im1 * tx.m[4] + im3 * tx.m[5]);
            float sx_w = src.w / dst.w;
            float sy_h = src.h / dst.h;
            out->vector.uv_row0[0] = sx_w * im0;
            out->vector.uv_row0[1] = sx_w * im2;
            out->vector.uv_row1[0] = sy_h * im1;
            out->vector.uv_row1[1] = sy_h * im3;
            out->vector.uv_off[0] = src.x + (im4 - dst.x) * sx_w;
            out->vector.uv_off[1] = src.y + (im5 - dst.y) * sy_h;
        }
        return;
    }
    }

    out->num_stops = stops ? stops->count : 0u;
    if (out->num_stops > OPTICS_GFX_GRADIENT_MAX_STOPS)
        out->num_stops = OPTICS_GFX_GRADIENT_MAX_STOPS;
    out->grad_from[0] = from.x;
    out->grad_from[1] = from.y;
    out->grad_to[0] = to.x;
    out->grad_to[1] = to.y;
    out->grad_radius = radius;
    if (stops) {
        for (uint32_t i = 0; i < out->num_stops; ++i) {
            out->stops[i].t = stops->stops[i].t;
            out->stops[i].color = stops->stops[i].color;
        }
    }
}

/* Bind the pipeline for the internal id lazily; skip the call if
 * it's already bound from a previous draw in the same pass. Delegates
 * to the active backend (the seam where Vulkan binding lives). The
 * blend mode comes from c->pending_blend (set by submit_triangles*
 * from paint). */
bool ensure_pipeline_bound_id(optics_gfx_canvas *c, canvas_pipe_id id) {
    return c->backend->bind_program(c->backend, c, id);
}

bool ensure_pipeline_bound(optics_gfx_canvas *c, canvas_paint_kind kind) {
    canvas_pipe_id id = CANVAS_PIPE_SOLID;
    if ((uint32_t)kind == 0xffu || kind == CANVAS_PAINT_IMAGE)
        id = CANVAS_PIPE_IMAGE;
    else if (kind == CANVAS_PAINT_LINEAR_GRADIENT || kind == CANVAS_PAINT_RADIAL_GRADIENT)
        id = CANVAS_PIPE_GRADIENT;
    return ensure_pipeline_bound_id(c, id);
}

/* Front end for a triangle batch: assemble the backend-neutral push block
 * from the paint and hand the batch to the active backend, which owns vertex
 * transport and the draw itself. The paint's blend mode is forwarded to
 * c->pending_blend so the GPU backend can key into the per-blend pipeline
 * cache; paint == nullptr keeps the current blend (used by the stencil write
 * pass, which ignores blend entirely). */
void submit_triangles_id(optics_gfx_canvas *c, const canvas_paint *paint, canvas_pipe_id id,
                         const optics_gfx_canvas_vertex *verts, uint32_t vertex_count) {
    if (!c->recording || vertex_count == 0)
        return;
    if (paint)
        c->pending_blend = paint->blend;
    optics_gfx_canvas_push pc;
    build_push(c, paint, &pc);
    canvas_emit(c, id, &pc, verts, vertex_count);
}

void submit_triangles(optics_gfx_canvas *c, const canvas_paint *paint,
                      const optics_gfx_canvas_vertex *verts, uint32_t vertex_count) {
    canvas_paint_kind kind = paint ? paint->kind : CANVAS_PAINT_SOLID;
    canvas_pipe_id id = CANVAS_PIPE_SOLID;
    if (kind == CANVAS_PAINT_LINEAR_GRADIENT || kind == CANVAS_PAINT_RADIAL_GRADIENT)
        id = CANVAS_PIPE_GRADIENT;
    submit_triangles_id(c, paint, id, verts, vertex_count);
}
