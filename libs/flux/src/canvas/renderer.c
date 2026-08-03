/*
 * Canvas renderer — pipelines, shader modules, draw submission.
 */
#include "backend.h"
#include "internal.h"

#include <stdalign.h>
#include <string.h>

/* Static guard so any growth of flux_canvas_push fails to compile if
 * it would exceed the device-wide budget. Bump
 * FLUX_DEVICE_REQUIRED_PUSH_BYTES (and re-evaluate Vulkan-minimum
 * compatibility) when this triggers. */
static_assert(sizeof(flux_canvas_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "flux_canvas_push exceeds FLUX_DEVICE_REQUIRED_PUSH_BYTES");

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
/*  flux_canvas_create previously built a fresh VkPipeline per        */
/*  canvas (~ms each). With multiple short-lived canvases (typical    */
/*  for flux-ui-style UIs) that cost added up. The pipeline depends   */
/*  only on (color_format, samples, stencil contract), so we cache it */
/*  and share it across canvases. Pipelines outlive any individual    */
/*  canvas; the device's canvas_state_destroy hook frees them.        */
/* ------------------------------------------------------------------ */

#define CANVAS_PIPELINE_CACHE_CAP 8
#define CANVAS_SAMPLE_VARIANT_COUNT 2
#define CANVAS_STENCIL_VARIANT_COUNT 2
#define CANVAS_BLEND_VARIANT_COUNT 4 /* SRC_OVER, SRC, PLUS, MULTIPLY */

enum canvas_sample_variant {
    CANVAS_SAMPLE_SINGLE = 0,
    CANVAS_SAMPLE_MSAA,
};

enum canvas_stencil_variant {
    CANVAS_NO_STENCIL = 0,
    CANVAS_WITH_STENCIL,
};

static int canvas_sample_variant(VkSampleCountFlagBits samples) {
    if (samples == VK_SAMPLE_COUNT_1_BIT)
        return CANVAS_SAMPLE_SINGLE;
    if (samples == FLUX_CANVAS_SAMPLES)
        return CANVAS_SAMPLE_MSAA;
    return -1;
}

static int canvas_blend_variant(flux_blend_mode b) {
    if ((unsigned)b < CANVAS_BLEND_VARIANT_COUNT)
        return (int)b;
    return 0; /* unknown blends fall back to SRC_OVER */
}

typedef struct canvas_cache_entry {
    VkFormat color_format; /* VK_FORMAT_UNDEFINED = unused slot */
    VkPipelineLayout layout;
    VkPipeline pipelines[CANVAS_STENCIL_VARIANT_COUNT][CANVAS_SAMPLE_VARIANT_COUNT]
                        [CANVAS_PIPE_COUNT][CANVAS_BLEND_VARIANT_COUNT];
} canvas_cache_entry;

typedef struct canvas_module_state {
    pthread_mutex_t lock;
    canvas_cache_entry entries[CANVAS_PIPELINE_CACHE_CAP];
    VkFormat stencil_format; /* probed once; see below */
    bool stencil_format_probed;
} canvas_module_state;

static void canvas_state_destroy(flux_device *d) {
    canvas_module_state *st = d->canvas_state;
    if (!st)
        return;
    for (uint32_t i = 0; i < CANVAS_PIPELINE_CACHE_CAP; ++i) {
        canvas_cache_entry *e = &st->entries[i];
        for (uint32_t stencil = 0; stencil < CANVAS_STENCIL_VARIANT_COUNT; ++stencil)
            for (uint32_t sample = 0; sample < CANVAS_SAMPLE_VARIANT_COUNT; ++sample)
                for (uint32_t k = 0; k < CANVAS_PIPE_COUNT; ++k)
                    for (uint32_t b = 0; b < CANVAS_BLEND_VARIANT_COUNT; ++b)
                        if (e->pipelines[stencil][sample][k][b])
                            vkDestroyPipeline(d->device, e->pipelines[stencil][sample][k][b],
                                              nullptr);
        if (e->layout)
            vkDestroyPipelineLayout(d->device, e->layout, nullptr);
    }
    pthread_mutex_destroy(&st->lock);
    flux_internal_free(d, st);
    d->canvas_state = nullptr;
    d->canvas_state_destroy = nullptr;
}

void *canvas_state_get_or_init(flux_device *d) {
    pthread_mutex_lock(&d->module_state_lock);
    canvas_module_state *published = d->canvas_state;
    pthread_mutex_unlock(&d->module_state_lock);
    if (published)
        return published;

    canvas_module_state *candidate = flux_internal_alloc(d, sizeof(*candidate));
    if (!candidate)
        return nullptr;
    if (pthread_mutex_init(&candidate->lock, nullptr) != 0) {
        flux_internal_free(d, candidate);
        return nullptr;
    }

    pthread_mutex_lock(&d->module_state_lock);
    if (!d->canvas_state) {
        d->canvas_state = candidate;
        d->canvas_state_destroy = canvas_state_destroy;
        published = candidate;
        candidate = nullptr;
    } else {
        published = d->canvas_state;
    }
    pthread_mutex_unlock(&d->module_state_lock);

    if (candidate) {
        pthread_mutex_destroy(&candidate->lock);
        flux_internal_free(d, candidate);
    }
    return published;
}

/* Probe the stencil attachment format once per device. Stencil-capable
 * passes and pipeline variants use this device-wide stable choice;
 * no-stencil variants independently declare VK_FORMAT_UNDEFINED.
 * Caller must hold st->lock. */
static VkFormat stencil_format_locked(flux_device *d, canvas_module_state *st) {
    if (!st->stencil_format_probed) {
        static const VkFormat candidates[] = {
            VK_FORMAT_S8_UINT, /* smallest; stencil-only */
            VK_FORMAT_D24_UNORM_S8_UINT,
            VK_FORMAT_D32_SFLOAT_S8_UINT,
        };
        st->stencil_format = VK_FORMAT_UNDEFINED;
        for (uint32_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(d->physical_device, candidates[i], &fp);
            if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                st->stencil_format = candidates[i];
                break;
            }
        }
        st->stencil_format_probed = true;
    }
    return st->stencil_format;
}

