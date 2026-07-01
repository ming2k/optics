/*
 * Vulkan implementation of the canvas rendering backend (flux_canvas_backend).
 *
 * This owns every GPU touchpoint of the canvas 2D layer: per-canvas state
 * (pipeline layout, colour/stencil formats, MSAA + stencil attachments), the
 * render-pass envelope (barriers, attachments, resolve), scissor, pipeline
 * binding, and the draw tail (transient upload, push constants, vkCmdDraw).
 * The device-level pipeline cache itself lives in renderer.c
 * (get_canvas_pipeline_id); this file consumes it.
 *
 * All state is held in flux_vk_canvas, reached through flux_canvas::backend_data
 * — struct flux_canvas holds no Vulkan types.
 */
#include "backend.h"

#include <flux/vulkan.h>
#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Backend-private per-canvas state                                  */
/* ------------------------------------------------------------------ */

typedef struct flux_vk_canvas {
    VkPipelineLayout layout;   /* borrowed from the device canvas cache */
    VkFormat color_format;     /* surface/target colour format (create time) */
    VkFormat stencil_format;   /* device stencil format, or UNDEFINED */
    VkPipeline bound_pipeline; /* last pipeline bound this pass */

    /* Stencil-then-cover attachment (ADR-0014), sized to the surface and
     * recreated on extent change. UNDEFINED stencil_format => no stencil path. */
    VkImage stencil_image;
    flux_vk_alloc stencil_alloc;
    VkImageView stencil_view;
    VkExtent2D stencil_extent;

    /* Multisample colour target: rendering goes here at FLUX_CANVAS_SAMPLES and
     * resolves to the surface/target image each pass. Surface-sized. */
    VkImage msaa_image;
    flux_vk_alloc msaa_alloc;
    VkImageView msaa_view;
    VkExtent2D msaa_extent;
} flux_vk_canvas;

static inline flux_vk_canvas *vkc(flux_canvas *c) {
    return (flux_vk_canvas *)c->backend_data;
}

/* ------------------------------------------------------------------ */
/*  Owned attachments (stencil + MSAA colour)                         */
/* ------------------------------------------------------------------ */

static void stencil_destroy(flux_canvas *c) {
    flux_vk_canvas *v = vkc(c);
    flux_device *d = c->device;
    if (v->stencil_view)
        vkDestroyImageView(d->device, v->stencil_view, nullptr);
    if (v->stencil_image)
        vkDestroyImage(d->device, v->stencil_image, nullptr);
    if (v->stencil_alloc.memory)
        flux_vk_deallocate(d, &v->stencil_alloc);
    v->stencil_view = VK_NULL_HANDLE;
    v->stencil_image = VK_NULL_HANDLE;
    v->stencil_alloc = (flux_vk_alloc){0};
    v->stencil_extent = (VkExtent2D){0, 0};
}

static bool stencil_ensure(flux_canvas *c, uint32_t w, uint32_t h) {
    flux_vk_canvas *v = vkc(c);
    if (v->stencil_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (v->stencil_view && v->stencil_extent.width == w && v->stencil_extent.height == h)
        return true;

    stencil_destroy(c);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = v->stencil_format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = FLUX_CANVAS_SAMPLES, /* match the MSAA colour target */
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &v->stencil_image,
                            &v->stencil_alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = v->stencil_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = v->stencil_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &v->stencil_view) != VK_SUCCESS) {
        stencil_destroy(c);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas stencil view failed");
        return false;
    }
    v->stencil_extent = (VkExtent2D){w, h};
    return true;
}

static void msaa_destroy(flux_canvas *c) {
    flux_vk_canvas *v = vkc(c);
    flux_device *d = c->device;
    if (v->msaa_view)
        vkDestroyImageView(d->device, v->msaa_view, nullptr);
    if (v->msaa_image)
        vkDestroyImage(d->device, v->msaa_image, nullptr);
    if (v->msaa_alloc.memory)
        flux_vk_deallocate(d, &v->msaa_alloc);
    v->msaa_view = VK_NULL_HANDLE;
    v->msaa_image = VK_NULL_HANDLE;
    v->msaa_alloc = (flux_vk_alloc){0};
    v->msaa_extent = (VkExtent2D){0, 0};
}

static bool msaa_ensure(flux_canvas *c, uint32_t w, uint32_t h) {
    flux_vk_canvas *v = vkc(c);
    if (v->color_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (v->msaa_view && v->msaa_extent.width == w && v->msaa_extent.height == h)
        return true;

    msaa_destroy(c);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = v->color_format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = FLUX_CANVAS_SAMPLES,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* Only ever rendered-to and resolved-from within a pass; never
         * sampled or stored, so it can stay device-local and transient. */
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &v->msaa_image,
                            &v->msaa_alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = v->msaa_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = v->color_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &v->msaa_view) != VK_SUCCESS) {
        msaa_destroy(c);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas msaa view failed");
        return false;
    }
    v->msaa_extent = (VkExtent2D){w, h};
    return true;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

