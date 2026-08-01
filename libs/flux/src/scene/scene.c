/*
 * Scene — Stage 5.
 *
 * camera + mesh + material + draw. Depth attachment is owned by the
 * caller (per the "peer-defined attachments" tenet from ADR-0001):
 * scene_draw assumes the frame's current pass has a depth attachment
 * matching material.depth_format.
 */
#include "../core/internal.h"
#include <flux/scene.h>
#include <flux/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

alignas(uint32_t) static const unsigned char scene_unlit_vert_spv[] = {
#embed "scene_unlit.vert.spv"
};
alignas(uint32_t) static const unsigned char scene_unlit_frag_spv[] = {
#embed "scene_unlit.frag.spv"
};
alignas(uint32_t) static const unsigned char scene_phong_vert_spv[] = {
#embed "scene_phong.vert.spv"
};
alignas(uint32_t) static const unsigned char scene_phong_frag_spv[] = {
#embed "scene_phong.frag.spv"
};
alignas(uint32_t) static const unsigned char scene_unlit_skin_vert_spv[] = {
#embed "scene_unlit_skin.vert.spv"
};
alignas(uint32_t) static const unsigned char scene_phong_skin_vert_spv[] = {
#embed "scene_phong_skin.vert.spv"
};

/* ================================================================== */
/*  Camera                                                            */
/* ================================================================== */

void flux_camera_perspective(flux_camera *cam, float fov_y_rad, float aspect, float z_near,
                             float z_far) {
    if (!cam)
        return;
    cam->view = flux_mat4_identity();
    cam->projection = flux_mat4_perspective(fov_y_rad, aspect, z_near, z_far);
}

void flux_camera_look_at(flux_camera *cam, flux_vec3 eye, flux_vec3 center, flux_vec3 up) {
    if (!cam)
        return;
    cam->view = flux_mat4_look_at(eye, center, up);
}

/* ================================================================== */
/*  Mesh                                                              */
/* ================================================================== */

struct flux_mesh {
    atomic_uint ref_count;
    flux_device *device;

    VkBuffer vertex_buffer;
    flux_vk_alloc vertex_alloc;
    uint32_t vertex_count;

    VkBuffer index_buffer; /* VK_NULL_HANDLE if non-indexed */
    flux_vk_alloc index_alloc;
    uint32_t index_count;

    VkBuffer skin_buffer; /* VK_NULL_HANDLE for a static mesh */
    flux_vk_alloc skin_alloc;
};

flux_result flux_mesh_create(flux_device *d, const flux_mesh_desc *desc, flux_mesh **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_MESH_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_MESH_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->vertices || desc->vertex_count == 0) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "mesh has no vertices");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    const flux_mesh_skin_desc *skin = desc->next;
    if (skin && (skin->type != FLUX_TYPE_MESH_SKIN_DESC || skin->next || !skin->vertices)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "invalid flux_mesh_skin_desc");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_mesh *m = flux_internal_alloc(d, sizeof(*m));
    if (!m)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(m, 0, sizeof(*m));
    atomic_init(&m->ref_count, 1u);
    m->device = flux_device_retain(d);
    m->vertex_count = desc->vertex_count;
    m->index_count = desc->indices ? desc->index_count : 0;

    VkDeviceSize vbytes = (VkDeviceSize)desc->vertex_count * sizeof(flux_vertex);
    flux_result r = flux_vk_alloc_buffer(
        d, vbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        /*wants_device_address=*/false, &m->vertex_buffer, &m->vertex_alloc);
    if (r != FLUX_OK)
        goto fail;
    r = flux_vk_upload_to_buffer(d, m->vertex_buffer, 0, desc->vertices, vbytes);
    if (r != FLUX_OK)
        goto fail;

    if (m->index_count > 0) {
        VkDeviceSize ibytes = (VkDeviceSize)desc->index_count * sizeof(uint32_t);
        r = flux_vk_alloc_buffer(
            d, ibytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            /*wants_device_address=*/false, &m->index_buffer, &m->index_alloc);
        if (r != FLUX_OK)
            goto fail;
        r = flux_vk_upload_to_buffer(d, m->index_buffer, 0, desc->indices, ibytes);
        if (r != FLUX_OK)
            goto fail;
    }

    if (skin) {
        VkDeviceSize sbytes = (VkDeviceSize)desc->vertex_count * sizeof(flux_skin_vertex);
        r = flux_vk_alloc_buffer(
            d, sbytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            /*wants_device_address=*/false, &m->skin_buffer, &m->skin_alloc);
        if (r != FLUX_OK)
            goto fail;
        r = flux_vk_upload_to_buffer(d, m->skin_buffer, 0, skin->vertices, sbytes);
        if (r != FLUX_OK)
            goto fail;
    }

    *out = m;
    return FLUX_OK;