VkFormat flux_canvas_stencil_format(flux_device *d) {
    canvas_module_state *st = canvas_state_get_or_init(d);
    if (!st)
        return VK_FORMAT_UNDEFINED;
    pthread_mutex_lock(&st->lock);
    VkFormat fmt = stencil_format_locked(d, st);
    pthread_mutex_unlock(&st->lock);
    return fmt;
}

static flux_result build_canvas_pipeline(flux_device *device, VkFormat color_format,
                                         VkFormat stencil_format, VkSampleCountFlagBits samples,
                                         canvas_pipe_id id, flux_blend_mode blend,
                                         VkPipelineLayout layout, VkPipeline *out_pipeline);

static flux_result build_canvas_layout(flux_device *device, VkPipelineLayout *out_layout) {
    /* Pipeline layout shared by every canvas pipeline. Push constants
     * cover the worst case (gradient block — solid only reads the
     * first 24 bytes). Bindless set at slot 0. */
    VkDescriptorSetLayout bindless = device->bindless.layout;
    VkPushConstantRange push = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(flux_canvas_push),
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &bindless,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push,
    };
    if (vkCreatePipelineLayout(device->device, &plci, nullptr, out_layout) != VK_SUCCESS) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas pipeline layout failed");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

/* Returns the cached layout for this format (always set) plus the
 * pipeline for the requested internal id + blend mode (built lazily).
 * Pipelines are owned by the device. */
