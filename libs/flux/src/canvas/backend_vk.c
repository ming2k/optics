/*
 * Vulkan implementation of the canvas rendering backend (flux_canvas_backend).
 *
 * This owns every GPU touchpoint of the canvas 2D layer: per-canvas state
 * (pipeline layout, colour/stencil formats, MSAA + stencil attachments), the
 * render-pass envelope (barriers, attachments, resolve), scissor, pipeline
 * binding, and the draw tail (transient upload, push constants, vkCmdDraw).
 * Consecutive state-identical submits are batched into a single vkCmdDraw;
 * see the batch block in struct flux_vk_canvas.
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

typedef struct canvas_owned_image {
    VkImage image;
    flux_vk_alloc alloc;
    VkImageView view;
    VkExtent2D extent;
    VkFormat format;
    VkImageLayout layout; /* UNDEFINED before first transition */
    uint32_t bindless;    /* FLUX_BINDLESS_INVALID unless registered */
} canvas_owned_image;

typedef struct canvas_attachment_set {
    canvas_owned_image stencil_single;
    canvas_owned_image stencil_msaa;
    canvas_owned_image msaa;
    /* ADR-0069 working-space intermediate: every canvas pass renders into
     * this RGBA16F image in linear light; vk_end_pass transforms it into
     * the destination. Sampled by the output pass, so not transient. */
    canvas_owned_image linear;
} canvas_attachment_set;

#define FLUX_CANVAS_TARGET_ATTACHMENT_CAP 16

typedef struct canvas_target_attachment_entry {
    canvas_attachment_set attachments;
    uint32_t width;
    uint32_t height;
    uint64_t last_used_serial;
} canvas_target_attachment_entry;

typedef struct canvas_target_slot {
    canvas_target_attachment_entry entries[FLUX_CANVAS_TARGET_ATTACHMENT_CAP];
    uint32_t count;
    uint64_t use_counter;
} canvas_target_slot;

typedef struct flux_vk_canvas {
    VkPipelineLayout layout;   /* borrowed from the device canvas cache */
    VkFormat color_format;     /* active surface/target colour format */
    VkFormat stencil_format;   /* device stencil format, or UNDEFINED */
    VkPipeline bound_pipeline; /* last pipeline bound this pass */
    VkSampleCountFlagBits active_samples;
    bool active_stencil; /* active pass really binds a stencil attachment */

    /* Open draw batch. Consecutive submits whose entire draw-visible
     * state (pipeline, scissor, every push-constant byte except the
     * vertex base address) matches — and whose transient slices land
     * back-to-back in the ring — append their vertices to one run and
     * share a single vkCmdDraw, recorded at flush time. The vertex
     * shader pulls verts[gl_VertexIndex] with no per-draw base
     * parameter, so a contiguous run draws identically to the per-
     * submit draws it replaces (same order, same blending).
     * pipeline == VK_NULL_HANDLE means no batch is open. */
    struct {
        VkPipeline pipeline;
        flux_recti scissor;
        flux_canvas_push push; /* verts_address = run base          */
        VkDeviceSize end;      /* gpu address one past the run      */
        uint32_t vertex_count;
    } batch;

    /* Attachments are isolated by frame slot and destination class. A target
     * capture and the final surface pass can have different extents/formats in
     * one command buffer; neither may destroy resources recorded by the other.
     * Target attachments are pooled per slot by dimension to avoid recreation
     * thrashing across multi-target frames (e.g. HUD, panels, backdrop blur). */
    canvas_attachment_set surface_attachments[FLUX_MAX_FRAMES_IN_FLIGHT];
    canvas_target_slot target_slots[FLUX_MAX_FRAMES_IN_FLIGHT];
    canvas_attachment_set *active_attachments;

    /* ADR-0069 output-transform state for the active pass, filled by
     * begin_pass and consumed by end_pass (and by the LOAD seed blit). */
    flux_output_push out_encode;
    flux_output_push out_decode;
    flux_recti out_area;
    uint32_t active_slot;
} flux_vk_canvas;

static inline flux_vk_canvas *vkc(flux_canvas *c) {
    return (flux_vk_canvas *)c->backend_data;
}

/* ------------------------------------------------------------------ */
/*  Owned attachments (stencil + MSAA colour)                         */
/* ------------------------------------------------------------------ */