fail:
    if (m->skin_buffer)
        vkDestroyBuffer(d->device, m->skin_buffer, nullptr);
    if (m->skin_alloc.memory)
        flux_vk_deallocate(d, &m->skin_alloc);
    if (m->index_buffer)
        vkDestroyBuffer(d->device, m->index_buffer, nullptr);
    if (m->index_alloc.memory)
        flux_vk_deallocate(d, &m->index_alloc);
    if (m->vertex_buffer)
        vkDestroyBuffer(d->device, m->vertex_buffer, nullptr);
    if (m->vertex_alloc.memory)
        flux_vk_deallocate(d, &m->vertex_alloc);
    flux_device_release(d);
    flux_internal_free(d, m);
    return r;
}

flux_mesh *flux_mesh_retain(flux_mesh *m) {
    if (m)
        atomic_fetch_add_explicit(&m->ref_count, 1u, memory_order_relaxed);
    return m;
}

void flux_mesh_release(flux_mesh *m) {
    if (!m)
        return;
    if (atomic_fetch_sub_explicit(&m->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    flux_device *d = m->device;
    /* Same in-flight hazard as flux_buffer_release: the vertex/index
     * buffers may still be bound to batches executing on the graphics
     * queue. Each buffer is parked on the device retire queue on its
     * own; the two zombies share no state, so teardown order is
     * irrelevant and nothing is freed twice. */
    flux_device_retire_buffer(d, m->vertex_buffer, &m->vertex_alloc);
    if (m->index_buffer)
        flux_device_retire_buffer(d, m->index_buffer, &m->index_alloc);
    if (m->skin_buffer)
        flux_device_retire_buffer(d, m->skin_buffer, &m->skin_alloc);
    flux_internal_free(d, m);
    flux_device_release(d);
}

/* ================================================================== */
/*  Material (pipeline holder)                                        */
/* ================================================================== */

/* Default Blinn-Phong specular exponent when desc.shininess <= 0. */
#define SCENE_PHONG_DEFAULT_SHININESS 32.0f

struct flux_material {
    atomic_uint ref_count;
    flux_device *device;
    flux_material_kind kind;
    flux_vec4 base_color;
    float shininess; /* PHONG only */
    float specular;  /* PHONG only */
    VkShaderStageFlags push_stages;
    uint32_t push_bytes;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkPipeline skinned_pipeline;
};

typedef struct scene_push {
    float mvp[16];
    float color[4];
    uint64_t joint_palette_address;
    uint32_t joint_count;
    uint32_t _pad;
} scene_push;

static_assert(sizeof(scene_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "scene_push exceeds FLUX_DEVICE_REQUIRED_PUSH_BYTES");

/* Phong push: MVP plus the buffer-device-address of the per-draw
 * scene_phong_params block in the frame's transient ring. */
typedef struct scene_phong_push {
    float mvp[16];
    uint64_t params_address;
} scene_phong_push;

static_assert(sizeof(scene_phong_push) <= FLUX_DEVICE_REQUIRED_PUSH_BYTES,
              "scene_phong_push exceeds FLUX_DEVICE_REQUIRED_PUSH_BYTES");

/* Per-draw lighting block, std430. Field-for-field mirror of the
 * PhongParams buffer_reference block in scene_phong.vert/.frag. */
typedef struct scene_phong_params {
    float world[16];
    float nrm0[4]; /* normal matrix columns:            */
    float nrm1[4]; /*   transpose(inverse(mat3(world))) */
    float nrm2[4];
    float base_color[4];
    float light_dir_shininess[4]; /* xyz = travel direction, w = exponent */
    float light_color_ambient[4]; /* rgb = light colour, w = ambient      */
    float eye_specular[4];        /* xyz = world eye pos, w = strength    */
    uint64_t joint_palette_address;
    uint32_t joint_count;
    uint32_t _pad;
} scene_phong_params;

static VkShaderModule make_module(VkDevice d, const void *bytes, size_t len) {
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = len,
        .pCode = (const uint32_t *)bytes,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    return vkCreateShaderModule(d, &smci, nullptr, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static flux_result create_material_pipeline(flux_material *mat, VkFormat color_fmt,
                                            VkFormat depth_fmt, bool skinned,
                                            VkPipeline *out_pipeline) {
    VkDevice d = mat->device->device;

    VkResult vr = VK_SUCCESS;
    if (!mat->layout) {
        VkPushConstantRange push = {
            .stageFlags = mat->push_stages,
            .offset = 0,
            .size = mat->push_bytes,
        };
        /* Every pipeline includes the device bindless set at slot 0,
         * even pipelines that don't reach for it. Future materials
         * (textured, PBR) will index it without re-layout. */
        VkDescriptorSetLayout bindless_layout = flux_device_bindless_layout(mat->device);
        VkPipelineLayoutCreateInfo plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = bindless_layout != VK_NULL_HANDLE ? 1u : 0u,
            .pSetLayouts = bindless_layout != VK_NULL_HANDLE ? &bindless_layout : nullptr,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        };
        vr = vkCreatePipelineLayout(d, &plci, nullptr, &mat->layout);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "scene pipeline layout", vr);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
    }

    VkShaderModule vs, fs;
    if (mat->kind == FLUX_MATERIAL_PHONG) {
        vs = skinned
                 ? make_module(d, scene_phong_skin_vert_spv, sizeof(scene_phong_skin_vert_spv))
                 : make_module(d, scene_phong_vert_spv, sizeof(scene_phong_vert_spv));
        fs = make_module(d, scene_phong_frag_spv, sizeof(scene_phong_frag_spv));
    } else {
        vs = skinned
                 ? make_module(d, scene_unlit_skin_vert_spv, sizeof(scene_unlit_skin_vert_spv))
                 : make_module(d, scene_unlit_vert_spv, sizeof(scene_unlit_vert_spv));
        fs = make_module(d, scene_unlit_frag_spv, sizeof(scene_unlit_frag_spv));
    }
    if (!vs || !fs) {
        if (vs)
            vkDestroyShaderModule(d, vs, nullptr);
        if (fs)
            vkDestroyShaderModule(d, fs, nullptr);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "scene shader module");
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

    VkVertexInputBindingDescription vbind[2] = {
        {
            .binding = 0,
            .stride = sizeof(flux_vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        },
        {
            .binding = 1,
            .stride = sizeof(flux_skin_vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        },
    };
    VkVertexInputAttributeDescription vattr[5] = {
        {.location = 0,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(flux_vertex, position)},
        {.location = 1,
         .binding = 0,
         .format = VK_FORMAT_R32G32B32_SFLOAT,
         .offset = offsetof(flux_vertex, normal)},
        {.location = 2,
         .binding = 0,
         .format = VK_FORMAT_R32G32_SFLOAT,
         .offset = offsetof(flux_vertex, uv)},
        {.location = 3,
         .binding = 1,
         .format = VK_FORMAT_R16G16B16A16_UINT,
         .offset = offsetof(flux_skin_vertex, joints)},
        {.location = 4,
         .binding = 1,
         .format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .offset = offsetof(flux_skin_vertex, weights)},
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = skinned ? 2u : 1u,
        .pVertexBindingDescriptions = vbind,
        .vertexAttributeDescriptionCount = skinned ? 5u : 3u,
        .pVertexAttributeDescriptions = vattr,
    };
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
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth_fmt != VK_FORMAT_UNDEFINED,
        .depthWriteEnable = depth_fmt != VK_FORMAT_UNDEFINED,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState ba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
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
        .layout = mat->layout,
    };
    flux_device_vk_pipeline_cache_lock(mat->device);
    vr = vkCreateGraphicsPipelines(d, mat->device->pipeline_cache, 1, &gpci, nullptr, out_pipeline);
    flux_device_vk_pipeline_cache_unlock(mat->device);
    vkDestroyShaderModule(d, vs, nullptr);
    vkDestroyShaderModule(d, fs, nullptr);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "scene material pipeline failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

flux_result flux_material_create(flux_device *d, const flux_material_desc *desc,
                                 flux_material **out) {
    if (!d || !desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_MATERIAL_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_MATERIAL_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->kind != FLUX_MATERIAL_UNLIT && desc->kind != FLUX_MATERIAL_PHONG) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "unknown flux_material_kind");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_format == FLUX_FORMAT_UNDEFINED) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "material color_format is required");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_material *m = flux_internal_alloc(d, sizeof(*m));
    if (!m)
        return FLUX_ERROR_OUT_OF_MEMORY;
    memset(m, 0, sizeof(*m));
    atomic_init(&m->ref_count, 1u);
    m->device = flux_device_retain(d);
    m->kind = desc->kind;
    m->base_color = desc->base_color;
    m->shininess = desc->shininess > 0.0f ? desc->shininess : SCENE_PHONG_DEFAULT_SHININESS;
    m->specular = desc->specular;
    if (desc->kind == FLUX_MATERIAL_PHONG) {
        m->push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        m->push_bytes = sizeof(scene_phong_push);
    } else {
        m->push_stages = VK_SHADER_STAGE_VERTEX_BIT;
        m->push_bytes = sizeof(scene_push);
    }

    VkFormat color_format = flux_format_to_vk(desc->color_format);
    VkFormat depth_format = flux_format_to_vk(desc->depth_format);
    flux_result r = create_material_pipeline(m, color_format, depth_format, false, &m->pipeline);
    if (r == FLUX_OK)
        r = create_material_pipeline(m, color_format, depth_format, true, &m->skinned_pipeline);
    if (r != FLUX_OK) {
        if (m->pipeline)
            vkDestroyPipeline(d->device, m->pipeline, nullptr);
        if (m->layout)
            vkDestroyPipelineLayout(d->device, m->layout, nullptr);
        flux_device_release(d);
        flux_internal_free(d, m);
        return r;
    }
    *out = m;
    return FLUX_OK;
}

flux_material *flux_material_retain(flux_material *m) {
    if (m)
        atomic_fetch_add_explicit(&m->ref_count, 1u, memory_order_relaxed);
    return m;
}

void flux_material_release(flux_material *m) {
    if (!m)
        return;
    if (atomic_fetch_sub_explicit(&m->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    flux_device *d = m->device;
    if (m->pipeline)
        vkDestroyPipeline(d->device, m->pipeline, nullptr);
    if (m->skinned_pipeline)
        vkDestroyPipeline(d->device, m->skinned_pipeline, nullptr);
    if (m->layout)
        vkDestroyPipelineLayout(d->device, m->layout, nullptr);
    flux_internal_free(d, m);
    flux_device_release(d);
}

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

/* Fill the per-draw phong parameter block. The normal matrix is the
 * transpose of the inverse of world's upper-left 3×3 (handles
 * non-uniform scale); the eye position is the translation column of
 * the inverted view matrix, provided by the caller (cached per frame —
 * see scene_cached_view_inv). */
static void fill_phong_params(scene_phong_params *p, const flux_mat4 *view_inv, flux_mat4 world,
                              const flux_material *material, const flux_scene_light *light) {
    static const flux_scene_light default_light = FLUX_SCENE_LIGHT_DEFAULT;
    if (!light)
        light = &default_light;

    memcpy(p->world, world.m, sizeof(p->world));

    /* Column i of transpose(inverse(world)) is row i of inverse(world);
     * flux_mat4 is column-major, so row i reads m[i], m[4+i], m[8+i].
     * The inverse is per-draw on purpose: world differs per mesh, so a
     * cache would not hit. */
    flux_mat4 inv = flux_mat4_invert(world);
    for (int i = 0; i < 3; ++i) {
        float *col = i == 0 ? p->nrm0 : i == 1 ? p->nrm1 : p->nrm2;
        col[0] = inv.m[i];
        col[1] = inv.m[4 + i];
        col[2] = inv.m[8 + i];
        col[3] = 0.0f;
    }

    p->base_color[0] = material->base_color.x;
    p->base_color[1] = material->base_color.y;
    p->base_color[2] = material->base_color.z;
    p->base_color[3] = material->base_color.w;

    flux_vec3 dir = flux_vec3_normalize(light->direction);
    if (flux_vec3_length(dir) == 0.0f)
        dir = flux_vec3_normalize(default_light.direction);
    p->light_dir_shininess[0] = dir.x;
    p->light_dir_shininess[1] = dir.y;
    p->light_dir_shininess[2] = dir.z;
    p->light_dir_shininess[3] = material->shininess;

    p->light_color_ambient[0] = light->color.x;
    p->light_color_ambient[1] = light->color.y;
    p->light_color_ambient[2] = light->color.z;
    p->light_color_ambient[3] = light->ambient;

    p->eye_specular[0] = view_inv->m[12];
    p->eye_specular[1] = view_inv->m[13];
    p->eye_specular[2] = view_inv->m[14];
    p->eye_specular[3] = material->specular;
    p->joint_palette_address = 0;
    p->joint_count = 0;
    p->_pad = 0;
}

/* Inverse of the camera's view matrix, cached on the frame: one memcmp
 * per draw instead of a full 4x4 inverse for the common case of one
 * camera held static across a pass's draws. Bit-identical input yields
 * bit-identical output, so rendering is unchanged. The cache resets
 * with the frame slot every begin_frame. */
static const flux_mat4 *scene_cached_view_inv(flux_frame *f, const flux_camera *cam) {
    if (!f->scene_view_inv_valid ||
        memcmp(f->scene_view_src.m, cam->view.m, sizeof(cam->view.m)) != 0) {
        f->scene_view_src = cam->view;
        f->scene_view_inv = flux_mat4_invert(cam->view);
        f->scene_view_inv_valid = true;
    }
    return &f->scene_view_inv;
}

static void scene_draw(flux_frame *f, const flux_camera *cam, flux_mat4 world, flux_mesh *mesh,
                       flux_material *material, const flux_scene_light *light,
                       const flux_mat4 *joint_matrices, uint32_t joint_count) {
    if (!f || !cam || !mesh || !material)
        return;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    if (!cmd)
        return;

    /* MVP = projection * view * world (column-major, multiplied via
     * flux_mat4_multiply which mirrors math notation a*b). */
    flux_mat4 view_proj = flux_mat4_multiply(cam->projection, cam->view);
    flux_mat4 mvp = flux_mat4_multiply(view_proj, world);
    bool skinned = mesh->skin_buffer && joint_matrices && joint_count > 0;
    uint64_t palette_address = 0;
    if (skinned) {
        size_t palette_bytes = (size_t)joint_count * sizeof(flux_mat4);
        if (joint_count != palette_bytes / sizeof(flux_mat4))
            return;
        flux_transient palette;
        if (flux_frame_alloc_transient(f, palette_bytes, 16, &palette) != FLUX_OK)
            return;
        memcpy(palette.cpu, joint_matrices, palette_bytes);
        palette_address = palette.gpu_address;
    }

    /* Skip redundant rebinds: the frame mirrors the bindings scene
     * last made on this command buffer (reset every begin_frame).
     * Bindings persist across the passes of one command buffer, so the
     * mirror holds for consecutive scene draws — the same dedup the
     * canvas backend applies per pass. Draws recorded by other modules
     * are not mirrored; interleaving another module's graphics pass
     * between scene passes of one frame is outside scene's contract
     * (each scene pass is expected to own its draws). */
    VkPipeline pipeline = skinned ? material->skinned_pipeline : material->pipeline;
    if (f->scene_bound_pipeline != pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        f->scene_bound_pipeline = pipeline;
    }

    /* Bind the device bindless set at slot 0 to match the pipeline
     * layout. Cheap; consistent with canvas and compute. */
    VkDescriptorSet bindless = flux_device_bindless_set(material->device);
    if (bindless != VK_NULL_HANDLE &&
        (f->scene_bound_set != bindless || f->scene_bound_layout != material->layout)) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->layout, 0, 1,
                                &bindless, 0, nullptr);
        f->scene_bound_set = bindless;
        f->scene_bound_layout = material->layout;
    }

    if (material->kind == FLUX_MATERIAL_PHONG) {
        flux_transient slice;
        if (flux_frame_alloc_transient(f, sizeof(scene_phong_params), 16, &slice) != FLUX_OK) {
            return; /* draw dropped; alloc_transient set the error */
        }
        scene_phong_params *params = slice.cpu;
        fill_phong_params(params, scene_cached_view_inv(f, cam), world, material, light);
        params->joint_palette_address = palette_address;
        params->joint_count = skinned ? joint_count : 0;

        scene_phong_push pc;
        memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        pc.params_address = slice.gpu_address;
        vkCmdPushConstants(cmd, material->layout, material->push_stages, 0, sizeof(pc), &pc);
    } else {
        scene_push pc = {0};
        memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        pc.color[0] = material->base_color.x;
        pc.color[1] = material->base_color.y;
        pc.color[2] = material->base_color.z;
        pc.color[3] = material->base_color.w;
        pc.joint_palette_address = palette_address;
        pc.joint_count = skinned ? joint_count : 0;
        vkCmdPushConstants(cmd, material->layout, material->push_stages, 0, sizeof(pc), &pc);
    }

    VkBuffer vertex_buffers[2] = {mesh->vertex_buffer, mesh->skin_buffer};
    VkDeviceSize offsets[2] = {0, 0};
    vkCmdBindVertexBuffers(cmd, 0, skinned ? 2u : 1u, vertex_buffers, offsets);

    if (mesh->index_count > 0 && mesh->index_buffer) {
        vkCmdBindIndexBuffer(cmd, mesh->index_buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, mesh->index_count, 1, 0, 0, 0);
    } else {
        vkCmdDraw(cmd, mesh->vertex_count, 1, 0, 0);
    }
}

void flux_scene_draw_mesh(flux_frame *f, const flux_camera *cam, flux_mat4 world, flux_mesh *mesh,
                          flux_material *material) {
    scene_draw(f, cam, world, mesh, material, nullptr, nullptr, 0);
}

void flux_scene_draw_mesh_lit(flux_frame *f, const flux_camera *cam, flux_mat4 world,
                              flux_mesh *mesh, flux_material *material,
                              const flux_scene_light *light) {
    scene_draw(f, cam, world, mesh, material, light, nullptr, 0);
}

void flux_scene_draw_mesh_skinned(flux_frame *f, const flux_camera *cam, flux_mat4 world,
                                  flux_mesh *mesh, flux_material *material,
                                  const flux_mat4 *joint_matrices, uint32_t joint_count) {
    scene_draw(f, cam, world, mesh, material, nullptr, joint_matrices, joint_count);
}

void flux_scene_draw_mesh_skinned_lit(flux_frame *f, const flux_camera *cam, flux_mat4 world,
                                      flux_mesh *mesh, flux_material *material,
                                      const flux_scene_light *light,
                                      const flux_mat4 *joint_matrices, uint32_t joint_count) {
    scene_draw(f, cam, world, mesh, material, light, joint_matrices, joint_count);
}