static flux_result vk_canvas_init(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    flux_device *d = c->device;
    flux_vk_canvas *v = flux_internal_alloc(d, sizeof(*v));
    if (!v)
        return FLUX_ERROR_OUT_OF_MEMORY;
    *v = (flux_vk_canvas){0};
    c->backend_data = v;

    v->color_format = flux_surface_vk_format(c->surface);
    v->stencil_format = flux_canvas_stencil_format(d);

    /* Warm the cache (so canvas_create can fail fast on shader/layout
     * problems rather than first-draw). Build every pipeline kind for this
     * surface's colour format up front: the glyph pipeline in particular pulls
     * in SPIR-V JIT work on first use that can take ~0.5-1.5 s on Mesa/Intel,
     * which trips IME watchdogs on the first banner render. Warming here keeps
     * the cost inside canvas_create and turns first-draw into a hot path. */
    for (int id = 0; id < CANVAS_PIPE_COUNT; id++) {
        VkPipeline warm;
        flux_result r =
            get_canvas_pipeline_id(d, v->color_format, (canvas_pipe_id)id, &v->layout, &warm);
        if (r != FLUX_OK) {
            flux_internal_free(d, v);
            c->backend_data = nullptr;
            return r;
        }
    }
    return FLUX_OK;
}

static void vk_canvas_destroy(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    flux_vk_canvas *v = vkc(c);
    if (!v)
        return;
    /* Owned attachments may still be referenced by in-flight frames. */
    if (v->stencil_image || v->msaa_image) {
        flux_device_wait_idle(c->device);
        stencil_destroy(c);
        msaa_destroy(c);
    }
    /* Pipeline + layout are owned by the device's canvas cache; we just drop
     * the borrowed references. */
    flux_internal_free(c->device, v);
    c->backend_data = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

static void unpack_clear(const flux_color *clear, flux_vec4 *out) {
    *out = (flux_vec4){0, 0, 0, 0};
    if (!clear)
        return;
    uint8_t r8, g8, b8, a8;
    flux_color_unpack(*clear, &r8, &g8, &b8, &a8);
    out->x = (float)r8 / 255.0f;
    out->y = (float)g8 / 255.0f;
    out->z = (float)b8 / 255.0f;
    out->w = (float)a8 / 255.0f;
}

static flux_result vk_begin_pass(const flux_canvas_backend *self, flux_canvas *c, flux_frame *f,
                                 flux_image *target, const flux_color *clear) {
    (void)self;
    flux_vk_canvas *v = vkc(c);

    flux_surface_info info;
    flux_surface_get_info(c->surface, &info);
    uint32_t w = target ? target->width : info.width;
    uint32_t h = target ? target->height : info.height;

    if (target) {
        /* v1: the target must match the canvas pipelines' baked-in colour
         * format (cache keyed on colour only) and the surface extent (the
         * pass render area and owned attachments are surface-sized). */
        if (flux_format_to_vk(target->format) != v->color_format) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "target colour format != canvas colour format");
            return FLUX_ERROR_INVALID_ARGUMENT;
        }
        if (target->width != info.width || target->height != info.height) {
            FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "target extent != surface extent (v1)");
            return FLUX_ERROR_INVALID_ARGUMENT;
        }
    }

    flux_vec4 cc;
    unpack_clear(clear, &cc);

    /* Colour attachment. Swapchain: render to MSAA, resolve to the swapchain
     * image (view left NULL for flux_frame to fill). Target: render to MSAA,
     * resolve into the caller's image. */
    flux_pass_attachment att = {
        .load_op = clear ? FLUX_LOAD_CLEAR : FLUX_LOAD_LOAD,
        .store_op = target ? FLUX_STORE_DONT_CARE : FLUX_STORE_STORE,
        .clear_color = cc,
    };
    if (target)
        att.resolve_view = target->view;

    if (target) {
        /* Transition target SHADER_READ_ONLY -> COLOR_ATTACHMENT (resolve dst). */
        VkCommandBuffer pre_cmd = flux_frame_vk_command_buffer(f);
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask =
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = target->image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1,
                                 .layerCount = 1},
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(pre_cmd, &di);
        target->current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    /* Render to the owned multisample colour target and resolve, so vector
     * fills are anti-aliased. */
    if (msaa_ensure(c, w, h)) {
        VkCommandBuffer pre_cmd = flux_frame_vk_command_buffer(f);
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = v->msaa_image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .levelCount = 1,
                                 .layerCount = 1},
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(pre_cmd, &di);

        att.view = v->msaa_view;
        att.format = v->color_format;
        if (!target)
            att.resolve_to_surface = true;
    }

    /* Canvas-owned stencil attachment (ADR-0014). The canvas pipelines all
     * declare this format, so when the device has one the pass must carry it —
     * even if no fill needs the fallback this frame. */
    flux_pass_depth_attachment stencil_att;
    bool has_stencil = stencil_ensure(c, w, h);
    if (has_stencil) {
        VkCommandBuffer pre_cmd = flux_frame_vk_command_buffer(f);
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .image = v->stencil_image,
            .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                                 .levelCount = 1,
                                 .layerCount = 1},
        };
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                               .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
        vkCmdPipelineBarrier2(pre_cmd, &di);

        stencil_att = (flux_pass_depth_attachment){
            .view = v->stencil_view,
            .format = v->stencil_format,
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_DONT_CARE,
            .clear_stencil = 0,
        };
    }

    flux_pass_desc pass = {
        .type = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
        .stencil = has_stencil ? &stencil_att : nullptr,
    };

    flux_frame_begin_pass(f, &pass);
    c->pass_active = true;
    c->target_pass = (target != nullptr);
    c->target = target;
    c->stencil_available = has_stencil;
    c->fb_width = w;
    c->fb_height = h;

    VkViewport vp = {.x = 0.0f,
                     .y = 0.0f,
                     .width = (float)w,
                     .height = (float)h,
                     .minDepth = 0.0f,
                     .maxDepth = 1.0f};
    VkRect2D sc = {.offset = {0, 0}, .extent = {w, h}};
    c->states[0].scissor = (flux_recti){0, 0, w, h};

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    /* Don't pre-bind a pipeline — each draw picks the right one. Bind the
     * bindless set now; it's pipeline-layout-scoped and shared. */
    v->bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bindless = flux_device_bindless_set(c->device);
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 0, 1, &bindless, 0,
                                nullptr);
    }
    return FLUX_OK;
}