/* Release an owned attachment through the device retire queue. A previous
 * extent's image may still be referenced by batches in flight on the
 * graphics queue (a resize during recording swaps the attachment while an
 * earlier frame reads it), and flux_canvas_destroy runs while the surface's
 * frames are in flight; destroying the VkImage inline can fault the engine
 * mid-batch. Retiring defers destruction until the queue provably passed
 * every referencing batch — no wait, no stall. */
static void owned_image_destroy(flux_canvas *c, canvas_owned_image *owned) {
    flux_device *d = c->device;
    if (owned->view || owned->image || owned->alloc.memory) {
        uint32_t bindless = owned->bindless;
        flux_device_retire_image(d, owned->view, owned->image, &owned->alloc, VK_NULL_HANDLE, 0,
                                 bindless, FLUX_BINDLESS_INVALID);
    }
    *owned = (canvas_owned_image){0};
    owned->bindless = FLUX_BINDLESS_INVALID;
}

static bool stencil_ensure(flux_canvas *c, canvas_attachment_set *attachments, uint32_t w,
                           uint32_t h, VkSampleCountFlagBits samples, canvas_owned_image **out) {
    flux_vk_canvas *v = vkc(c);
    canvas_owned_image *owned = samples == VK_SAMPLE_COUNT_1_BIT ? &attachments->stencil_single
                                                                 : &attachments->stencil_msaa;
    *out = nullptr;
    if (v->stencil_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (owned->view && owned->extent.width == w && owned->extent.height == h) {
        *out = owned;
        return true;
    }

    owned_image_destroy(c, owned);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = v->stencil_format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &owned->image,
                            &owned->alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = owned->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = v->stencil_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &owned->view) != VK_SUCCESS) {
        owned_image_destroy(c, owned);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas stencil view failed");
        return false;
    }
    owned->extent = (VkExtent2D){w, h};
    *out = owned;
    return true;
}

static void attachments_destroy(flux_canvas *c, canvas_attachment_set *attachments) {
    owned_image_destroy(c, &attachments->stencil_single);
    owned_image_destroy(c, &attachments->stencil_msaa);
    owned_image_destroy(c, &attachments->msaa);
    owned_image_destroy(c, &attachments->linear);
}

/* ADR-0069: the per-slot working-space intermediate. RGBA16F, single
 * sample, sampled by the output pass — hence a bindless handle and a
 * tracked layout (COLOR_ATTACHMENT_OPTIMAL while a pass records into
 * it, SHADER_READ_ONLY_OPTIMAL in between). */
static bool linear_ensure(flux_canvas *c, canvas_attachment_set *attachments, uint32_t w,
                          uint32_t h) {
    canvas_owned_image *owned = &attachments->linear;
    if (w == 0 || h == 0)
        return false;
    if (owned->view && owned->extent.width == w && owned->extent.height == h)
        return true;

    owned_image_destroy(c, owned);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FLUX_CANVAS_LINEAR_FORMAT,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &owned->image,
                            &owned->alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = owned->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = FLUX_CANVAS_LINEAR_FORMAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &owned->view) != VK_SUCCESS) {
        owned_image_destroy(c, owned);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas linear view failed");
        return false;
    }
    if (flux_bindless_register_image(d, owned->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                     &owned->bindless) != FLUX_OK) {
        owned_image_destroy(c, owned);
        return false;
    }
    owned->extent = (VkExtent2D){w, h};
    owned->format = FLUX_CANVAS_LINEAR_FORMAT;
    owned->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static bool msaa_ensure(flux_canvas *c, canvas_attachment_set *attachments, uint32_t w,
                        uint32_t h) {
    flux_vk_canvas *v = vkc(c);
    canvas_owned_image *owned = &attachments->msaa;
    if (v->color_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (owned->view && owned->extent.width == w && owned->extent.height == h &&
        owned->format == FLUX_CANVAS_LINEAR_FORMAT)
        return true;

    owned_image_destroy(c, owned);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = FLUX_CANVAS_LINEAR_FORMAT,
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
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &owned->image,
                            &owned->alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = owned->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = FLUX_CANVAS_LINEAR_FORMAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &owned->view) != VK_SUCCESS) {
        owned_image_destroy(c, owned);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas msaa view failed");
        return false;
    }
    owned->extent = (VkExtent2D){w, h};
    owned->format = FLUX_CANVAS_LINEAR_FORMAT;
    return true;
}