flux_result get_canvas_pipeline_id(flux_device *device, VkFormat color_format,
                                   VkSampleCountFlagBits samples, canvas_pipe_id id,
                                   flux_blend_mode blend, bool with_stencil,
                                   VkPipelineLayout *out_layout, VkPipeline *out_pipeline) {
    int sample_variant = canvas_sample_variant(samples);
    int blend_variant = canvas_blend_variant(blend);
    bool stencil_program = id == CANVAS_PIPE_STENCIL_WRITE || id == CANVAS_PIPE_STENCIL_WRITE_EO ||
                           id == CANVAS_PIPE_COVER_SOLID || id == CANVAS_PIPE_COVER_GRADIENT;
    if (sample_variant < 0 || id < 0 || id >= CANVAS_PIPE_COUNT ||
        (stencil_program && !with_stencil)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "invalid canvas pipeline sample count or id");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    int stencil_variant = with_stencil ? CANVAS_WITH_STENCIL : CANVAS_NO_STENCIL;
    canvas_module_state *st = canvas_state_get_or_init(device);
    if (!st)
        return FLUX_ERROR_OUT_OF_MEMORY;

    pthread_mutex_lock(&st->lock);
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
            pthread_mutex_unlock(&st->lock);
            FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "canvas pipeline cache full");
            return FLUX_ERROR_OUT_OF_RANGE;
        }
        slot->color_format = color_format;
    }

    if (!slot->layout) {
        flux_result r = build_canvas_layout(device, &slot->layout);
        if (r != FLUX_OK) {
            pthread_mutex_unlock(&st->lock);
            return r;
        }
    }
    *out_layout = slot->layout;

    if (!slot->pipelines[stencil_variant][sample_variant][id][blend_variant]) {
        VkFormat stencil_format =
            with_stencil ? stencil_format_locked(device, st) : VK_FORMAT_UNDEFINED;
        if (with_stencil && stencil_format == VK_FORMAT_UNDEFINED) {
            pthread_mutex_unlock(&st->lock);
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED, "canvas stencil format unavailable");
            return FLUX_ERROR_UNSUPPORTED;
        }
        flux_result r = build_canvas_pipeline(
            device, color_format, stencil_format, samples, id, blend, slot->layout,
            &slot->pipelines[stencil_variant][sample_variant][id][blend_variant]);
        if (r != FLUX_OK) {
            pthread_mutex_unlock(&st->lock);
            return r;
        }
    }
    *out_pipeline = slot->pipelines[stencil_variant][sample_variant][id][blend_variant];
    pthread_mutex_unlock(&st->lock);
    return FLUX_OK;
}

/* Paint-kind front end kept for canvas.c call sites. The internal
 * 0xff code selects the image pipeline (images are dispatched via
 * flux_canvas_draw_image, not paint.kind). */
flux_result get_canvas_pipeline(flux_device *device, VkFormat color_format,
                                VkSampleCountFlagBits samples, flux_paint_kind kind,
                                flux_blend_mode blend, bool with_stencil,
                                VkPipelineLayout *out_layout, VkPipeline *out_pipeline) {
    canvas_pipe_id id = CANVAS_PIPE_SOLID;
    if ((uint32_t)kind == 0xffu)
        id = CANVAS_PIPE_IMAGE;
    else if (kind == FLUX_PAINT_LINEAR_GRADIENT || kind == FLUX_PAINT_RADIAL_GRADIENT)
        id = CANVAS_PIPE_GRADIENT;
    return get_canvas_pipeline_id(device, color_format, samples, id, blend, with_stencil,
                                  out_layout, out_pipeline);
}