static void vk_end_pass(const flux_canvas_backend *self, flux_canvas *c) {
    (void)self;
    if (!c->pass_active)
        return;
    flux_frame_end_pass(c->frame);
    c->pass_active = false;

    if (!c->target)
        return;

    /* Trailing barrier: COLOR_ATTACHMENT -> SHADER_READ_ONLY so a following
     * flux_effect_blur / draw_image needs no caller sync. */
    flux_image *t = c->target;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .image = t->image,
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &di);
    t->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void vk_set_scissor(const flux_canvas_backend *self, flux_canvas *c, flux_recti clip) {
    (void)self;
    if (!c->pass_active)
        return;
    VkRect2D sc = {.offset = {clip.x, clip.y}, .extent = {clip.w, clip.h}};
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

/* ------------------------------------------------------------------ */
/*  Program bind + batch submit                                       */
/* ------------------------------------------------------------------ */

static bool vk_bind_program(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id) {
    (void)self;
    flux_vk_canvas *v = vkc(c);
    VkPipelineLayout layout;
    VkPipeline pipeline;
    if (get_canvas_pipeline_id(c->device, v->color_format, id, &layout, &pipeline) != FLUX_OK)
        return false;
    if (v->bound_pipeline != pipeline) {
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        v->bound_pipeline = pipeline;
    }
    return true;
}

static void vk_submit(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id,
                      const flux_canvas_push *push, const flux_canvas_vertex *verts,
                      uint32_t vertex_count) {
    if (!c->recording || vertex_count == 0)
        return;
    if (!vk_bind_program(self, c, id))
        return;

    /* Vertices travel through the frame's transient ring; the shader reads
     * them by buffer-reference address (push.verts_address). */
    flux_transient slice;
    flux_result r = flux_frame_alloc_transient(c->frame, vertex_count * sizeof(flux_canvas_vertex),
                                               alignof(flux_canvas_vertex), &slice);
    if (r != FLUX_OK) {
        c->dropped_draws++;
        return;
    }
    memcpy(slice.cpu, verts, vertex_count * sizeof(flux_canvas_vertex));

    flux_canvas_push pc = *push;
    pc.verts_address = slice.gpu_address;

    flux_recti clip = c->states[c->state_top].scissor;
    VkRect2D sc = {.offset = {clip.x, clip.y}, .extent = {clip.w, clip.h}};
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, vkc(c)->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

static const flux_canvas_backend vk_backend = {
    .name = "vulkan",
    .canvas_init = vk_canvas_init,
    .canvas_destroy = vk_canvas_destroy,
    .begin_pass = vk_begin_pass,
    .end_pass = vk_end_pass,
    .set_scissor = vk_set_scissor,
    .bind_program = vk_bind_program,
    .submit = vk_submit,
};

const flux_canvas_backend *flux_canvas_backend_vk(void) {
    return &vk_backend;
}