static canvas_attachment_set *target_attachments_get(flux_canvas *c, uint32_t slot, uint32_t w,
                                                     uint32_t h) {
    flux_vk_canvas *v = vkc(c);
    if (slot >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return nullptr;
    canvas_target_slot *ts = &v->target_slots[slot];
    ts->use_counter++;
    uint64_t now = ts->use_counter;

    for (uint32_t i = 0; i < ts->count; ++i) {
        if (ts->entries[i].width == w && ts->entries[i].height == h) {
            ts->entries[i].last_used_serial = now;
            return &ts->entries[i].attachments;
        }
    }

    if (ts->count < FLUX_CANVAS_TARGET_ATTACHMENT_CAP) {
        uint32_t idx = ts->count++;
        canvas_target_attachment_entry *entry = &ts->entries[idx];
        *entry = (canvas_target_attachment_entry){0};
        entry->attachments.linear.bindless = FLUX_BINDLESS_INVALID;
        entry->width = w;
        entry->height = h;
        entry->last_used_serial = now;
        return &entry->attachments;
    }

    uint32_t lru_idx = 0;
    uint64_t oldest = ts->entries[0].last_used_serial;
    for (uint32_t i = 1; i < ts->count; ++i) {
        if (ts->entries[i].last_used_serial < oldest) {
            oldest = ts->entries[i].last_used_serial;
            lru_idx = i;
        }
    }

    canvas_target_attachment_entry *lru = &ts->entries[lru_idx];
    attachments_destroy(c, &lru->attachments);
    *lru = (canvas_target_attachment_entry){0};
    lru->attachments.linear.bindless = FLUX_BINDLESS_INVALID;
    lru->width = w;
    lru->height = h;
    lru->last_used_serial = now;
    return &lru->attachments;
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
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        v->surface_attachments[i].linear.bindless = FLUX_BINDLESS_INVALID;
        v->target_slots[i].count = 0;
        v->target_slots[i].use_counter = 0;
    }

    /* ADR-0069: the working-space intermediate must support blending —
     * without it there is no linear-light canvas on this device. */
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(d->physical_device, FLUX_CANVAS_LINEAR_FORMAT, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                          VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            flux_internal_free(d, v);
            c->backend_data = nullptr;
            FLUX_FAIL(FLUX_ERROR_UNSUPPORTED,
                      "device cannot blend R16G16B16A16_SFLOAT; no linear working space");
            return FLUX_ERROR_UNSUPPORTED;
        }
    }

    /* Warm the cache (so canvas_create can fail fast on shader/layout
     * problems rather than first-draw). Build every draw pipeline kind for
     * the working-space intermediate format up front (ADR-0069: draws no
     * longer target the surface format): the glyph pipeline in particular pulls
     * in SPIR-V JIT work on first use that can take ~0.5-1.5 s on Mesa/Intel,
     * which trips IME watchdogs on the first banner render. Warming here keeps
     * the cost inside canvas_create and turns first-draw into a hot path. */
    static const VkSampleCountFlagBits sample_counts[] = {
        VK_SAMPLE_COUNT_1_BIT,
        FLUX_CANVAS_SAMPLES,
    };
    for (uint32_t sample = 0; sample < sizeof(sample_counts) / sizeof(sample_counts[0]); ++sample) {
        for (int id = 0; id < CANVAS_PIPE_COUNT; id++) {
            if (id == CANVAS_PIPE_OUTPUT)
                continue; /* destination-format keyed; warmed below */
            /* Warm the default SRC_OVER pipeline for each id; non-default
             * blend variants are built lazily on first use. */
            VkPipeline warm;
            if (v->stencil_format != VK_FORMAT_UNDEFINED) {
                flux_result r = get_canvas_pipeline_id(d, FLUX_CANVAS_LINEAR_FORMAT,
                                                       sample_counts[sample], (canvas_pipe_id)id,
                                                       FLUX_BLEND_SRC_OVER, true, &v->layout, &warm);
                if (r != FLUX_OK) {
                    flux_internal_free(d, v);
                    c->backend_data = nullptr;
                    return r;
                }
            }
            bool stencil_program =
                id == CANVAS_PIPE_STENCIL_WRITE || id == CANVAS_PIPE_STENCIL_WRITE_EO ||
                id == CANVAS_PIPE_COVER_SOLID || id == CANVAS_PIPE_COVER_GRADIENT;
            if (!stencil_program) {
                flux_result r = get_canvas_pipeline_id(d, FLUX_CANVAS_LINEAR_FORMAT,
                                                       sample_counts[sample], (canvas_pipe_id)id,
                                                       FLUX_BLEND_SRC_OVER, false, &v->layout,
                                                       &warm);
                if (r != FLUX_OK) {
                    flux_internal_free(d, v);
                    c->backend_data = nullptr;
                    return r;
                }
            }
        }
    }

    /* Warm the output transform for this surface's format (1x, no stencil,
     * blend irrelevant — the builder forces it off). Target-pass output
     * variants stay lazy: their formats differ per caller image. */
    {
        VkPipeline warm;
        flux_result r = get_canvas_pipeline_id(d, v->color_format, VK_SAMPLE_COUNT_1_BIT,
                                               CANVAS_PIPE_OUTPUT, FLUX_BLEND_SRC_OVER, false,
                                               &v->layout, &warm);
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
    /* Owned attachments may still be referenced by in-flight frames, so
     * they go through the device retire queue (deferred destruction) —
     * never a device-wide wait here: stalling the whole GPU at canvas
     * teardown is exactly the pause class ADR-0021/0022 removed. */
    for (uint32_t i = 0; i < FLUX_MAX_FRAMES_IN_FLIGHT; ++i) {
        attachments_destroy(c, &v->surface_attachments[i]);
        canvas_target_slot *ts = &v->target_slots[i];
        for (uint32_t j = 0; j < ts->count; ++j) {
            attachments_destroy(c, &ts->entries[j].attachments);
        }
        ts->count = 0;
    }
    /* Pipeline + layout are owned by the device's canvas cache; we just drop
     * the borrowed references. */
    flux_internal_free(c->device, v);
    c->backend_data = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

/* Defined with the submit path below; end_pass must drain any open
 * vertex batch before closing the render pass. */
static void batch_flush(flux_canvas *c);

static void unpack_clear(const flux_color *clear, flux_vec4 *out) {
    *out = (flux_vec4){0, 0, 0, 0};
    if (!clear)
        return;
    /* ADR-0069: the clear value clears the linear intermediate, so it is
     * decoded to the working space here (premultiplied linear). */
    *out = flux_color_to_linear(*clear);
}

static void image_barrier2(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                           VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
                           VkImageLayout old_layout, VkImageLayout new_layout) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .image = image,
        .subresourceRange = {.aspectMask = aspect, .levelCount = 1, .layerCount = 1},
    };
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                           .imageMemoryBarrierCount = 1,
                           .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &di);
}