static flux_result build_canvas_pipeline(flux_device *device, VkFormat color_format,
                                         VkFormat stencil_format, VkSampleCountFlagBits samples,
                                         canvas_pipe_id id, flux_blend_mode blend,
                                         VkPipelineLayout layout, VkPipeline *out_pipeline) {
    VkDevice d = device->device;

    const unsigned char *frag_spv = canvas_solid_frag_spv;
    size_t frag_spv_size = sizeof(canvas_solid_frag_spv);
    switch (id) {
    case CANVAS_PIPE_GRADIENT:
    case CANVAS_PIPE_COVER_GRADIENT:
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
    default:
        break; /* SOLID, COVER_SOLID, STENCIL_WRITE use the solid frag */
    }

    VkShaderModule vs = make_module(d, canvas_solid_vert_spv, sizeof(canvas_solid_vert_spv));
    VkShaderModule fs = make_module(d, frag_spv, frag_spv_size);
    if (!vs || !fs) {
        if (vs)
            vkDestroyShaderModule(d, vs, nullptr);
        if (fs)
            vkDestroyShaderModule(d, fs, nullptr);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas shader module failed");
        return FLUX_ERROR_BACKEND_FAILURE;
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
        .rasterizationSamples = samples,
    };
    /* Blend state. SRC_OVER is the default premultiplied-over composite;
     * the other modes are the standard canvas / PDF separable blend modes
     * expressed via fixed-function blend (per ADR-0008 / canvas blend).
     * STENCIL_WRITE masks all colour channels off — it only touches the
     * stencil attachment — so the blend mode is irrelevant there. */
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_TRUE,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = (id == CANVAS_PIPE_STENCIL_WRITE || id == CANVAS_PIPE_STENCIL_WRITE_EO)
                              ? 0
                              : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    switch (blend) {
    default:
    case FLUX_BLEND_SRC_OVER:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case FLUX_BLEND_SRC:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case FLUX_BLEND_PLUS:
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case FLUX_BLEND_MULTIPLY:
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

    /* Stencil states (ADR-0014). The write pass accumulates nonzero
     * winding: front-facing fan triangles increment, back-facing
     * decrement, both wrapping. The cover pass draws where the count
     * is non-zero and resets it to zero either way, leaving the
     * attachment clean for the next fill in the same pass.
     *
     * The even-odd write variant (CANVAS_PIPE_STENCIL_WRITE_EO) flips
     * the stencil on every covered triangle (INVERT on both faces);
     * after all contours, the stencil is 1 inside odd-overlap regions
     * and 0 inside even-overlap regions, which the SAME cover pass
     * picks up correctly via NOT_EQUAL 0. */
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
    };
    if (id == CANVAS_PIPE_STENCIL_WRITE) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){
            .failOp = VK_STENCIL_OP_KEEP,
            .passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .compareMask = 0xffu,
            .writeMask = 0xffu,
        };
        ds.back = ds.front;
        ds.back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
    } else if (id == CANVAS_PIPE_STENCIL_WRITE_EO) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){
            .failOp = VK_STENCIL_OP_KEEP,
            .passOp = VK_STENCIL_OP_INVERT,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_ALWAYS,
            .compareMask = 0xffu,
            .writeMask = 0xffu,
        };
        ds.back = ds.front;
    } else if (id == CANVAS_PIPE_COVER_SOLID || id == CANVAS_PIPE_COVER_GRADIENT) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front = (VkStencilOpState){
            .failOp = VK_STENCIL_OP_ZERO,
            .passOp = VK_STENCIL_OP_ZERO,
            .depthFailOp = VK_STENCIL_OP_KEEP,
            .compareOp = VK_COMPARE_OP_NOT_EQUAL,
            .compareMask = 0xffu,
            .writeMask = 0xffu,
            .reference = 0u,
        };
        ds.back = ds.front;
    }

    VkPipelineRenderingCreateInfo prci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
        .stencilAttachmentFormat = stencil_format,
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
        .pDepthStencilState = (stencil_format != VK_FORMAT_UNDEFINED) ? &ds : nullptr,
        .pColorBlendState = &cb,
        .pDynamicState = &dyn,
        .layout = layout,
    };

    flux_device_vk_pipeline_cache_lock(device);
    VkResult vr =
        vkCreateGraphicsPipelines(d, device->pipeline_cache, 1, &gpci, nullptr, out_pipeline);
    flux_device_vk_pipeline_cache_unlock(device);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "canvas pipeline failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Draw helpers                                                      */
/* ------------------------------------------------------------------ */

void push_vertex(flux_canvas_vertex *v, flux_point p, flux_mat3x2 tx, flux_color c) {
    flux_point t = flux_mat3x2_transform_point(tx, p);
    v->pos[0] = t.x;
    v->pos[1] = t.y;
    v->color = c;
    v->_pad = 0;
}

/* Fill a flux_canvas_push from the active paint. For solid paints
 * the gradient fields are zeroed (the solid shader ignores them).
 * For gradients, copies endpoint + stops, converting the parameters
 * into framebuffer pixel space with the current transform — the
 * shaders evaluate gradients per fragment in pixel space, matching
 * the SDF path's draw_sdf_rrect handling. */
void build_push(flux_canvas *c, const flux_paint *paint, flux_canvas_push *out) {
    *out = (flux_canvas_push){
        .inv_window_size = {2.0f / (float)c->fb_width, 2.0f / (float)c->fb_height},
        .kind = (uint32_t)(paint ? paint->kind : FLUX_PAINT_SOLID),
    };
    if (!paint)
        return;

    const flux_gradient_stops *stops = nullptr;
    flux_point from = {0, 0};
    flux_point to = {0, 0};
    float radius = 0.0f;
    const flux_mat3x2 tx = c->states[c->state_top].transform;
    const float pixel_scale = flux_canvas_mat3x2_pixel_scale(tx);
    switch (paint->kind) {
    case FLUX_PAINT_SOLID:
        return;
    case FLUX_PAINT_LINEAR_GRADIENT:
        from = flux_mat3x2_transform_point(tx, paint->gradient.linear.from);
        to = flux_mat3x2_transform_point(tx, paint->gradient.linear.to);
        stops = &paint->gradient.linear.stops;
        break;
    case FLUX_PAINT_RADIAL_GRADIENT:
        from = flux_mat3x2_transform_point(tx, paint->gradient.radial.center);
        radius = paint->gradient.radial.radius * pixel_scale;
        stops = &paint->gradient.radial.stops;
        break;
    }

    out->num_stops = stops ? stops->count : 0u;
    if (out->num_stops > FLUX_GRADIENT_MAX_STOPS)
        out->num_stops = FLUX_GRADIENT_MAX_STOPS;
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
bool ensure_pipeline_bound_id(flux_canvas *c, canvas_pipe_id id) {
    return c->backend->bind_program(c->backend, c, id);
}

bool ensure_pipeline_bound(flux_canvas *c, flux_paint_kind kind) {
    canvas_pipe_id id = CANVAS_PIPE_SOLID;
    if ((uint32_t)kind == 0xffu)
        id = CANVAS_PIPE_IMAGE;
    else if (kind == FLUX_PAINT_LINEAR_GRADIENT || kind == FLUX_PAINT_RADIAL_GRADIENT)
        id = CANVAS_PIPE_GRADIENT;
    return ensure_pipeline_bound_id(c, id);
}

/* Front end for a triangle batch: assemble the backend-neutral push block
 * from the paint and hand the batch to the active backend, which owns vertex
 * transport and the draw itself. The paint's blend mode is forwarded to
 * c->pending_blend so the GPU backend can key into the per-blend pipeline
 * cache; paint == nullptr keeps the current blend (used by the stencil write
 * pass, which ignores blend entirely). */
void submit_triangles_id(flux_canvas *c, const flux_paint *paint, canvas_pipe_id id,
                         const flux_canvas_vertex *verts, uint32_t vertex_count) {
    if (!c->recording || vertex_count == 0)
        return;
    if (paint)
        c->pending_blend = paint->blend;
    flux_canvas_push pc;
    build_push(c, paint, &pc);
    canvas_emit(c, id, &pc, verts, vertex_count);
}

void submit_triangles(flux_canvas *c, const flux_paint *paint, const flux_canvas_vertex *verts,
                      uint32_t vertex_count) {
    flux_paint_kind kind = paint ? paint->kind : FLUX_PAINT_SOLID;
    canvas_pipe_id id = CANVAS_PIPE_SOLID;
    if (kind == FLUX_PAINT_LINEAR_GRADIENT || kind == FLUX_PAINT_RADIAL_GRADIENT)
        id = CANVAS_PIPE_GRADIENT;
    submit_triangles_id(c, paint, id, verts, vertex_count);
}

/* Push a triangle into the scratch vertex array. Bounds-checked.
 * On overflow records the failure via FLUX_FAIL so consumers see a
 * non-OK flux_get_last_error after the frame; subsequent draws into
 * the same scratch silently no-op (last-error has already been set). */
void emit_tri(flux_canvas_vertex *verts, uint32_t *count, uint32_t cap, flux_mat3x2 tx,
              flux_color color, flux_point a, flux_point b, flux_point e) {
    if (*count + 3 > cap) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "canvas vertex scratch exhausted "
                                           "(FLUX_CANVAS_PATH_SCRATCH_CAP*3); triangles dropped");
        return;
    }
    push_vertex(&verts[(*count)++], a, tx, color);
    push_vertex(&verts[(*count)++], b, tx, color);
    push_vertex(&verts[(*count)++], e, tx, color);
}