/* Colour-aspect shorthand for the common case. */
static void color_barrier2(VkCommandBuffer cmd, VkImage image, VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access, VkImageLayout old_layout,
                           VkImageLayout new_layout) {
    image_barrier2(cmd, image, VK_IMAGE_ASPECT_COLOR_BIT, src_stage, src_access, dst_stage,
                   dst_access, old_layout, new_layout);
}

static void mat3_to_push(const flux_mat3 *m, float out[3][4]) {
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row)
            out[col][row] = m->m[col * 3 + row];
        out[col][3] = 0.0f;
    }
}

/* Record one fullscreen output-transform blit (ADR-0069). `push->flags`
 * picks the direction: encode (intermediate -> destination, the end_pass
 * tail) or decode (destination -> intermediate, the LOAD seed).
 * dest_view == VK_NULL_HANDLE targets the frame's swapchain image. */
static void record_output_blit(flux_canvas *c, VkImageView dest_view, VkFormat dest_format,
                               flux_recti area, const flux_output_push *push) {
    flux_vk_canvas *v = vkc(c);
    VkPipelineLayout layout;
    VkPipeline pipeline;
    if (get_canvas_pipeline_id(c->device, dest_format, VK_SAMPLE_COUNT_1_BIT, CANVAS_PIPE_OUTPUT,
                               FLUX_BLEND_SRC_OVER, false, &layout,
                               &pipeline) != FLUX_OK) {
        if (c->pass_error == FLUX_OK)
            c->pass_error = FLUX_ERROR_BACKEND_FAILURE;
        return;
    }

    flux_pass_attachment att = {
        .view = dest_view,
        .format = dest_format,
        /* Encode preserves the pixels outside `area` (partial redraw);
         * the decode seed overwrites exactly the region the main pass
         * will read, so its load is irrelevant. */
        .load_op = (push->flags & FLUX_OUTPUT_F_DECODE) ? FLUX_LOAD_DONT_CARE : FLUX_LOAD_LOAD,
        .store_op = FLUX_STORE_STORE,
    };
    flux_pass_desc pass = {
        .type = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
        .width = (uint32_t)area.w,
        .height = (uint32_t)area.h,
        .render_offset_x = area.x,
        .render_offset_y = area.y,
    };
    flux_frame_begin_pass(c->frame, &pass);

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet bindless = flux_device_bindless_set(c->device);
    if (bindless != VK_NULL_HANDLE)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &bindless, 0,
                                nullptr);
    VkViewport vp = {.x = 0.0f,
                     .y = 0.0f,
                     .width = (float)c->fb_width,
                     .height = (float)c->fb_height,
                     .minDepth = 0.0f,
                     .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkRect2D sc = {.offset = {area.x, area.y}, .extent = {(uint32_t)area.w, (uint32_t)area.h}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(*push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    c->recorded_draws++;
    flux_frame_end_pass(c->frame);
    v->bound_pipeline = VK_NULL_HANDLE; /* the blit clobbered the canvas binding */
}

static flux_result vk_begin_pass(const flux_canvas_backend *self, flux_canvas *c, flux_frame *f,
                                 flux_image *target, const canvas_pass_config *config) {
    (void)self;
    flux_vk_canvas *v = vkc(c);

    flux_surface_info info;
    flux_surface_get_info(c->surface, &info);
    uint32_t w = target ? target->width : info.width;
    uint32_t h = target ? target->height : info.height;
    flux_recti area;
    if (!canvas_pass_render_area(config, w, h, &area)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "Canvas render area is invalid or out of bounds");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    v->color_format =
        target ? flux_format_to_vk(target->format) : flux_surface_vk_format(c->surface);
    if (v->color_format == VK_FORMAT_UNDEFINED)
        return FLUX_ERROR_UNSUPPORTED;
    uint32_t slot = flux_frame_index(f);
    if (slot >= FLUX_MAX_FRAMES_IN_FLIGHT)
        return FLUX_ERROR_OUT_OF_RANGE;
    canvas_attachment_set *attachments =
        target ? target_attachments_get(c, slot, w, h) : &v->surface_attachments[slot];
    if (!attachments) {
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "canvas target attachment pool unavailable");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    v->active_attachments = attachments;

    /* ADR-0069: every pass renders into the per-slot working-space
     * intermediate; the destination only sees pixels through the output
     * transform at end_pass. */
    if (!linear_ensure(c, attachments, w, h)) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas linear intermediate unavailable");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    canvas_owned_image *linear = &attachments->linear;

    /* Destination colour space: what pixels are written in — the
     * negotiated surface space, or the display profile override
     * (ADR-0069 legacy-platform path) — or the render target's implied
     * content space: 16F targets are working-space linear, 8-bit targets
     * are sRGB content. */
    flux_color_space dest_space;
    if (target)
        dest_space = target->format == FLUX_FORMAT_RGBA16_SFLOAT
                         ? (flux_color_space)FLUX_COLOR_SPACE_SCRGB
                         : (flux_color_space)FLUX_COLOR_SPACE_SRGB;
    else
        dest_space = c->surface->content_space;

    const flux_color_space working = FLUX_COLOR_SPACE_SCRGB;
    flux_mat3 enc, dec;
    flux_color_space_transform_matrix(working, dest_space, &enc);
    flux_color_space_transform_matrix(dest_space, working, &dec);

    uint32_t encode_flags = 0;
    float dither_levels = 255.0f;
    if (v->color_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        encode_flags |= FLUX_OUTPUT_F_NO_DITHER; /* float destination keeps full precision */
        dither_levels = 0.0f;
    } else if (v->color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        dither_levels = 1023.0f;
    }

    v->out_encode = (flux_output_push){0};
    mat3_to_push(&enc, v->out_encode.primaries);
    v->out_encode.image_handle = linear->bindless;
    v->out_encode.sampler_handle = flux_device_default_sampler_handle(c->device);
    v->out_encode.transfer = (uint32_t)dest_space.transfer;
    v->out_encode.flags = encode_flags;
    v->out_encode.gamma = dest_space.gamma;
    v->out_encode.dither_levels = dither_levels;
    /* BT.2408 graphics white by default; the surface's HDR desc wins. */
    float sdr_white = 203.0f;
    if (!target && c->surface->sdr_white_nits > 0.0f)
        sdr_white = c->surface->sdr_white_nits;
    v->out_encode.sdr_white_nits = sdr_white;

    v->out_decode = v->out_encode;
    mat3_to_push(&dec, v->out_decode.primaries);
    v->out_decode.flags = FLUX_OUTPUT_F_DECODE | FLUX_OUTPUT_F_NO_DITHER;
    v->out_decode.image_handle = FLUX_BINDLESS_INVALID; /* filled by the seed blit */
    v->out_area = area;
    v->active_slot = slot;
    /* fb size feeds the seed blit's viewport, so publish it before the
     * LOAD block (it is re-asserted with the rest of the pass state below). */
    c->fb_width = w;
    c->fb_height = h;

    flux_vec4 cc;
    unpack_clear(config->clear_color, &cc);

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);

    /* Intermediate: sampled (or fresh) -> attachment. */
    color_barrier2(cmd, linear->image,
                   linear->layout == VK_IMAGE_LAYOUT_UNDEFINED
                       ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                       : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   linear->layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_READ_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   linear->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    linear->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    /* LOAD passes: seed the intermediate with the destination's current
     * pixels, decoded into the working space (ADR-0069). */
    if (!config->clear_color) {
        uint32_t src = FLUX_BINDLESS_INVALID;
        if (target) {
            /* First use has undefined contents — nothing meaningful to
             * load, matching the legacy load-from-undefined behaviour. */
            if (target->current_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                src = target->bindless;
        } else {
            flux_surface *s = c->surface;
            VkImage img = s->images[s->current_image];
            color_barrier2(cmd, img, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                           s->image_layouts[s->current_image],
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            s->image_layouts[s->current_image] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            src = s->image_bindless[s->current_image];
        }
        if (src != FLUX_BINDLESS_INVALID) {
            v->out_decode.image_handle = src;
            record_output_blit(c, linear->view, FLUX_CANVAS_LINEAR_FORMAT, area, &v->out_decode);
            /* The seed's attachment writes must be visible to the main
             * pass's load-op read of the same image. */
            color_barrier2(cmd, linear->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (!target) {
            flux_surface *s = c->surface;
            color_barrier2(cmd, s->images[s->current_image],
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                           VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            s->image_layouts[s->current_image] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    /* A multisample attachment cannot LOAD the contents of its one-sample
     * resolve destination. AUTO preserves the historical CLEAR => 4x MSAA,
     * LOAD => single-sample policy; explicit NONE lets image-heavy or
     * compositor passes clear without allocating and resolving a 4x target. */
    bool use_msaa =
        config->antialias == FLUX_CANVAS_ANTIALIAS_MSAA_4X ||
        (config->antialias == FLUX_CANVAS_ANTIALIAS_AUTO && config->clear_color != nullptr);
    VkSampleCountFlagBits samples = use_msaa ? FLUX_CANVAS_SAMPLES : VK_SAMPLE_COUNT_1_BIT;
    flux_pass_attachment att = {
        .load_op = config->clear_color ? FLUX_LOAD_CLEAR : FLUX_LOAD_LOAD,
        .store_op = use_msaa ? FLUX_STORE_DONT_CARE : FLUX_STORE_STORE,
        .clear_color = cc,
        .view = linear->view,
        .format = FLUX_CANVAS_LINEAR_FORMAT,
    };

    if (target) {
        /* The target image is no longer rendered here — only sampled by the
         * seed (above) and written by the output transform at end_pass.
         * First use starts from UNDEFINED; later passes arrive in the
         * sampleable layout established by the preceding target pass. */
        if (target->current_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            target->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                      "canvas target is neither new nor sampleable before begin");
            return FLUX_ERROR_INVALID_STATE;
        }
    }

    /* Render to the owned multisample colour target and resolve into the
     * intermediate, so vector fills are anti-aliased. LOAD deliberately
     * bypasses this block. */
    if (use_msaa) {
        if (!msaa_ensure(c, attachments, w, h)) {
            FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas MSAA attachment unavailable");
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        color_barrier2(cmd, attachments->msaa.image,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        att.view = attachments->msaa.view;
        att.resolve_view = linear->view;
    }

    /* Canvas-owned stencil attachment (ADR-0014). A no-stencil pass is a
     * distinct dynamic-rendering contract: it neither allocates nor barriers
     * an image, and its pipelines declare VK_FORMAT_UNDEFINED. Default passes
     * retain the historical stencil fallback and therefore bind the matching
     * attachment whenever the device exposes a supported format. */
    flux_pass_depth_attachment stencil_att;
    canvas_owned_image *stencil = nullptr;
    bool has_stencil = false;
    if (!config->skip_stencil)
        has_stencil = stencil_ensure(c, attachments, w, h, samples, &stencil);
    if (!config->skip_stencil && v->stencil_format != VK_FORMAT_UNDEFINED && !has_stencil) {
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas stencil attachment unavailable");
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    if (has_stencil) {
        image_barrier2(cmd, stencil->image, VK_IMAGE_ASPECT_STENCIL_BIT,
                   VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                           VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

        stencil_att = (flux_pass_depth_attachment){
            .view = stencil->view,
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
        .width = area.w,
        .height = area.h,
        .render_offset_x = area.x,
        .render_offset_y = area.y,
    };

    flux_frame_begin_pass(f, &pass);
    c->pass_active = true;
    c->target_pass = (target != nullptr);
    c->target = target;
    c->stencil_available = has_stencil;
    c->stencil_forbidden = config->skip_stencil;
    c->fb_width = w;
    c->fb_height = h;
    v->active_samples = samples;
    v->active_stencil = has_stencil;

    VkViewport vp = {.x = 0.0f,
                     .y = 0.0f,
                     .width = (float)w,
                     .height = (float)h,
                     .minDepth = 0.0f,
                     .maxDepth = 1.0f};
    VkRect2D sc = {.offset = {area.x, area.y}, .extent = {area.w, area.h}};
    c->states[0].scissor = area;

    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    /* Don't pre-bind a pipeline — each draw picks the right one. Bind the
     * bindless set now; it's pipeline-layout-scoped and shared. */
    v->bound_pipeline = VK_NULL_HANDLE;
    /* end_pass drains the batch; reset defensively so a pass that failed
     * mid-begin never inherits a stale open batch. */
    v->batch.pipeline = VK_NULL_HANDLE;
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
    batch_flush(c);
    flux_frame_end_pass(c->frame);
    c->pass_active = false;

    flux_vk_canvas *v = vkc(c);
    canvas_attachment_set *attachments = v->active_attachments;
    if (!attachments) {
        attachments = c->target ? target_attachments_get(c, v->active_slot, c->fb_width, c->fb_height)
                                : &v->surface_attachments[v->active_slot];
    }
    v->active_attachments = nullptr;
    if (!attachments)
        return;
    canvas_owned_image *linear = &attachments->linear;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);

    /* Intermediate: attachment -> sampled by the output transform. */
    color_barrier2(cmd, linear->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    linear->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    flux_image *t = c->target;
    if (t) {
        /* The target only now becomes a render target, receiving the
         * transformed pixels. */
        bool first_use = t->current_layout == VK_IMAGE_LAYOUT_UNDEFINED;
        color_barrier2(cmd, t->image,
                       first_use ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                                 : (VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT),
                       first_use ? 0 : VK_ACCESS_2_SHADER_READ_BIT,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                       t->current_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        t->current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    /* Output transform into the destination (NULL view = swapchain image,
     * filled by flux_frame_begin_pass). LOAD preserves pixels outside the
     * pass area; the swapchain's final PRESENT transition is frame.c's. */
    record_output_blit(c, t ? t->view : VK_NULL_HANDLE, v->color_format, v->out_area,
                       &v->out_encode);

    if (!t)
        return;

    /* Trailing barrier: COLOR_ATTACHMENT -> SHADER_READ_ONLY so a following
     * flux_effect_blur / draw_image needs no caller sync. */
    color_barrier2(cmd, t->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                       VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
    bool stencil_program = id == CANVAS_PIPE_STENCIL_WRITE || id == CANVAS_PIPE_STENCIL_WRITE_EO ||
                           id == CANVAS_PIPE_COVER_SOLID || id == CANVAS_PIPE_COVER_GRADIENT;
    if (stencil_program && !v->active_stencil) {
        if (c->pass_error == FLUX_OK)
            c->pass_error = FLUX_ERROR_INVALID_STATE;
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "stencil-dependent Canvas draw used in a no-stencil pass");
        return false;
    }
    VkPipelineLayout layout;
    VkPipeline pipeline;
    /* ADR-0069: draws always render into the working-space intermediate,
     * so pipelines are keyed by its format — never the destination's. */
    if (get_canvas_pipeline_id(c->device, FLUX_CANVAS_LINEAR_FORMAT, v->active_samples, id,
                               c->pending_blend, v->active_stencil, &layout, &pipeline) != FLUX_OK)
        return false;
    if (v->bound_pipeline != pipeline) {
        /* A pipeline change ends the open batch: its draw must be
         * recorded before the new vkCmdBindPipeline, so batched
         * vertices always execute under the pipeline they were
         * submitted with. */
        batch_flush(c);
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        v->bound_pipeline = pipeline;
    }
    return true;
}

/* Record the open batch's scissor + push constants + a single draw for
 * every vertex accumulated so far, then close the batch. No-op when no
 * batch is open. Invariant: an open batch's pipeline is always the
 * bound one — vk_bind_program flushes before recording a new bind. */
static void batch_flush(flux_canvas *c) {
    flux_vk_canvas *v = vkc(c);
    if (v->batch.pipeline == VK_NULL_HANDLE || v->batch.vertex_count == 0) {
        v->batch.pipeline = VK_NULL_HANDLE;
        v->batch.vertex_count = 0;
        return;
    }
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    VkRect2D sc = {.offset = {v->batch.scissor.x, v->batch.scissor.y},
                   .extent = {v->batch.scissor.w, v->batch.scissor.h}};
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdPushConstants(cmd, v->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(v->batch.push), &v->batch.push);
    vkCmdDraw(cmd, v->batch.vertex_count, 1, 0, 0);
    c->recorded_draws++;
    v->batch.pipeline = VK_NULL_HANDLE;
    v->batch.vertex_count = 0;
}

/* Every push-constant byte after verts_address must match for two
 * submits to share a draw: paint kind, gradient stops, image/sampler
 * bindless handles, UV remap. build_push zero-initialises the whole
 * block, so padding compares deterministically; a false mismatch only
 * splits a batch, never corrupts one. */
static bool push_params_equal(const flux_canvas_push *a, const flux_canvas_push *b) {
    return memcmp((const char *)a + offsetof(flux_canvas_push, inv_window_size),
                  (const char *)b + offsetof(flux_canvas_push, inv_window_size),
                  sizeof(flux_canvas_push) - offsetof(flux_canvas_push, inv_window_size)) == 0;
}

static void vk_submit(const flux_canvas_backend *self, flux_canvas *c, canvas_pipe_id id,
                      const flux_canvas_push *push, const flux_canvas_vertex *verts,
                      uint32_t vertex_count) {
    if (!c->recording || vertex_count == 0)
        return;
    if (!vk_bind_program(self, c, id))
        return;
    c->submit_calls++;
    flux_vk_canvas *v = vkc(c);

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

    /* Append to the open batch when all draw-visible state matches and
     * the new slice continues the batch's ring run. The ring is a bump
     * allocator and flux_canvas_vertex is 16-byte/4-aligned, so slices
     * from back-to-back canvas submits are exactly contiguous; any
     * interleaved allocation or a wrap breaks contiguity and flushes. */
    if (v->batch.pipeline == v->bound_pipeline && v->batch.end == slice.gpu_address &&
        v->batch.scissor.x == clip.x && v->batch.scissor.y == clip.y &&
        v->batch.scissor.w == clip.w && v->batch.scissor.h == clip.h &&
        push_params_equal(&v->batch.push, &pc)) {
        v->batch.vertex_count += vertex_count;
        v->batch.end += vertex_count * sizeof(flux_canvas_vertex);
        return;
    }

    /* State changed or the ring broke the run: draw what accumulated,
     * then open a new batch. */
    batch_flush(c);
    v->batch.pipeline = v->bound_pipeline;
    v->batch.scissor = clip;
    v->batch.push = pc;
    v->batch.end = slice.gpu_address + vertex_count * sizeof(flux_canvas_vertex);
    v->batch.vertex_count = vertex_count;
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
