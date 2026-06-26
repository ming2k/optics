/*
 * Canvas — lifecycle, state stack, and public draw entry points.
 *
 * Heavy geometry work (flattening, tessellation, stroking) lives in
 * geometry_*.c; pipeline caching and draw submission live in renderer.c.
 */
#include "internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

flux_result flux_canvas_create(const flux_canvas_desc *desc, flux_canvas **out) {
    if (!desc || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_CANVAS_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_CANVAS_DESC");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    if (!desc->surface) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "canvas requires a surface");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    flux_device *device = desc->surface->device;
    flux_canvas *c = flux_internal_alloc(device, sizeof(*c));
    if (!c)
        return FLUX_ERROR_OUT_OF_MEMORY;
    atomic_init(&c->ref_count, 1u);
    c->device = flux_device_retain(device);
    c->surface = flux_surface_retain(desc->surface);

    /* Content scale (device-pixel ratio); the base transform applies it. */
    c->content_scale = (desc->scale > 0.0f) ? desc->scale : 1.0f;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);
    c->state_top = 0;

    /* Shared device-cached layout, keyed by surface format. The
     * canvas borrows it (the device owns it). Pipelines are looked
     * up on demand based on paint kind. */
    c->color_format = flux_surface_vk_format(desc->surface);
    c->stencil_format = flux_canvas_stencil_format(device);
    void *st = canvas_state_get_or_init(device);
    if (!st) {
        flux_surface_release(c->surface);
        flux_device_release(c->device);
        flux_internal_free(device, c);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    /* Warm the cache (so canvas_create can fail fast on shader/layout
     * problems rather than first-draw). Build every pipeline kind for
     * this surface's color format up front: the glyph pipeline in
     * particular pulls in SPIR-V JIT work on first use that can take
     * ~0.5-1.5 s on Mesa/Intel, which trips the IME daemon's 3 s
     * watchdog on the first indicator banner render. Warming every
     * kind here keeps the cost inside canvas_create (no watchdog
     * oversight at typio's App::init stage) and turns first-draw
     * into a hot path. The on-disk VkPipelineCache makes restarts
     * cheaper. */
    for (int id = 0; id < CANVAS_PIPE_COUNT; id++) {
        VkPipeline warm;
        flux_result r =
            get_canvas_pipeline_id(device, c->color_format, (canvas_pipe_id)id, &c->layout, &warm);
        if (r != FLUX_OK) {
            flux_surface_release(c->surface);
            flux_device_release(c->device);
            flux_internal_free(device, c);
            return r;
        }
    }

    /* Per-instance scratch (was a function-static; moved here for
     * reentrancy — two canvases on two threads now isolate cleanly). */
    c->scratch_pts =
        flux_internal_alloc(device, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_pts));
    c->scratch_verts =
        flux_internal_alloc(device, FLUX_CANVAS_PATH_SCRATCH_CAP * 3 * sizeof(*c->scratch_verts));
    c->scratch_contours =
        flux_internal_alloc(device, FLUX_CANVAS_MAX_CONTOURS * sizeof(*c->scratch_contours));
    c->scratch_lnk_prev =
        flux_internal_alloc(device, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_lnk_prev));
    c->scratch_lnk_next =
        flux_internal_alloc(device, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_lnk_next));
    c->scratch_frames =
        flux_internal_alloc(device, FLUX_CANVAS_PATH_SCRATCH_CAP * sizeof(*c->scratch_frames));
    if (!c->scratch_pts || !c->scratch_verts || !c->scratch_contours || !c->scratch_lnk_prev ||
        !c->scratch_lnk_next || !c->scratch_frames) {
        flux_internal_free(device, c->scratch_pts);
        flux_internal_free(device, c->scratch_verts);
        flux_internal_free(device, c->scratch_contours);
        flux_internal_free(device, c->scratch_lnk_prev);
        flux_internal_free(device, c->scratch_lnk_next);
        flux_internal_free(device, c->scratch_frames);
        flux_surface_release(c->surface);
        flux_device_release(c->device);
        flux_internal_free(device, c);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    *out = c;
    return FLUX_OK;
}

/* Canvas-owned stencil attachment (ADR-0014), sized to the surface
 * and recreated on extent change. Sized resources are safe to drop
 * here without a wait: the only extent-change path is
 * flux_surface_resize, which stalls the device first. */
static void canvas_stencil_destroy(flux_canvas *c) {
    flux_device *d = c->device;
    if (c->stencil_view)
        vkDestroyImageView(d->device, c->stencil_view, nullptr);
    if (c->stencil_image)
        vkDestroyImage(d->device, c->stencil_image, nullptr);
    if (c->stencil_alloc.memory)
        flux_vk_deallocate(d, &c->stencil_alloc);
    c->stencil_view = VK_NULL_HANDLE;
    c->stencil_image = VK_NULL_HANDLE;
    c->stencil_alloc = (flux_vk_alloc){0};
    c->stencil_extent = (VkExtent2D){0, 0};
}

static bool canvas_stencil_ensure(flux_canvas *c, uint32_t w, uint32_t h) {
    if (c->stencil_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (c->stencil_view && c->stencil_extent.width == w && c->stencil_extent.height == h)
        return true;

    canvas_stencil_destroy(c);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = c->stencil_format,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = FLUX_CANVAS_SAMPLES, /* match the MSAA colour target */
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &c->stencil_image,
                            &c->stencil_alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = c->stencil_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = c->stencil_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &c->stencil_view) != VK_SUCCESS) {
        canvas_stencil_destroy(c);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas stencil view failed");
        return false;
    }
    c->stencil_extent = (VkExtent2D){w, h};
    return true;
}

/* Canvas-owned multisample colour target, sized to the surface and recreated
 * on extent change (same lifecycle rules as the stencil above). */
static void canvas_msaa_destroy(flux_canvas *c) {
    flux_device *d = c->device;
    if (c->msaa_view)
        vkDestroyImageView(d->device, c->msaa_view, nullptr);
    if (c->msaa_image)
        vkDestroyImage(d->device, c->msaa_image, nullptr);
    if (c->msaa_alloc.memory)
        flux_vk_deallocate(d, &c->msaa_alloc);
    c->msaa_view = VK_NULL_HANDLE;
    c->msaa_image = VK_NULL_HANDLE;
    c->msaa_alloc = (flux_vk_alloc){0};
    c->msaa_extent = (VkExtent2D){0, 0};
}

static bool canvas_msaa_ensure(flux_canvas *c, uint32_t w, uint32_t h) {
    if (c->color_format == VK_FORMAT_UNDEFINED || w == 0 || h == 0)
        return false;
    if (c->msaa_view && c->msaa_extent.width == w && c->msaa_extent.height == h)
        return true;

    canvas_msaa_destroy(c);

    flux_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = c->color_format,
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
    if (flux_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &c->msaa_image,
                            &c->msaa_alloc) != FLUX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = c->msaa_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = c->color_format,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &c->msaa_view) != VK_SUCCESS) {
        canvas_msaa_destroy(c);
        FLUX_FAIL(FLUX_ERROR_BACKEND_FAILURE, "canvas msaa view failed");
        return false;
    }
    c->msaa_extent = (VkExtent2D){w, h};
    return true;
}

void flux_canvas_destroy(flux_canvas *c) {
    if (!c)
        return;
    /* The owned attachments may still be referenced by in-flight frames. */
    if (c->stencil_image || c->msaa_image) {
        flux_device_wait_idle(c->device);
        canvas_stencil_destroy(c);
        canvas_msaa_destroy(c);
    }
    /* Pipeline + layout are owned by the device's canvas cache; we
     * just drop our borrowed references. flux_device_release frees
     * the cache via canvas_state_destroy. */
    flux_device *dev = c->device;
    flux_internal_free(dev, c->scratch_pts);
    flux_internal_free(dev, c->scratch_verts);
    flux_internal_free(dev, c->scratch_contours);
    flux_internal_free(dev, c->scratch_lnk_prev);
    flux_internal_free(dev, c->scratch_lnk_next);
    flux_internal_free(dev, c->scratch_frames);
    flux_surface_release(c->surface);
    flux_internal_free(dev, c);
    flux_device_release(dev);
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

flux_result flux_canvas_begin(flux_canvas *c, flux_frame *f, const flux_color *clear) {
    if (!c || !f)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (c->recording) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "flux_canvas_begin called twice without _end");
        return FLUX_ERROR_INVALID_STATE;
    }
    c->frame = f;
    c->recording = true;
    c->state_top = 0;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);

    /* Clear value is interpreted in the swapchain's storage colour
     * space. We negotiate a UNORM (non-_SRGB) format, so the hardware
     * does NOT linearise on write — the value goes straight to bytes
     * and the user sees it as-is. Linearising here would darken the
     * cleared area below every premultiplied colour drawn into it. */
    flux_vec4 cc = {0, 0, 0, 0};
    if (clear) {
        uint8_t r8, g8, b8, a8;
        flux_color_unpack(*clear, &r8, &g8, &b8, &a8);
        cc.x = (float)r8 / 255.0f;
        cc.y = (float)g8 / 255.0f;
        cc.z = (float)b8 / 255.0f;
        cc.w = (float)a8 / 255.0f;
    }
    flux_pass_attachment att = {
        .view = VK_NULL_HANDLE, /* swapchain image */
        .load_op = clear ? FLUX_LOAD_CLEAR : FLUX_LOAD_LOAD,
        .store_op = FLUX_STORE_STORE,
        .clear_color = cc,
    };
    flux_pass_desc pass = {
        .type = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
    };

    flux_surface_info info;
    flux_surface_get_info(c->surface, &info);

    /* Render to the owned multisample colour target and resolve to the
     * surface image, so vector fills are anti-aliased. */
    if (canvas_msaa_ensure(c, info.width, info.height)) {
        VkCommandBuffer pre_cmd = flux_frame_vk_command_buffer(f);
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = c->msaa_image,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(pre_cmd, &di);

        att.view = c->msaa_view;
        att.format = c->color_format;
        att.resolve_to_surface = true;
    }

    /* Canvas-owned stencil attachment (ADR-0014). The canvas
     * pipelines all declare this format, so when the device has one
     * the pass must carry it — even if no fill ends up needing the
     * stencil fallback this frame. Cleared at load; the cover pass
     * re-zeroes whatever it consumed, so the attachment stays clean
     * across fills within the pass. */
    flux_pass_depth_attachment stencil_att;
    if (canvas_stencil_ensure(c, info.width, info.height)) {
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
            .image = c->stencil_image,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(pre_cmd, &di);

        stencil_att = (flux_pass_depth_attachment){
            .view = c->stencil_view,
            .format = c->stencil_format,
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_DONT_CARE,
            .clear_stencil = 0,
        };
        pass.stencil = &stencil_att;
    }

    flux_frame_begin_pass(f, &pass);
    c->pass_active = true;
    VkViewport vp = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)info.width,
        .height = (float)info.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {.offset = {0, 0}, .extent = {info.width, info.height}};
    c->states[0].scissor = sc;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    /* Don't pre-bind a pipeline — each draw picks the right one for
     * its paint kind. We do bind the bindless set now though; it's
     * pipeline-layout-scoped and shared across canvas pipelines. */
    c->bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bindless = flux_device_bindless_set(c->device);
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 1, &bindless, 0,
                                nullptr);
    }
    return FLUX_OK;
}

void flux_canvas_end(flux_canvas *c) {
    if (!c || !c->recording)
        return;
    if (c->pass_active) {
        flux_frame_end_pass(c->frame);
        c->pass_active = false;
    }
    c->frame = nullptr;
    c->recording = false;
}

/* ------------------------------------------------------------------ */
/*  Render-target capture (ADR-0017)                                  */
/* ------------------------------------------------------------------ */

flux_result flux_canvas_begin_target(flux_canvas *c, flux_frame *f, flux_image *target,
                                     const flux_color *clear) {
    if (!c || !f || !target)
        return FLUX_ERROR_INVALID_ARGUMENT;
    /* A target pass may not nest inside an active pass (frame or target). */
    if (c->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "flux_canvas_begin_target while a pass is already active");
        return FLUX_ERROR_INVALID_STATE;
    }
    /* v1: target format must match the canvas pipelines' baked-in colour
     * format (the pipeline cache is keyed on colour format only). */
    VkFormat target_fmt = flux_format_to_vk(target->format);
    if (target_fmt != c->color_format) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "target colour format != canvas colour format");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    /* v1: target extent must match the surface extent, because
     * flux_frame_begin_pass hardwires the render area to s->extent and the
     * canvas-owned MSAA/stencil attachments are surface-sized. */
    flux_surface_info info;
    flux_surface_get_info(c->surface, &info);
    if (target->width != info.width || target->height != info.height) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT,
                  "target extent != surface extent (unsupported in v1)");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    c->frame = f;
    c->recording = true;
    c->state_top = 0;
    c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);

    /* Transition target SHADER_READ_ONLY_OPTIMAL -> COLOR_ATTACHMENT_OPTIMAL
     * in the frame's command stream (it is the MSAA resolve destination). */
    {
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
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(pre_cmd, &di);
        target->current_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    /* Clear value in UNORM/byte space (same convention as canvas_begin). */
    flux_vec4 cc = {0, 0, 0, 0};
    if (clear) {
        uint8_t r8, g8, b8, a8;
        flux_color_unpack(*clear, &r8, &g8, &b8, &a8);
        cc.x = (float)r8 / 255.0f;
        cc.y = (float)g8 / 255.0f;
        cc.z = (float)b8 / 255.0f;
        cc.w = (float)a8 / 255.0f;
    }

    /* Canvas pipelines are built with rasterizationSamples = FLUX_CANVAS_SAMPLES
     * (4x) AND declare a stencil attachment format, so the target pass must:
     *   - render into the canvas-owned 4x MSAA image, resolving into `target`
     *   - carry the canvas-owned stencil attachment (pipeline/pass match, ADR-0014)
     * This mirrors canvas_begin exactly, except the resolve destination is the
     * caller's target image instead of the swapchain. */
    flux_pass_attachment att = {
        .format = target_fmt,
        .load_op = clear ? FLUX_LOAD_CLEAR : FLUX_LOAD_LOAD,
        .store_op = FLUX_STORE_DONT_CARE, /* MSAA colour is discarded after resolve */
        .clear_color = cc,
        .resolve_view = target->view, /* resolve into the capture target */
    };

    if (canvas_msaa_ensure(c, info.width, info.height)) {
        VkCommandBuffer pre_cmd = flux_frame_vk_command_buffer(f);
        VkImageMemoryBarrier2 b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, /* contents cleared at load */
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .image = c->msaa_image,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(pre_cmd, &di);
        att.view = c->msaa_view;
    }

    /* Stencil attachment (ADR-0014): the canvas pipelines declare this format,
     * so the pass must carry it or pipeline/pass compatibility breaks and no
     * draws render. Sized to the surface (== target extent in v1). */
    flux_pass_depth_attachment stencil_att;
    if (canvas_stencil_ensure(c, info.width, info.height)) {
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
            .image = c->stencil_image,
            .subresourceRange =
                {
                    .aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        };
        VkDependencyInfo di = {
            .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers = &b,
        };
        vkCmdPipelineBarrier2(pre_cmd, &di);

        stencil_att = (flux_pass_depth_attachment){
            .view = c->stencil_view,
            .format = c->stencil_format,
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_DONT_CARE,
            .clear_stencil = 0,
        };
    }

    flux_pass_desc pass = {
        .type = FLUX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
    };
    if (c->stencil_view) {
        pass.stencil = &stencil_att;
    }

    flux_frame_begin_pass(f, &pass);
    c->pass_active = true;
    c->target_pass = true;
    c->target = target;

    VkViewport vp = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)target->width,
        .height = (float)target->height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {.offset = {0, 0}, .extent = {target->width, target->height}};
    c->states[0].scissor = sc;
    VkCommandBuffer cmd = flux_frame_vk_command_buffer(f);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    c->bound_pipeline = VK_NULL_HANDLE;
    VkDescriptorSet bindless = flux_device_bindless_set(c->device);
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c->layout, 0, 1, &bindless, 0,
                                nullptr);
    }
    return FLUX_OK;
}

void flux_canvas_end_target(flux_canvas *c) {
    if (!c || !c->target_pass)
        return;

    if (c->pass_active) {
        flux_frame_end_pass(c->frame);
        c->pass_active = false;
    }

    /* Trailing barrier: COLOR_ATTACHMENT_OPTIMAL -> SHADER_READ_ONLY_OPTIMAL
     * so the following flux_effect_blur / draw_image needs no caller sync. */
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
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);
    t->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    c->target = nullptr;
    c->target_pass = false;
    c->frame = nullptr;
    c->recording = false;
    /* The capture session is closed; a subsequent canvas_begin or
     * begin_target opens a fresh session on a (possibly different) frame. */
}

/* ------------------------------------------------------------------ */
/*  State stack                                                       */
/* ------------------------------------------------------------------ */

void flux_canvas_save(flux_canvas *c) {
    if (!c)
        return;
    if (c->state_top + 1 >= FLUX_CANVAS_MAX_STATES) {
        /* Stack overflow: previously this was a silent no-op, leaving
         * the caller's save/restore pairing stranded with no diagnostic.
         * Surface the failure through both the device logger (visible
         * in any flux-log-equipped app) and the thread-local error
         * slot, so the caller can detect via flux_get_last_error. */
        if (c->device && c->device->log) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "flux_canvas_save: state stack overflow (depth > %d); "
                     "save was a no-op — check for unbalanced save/restore",
                     FLUX_CANVAS_MAX_STATES);
            c->device->log(FLUX_LOG_ERROR, "flux_canvas_save", 0, "%s", buf, c->device->log_user);
        }
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "flux_canvas_save: state stack overflow");
        return;
    }
    c->states[c->state_top + 1] = c->states[c->state_top];
    c->state_top++;
}

void flux_canvas_restore(flux_canvas *c) {
    if (!c || c->state_top == 0)
        return;
    c->state_top--;
    if (c->pass_active) {
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdSetScissor(cmd, 0, 1, &c->states[c->state_top].scissor);
    }
}

void flux_canvas_translate(flux_canvas *c, float x, float y) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_translate(x, y));
}

void flux_canvas_scale(flux_canvas *c, float sx, float sy) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_scale(sx, sy));
}

void flux_canvas_rotate(flux_canvas *c, float radians) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, flux_mat3x2_rotate(radians));
}

void flux_canvas_set_scale(flux_canvas *c, float scale) {
    if (!c)
        return;
    c->content_scale = (scale > 0.0f) ? scale : 1.0f;
    /* If set between begin/end, refresh the base transform so the change
     * takes effect this frame (callers usually set it before begin). */
    if (c->recording && c->state_top == 0)
        c->states[0].transform = flux_mat3x2_scale(c->content_scale, c->content_scale);
}

float flux_canvas_get_scale(const flux_canvas *c) {
    if (!c)
        return 1.0f;
    /* The *effective* scale: the active transform's pixel scale, so callers
     * (e.g. flux_text) rasterise to match whether the scale comes from the
     * content-scale base transform or a manual flux_canvas_scale on top. */
    return flux_canvas_mat3x2_pixel_scale(c->states[c->state_top].transform);
}

void flux_canvas_transform(flux_canvas *c, flux_mat3x2 m) {
    if (!c)
        return;
    flux_canvas_state *s = &c->states[c->state_top];
    s->transform = flux_mat3x2_multiply(s->transform, m);
}

void flux_canvas_clip_rect(flux_canvas *c, flux_rect r) {
    if (!c)
        return;
    int32_t x = (int32_t)r.x;
    int32_t y = (int32_t)r.y;
    uint32_t w = (uint32_t)(r.w > 0.0f ? r.w : 0.0f);
    uint32_t h = (uint32_t)(r.h > 0.0f ? r.h : 0.0f);
    VkRect2D sc = {.offset = {x, y}, .extent = {w, h}};
    c->states[c->state_top].scissor = sc;
    if (c->pass_active) {
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdSetScissor(cmd, 0, 1, &sc);
    }
}

/* ------------------------------------------------------------------ */
/*  Public draws                                                      */
/* ------------------------------------------------------------------ */

void flux_canvas_fill_rect(flux_canvas *c, flux_rect r, const flux_paint *paint) {
    if (!c || !c->recording)
        return;
    flux_mat3x2 tx = c->states[c->state_top].transform;
    flux_color vc = paint ? paint->color : 0xFF000000u;

    flux_point p0 = {r.x, r.y};
    flux_point p1 = {r.x + r.w, r.y};
    flux_point p2 = {r.x + r.w, r.y + r.h};
    flux_point p3 = {r.x, r.y + r.h};

    flux_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, vc);
    push_vertex(&v[1], p1, tx, vc);
    push_vertex(&v[2], p2, tx, vc);
    push_vertex(&v[3], p0, tx, vc);
    push_vertex(&v[4], p2, tx, vc);
    push_vertex(&v[5], p3, tx, vc);
    submit_triangles(c, paint, v, 6);
}

void flux_canvas_fill_rect_color(flux_canvas *c, flux_rect r, flux_color color) {
    flux_paint p = flux_paint_default();
    p.color = color;
    flux_canvas_fill_rect(c, r, &p);
}

static void draw_image_with_sampler_handle(flux_canvas *c, flux_image *img, flux_bindless_handle sh,
                                           flux_rect dst, flux_rect src, flux_color tint,
                                           uint32_t kind) {
    if (sh == FLUX_BINDLESS_INVALID)
        return;

    /* Force the image pipeline for this draw. */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    if (get_canvas_pipeline(c->device, c->color_format, (flux_paint_kind)0xff, &layout,
                            &pipeline) != FLUX_OK) {
        return;
    }
    if (c->bound_pipeline != pipeline) {
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        c->bound_pipeline = pipeline;
    }

    flux_mat3x2 tx = c->states[c->state_top].transform;
    flux_point p0 = {dst.x, dst.y};
    flux_point p1 = {dst.x + dst.w, dst.y};
    flux_point p2 = {dst.x + dst.w, dst.y + dst.h};
    flux_point p3 = {dst.x, dst.y + dst.h};

    /* Vertices carry pre-transform position; the fragment shader reads
     * v_pos and derives UV from image_dst. The colour field is ignored
     * for a plain image (kind 3) but carries the premultiplied tint for
     * a coverage glyph (kind 4). */
    flux_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, tint);
    push_vertex(&v[1], p1, tx, tint);
    push_vertex(&v[2], p2, tx, tint);
    push_vertex(&v[3], p0, tx, tint);
    push_vertex(&v[4], p2, tx, tint);
    push_vertex(&v[5], p3, tx, tint);

    flux_transient slice;
    if (flux_frame_alloc_transient(c->frame, sizeof(v), alignof(flux_canvas_vertex), &slice) !=
        FLUX_OK)
        return;
    memcpy(slice.cpu, v, sizeof(v));

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    pc.verts_address = slice.gpu_address;
    pc.kind = kind;
    pc.image_handle = img->bindless;
    pc.sampler_handle = sh;
    /* image_dst is the post-transform rect (because v_pos is also
     * post-transform from the vertex shader's perspective). The
     * vertex shader passes raw position from push verts → so we use
     * the transformed corners' bounds. For now (no rotation in
     * draw_image) the rect-axis-aligned case maps cleanly. */
    flux_point t0 = flux_mat3x2_transform_point(tx, p0);
    flux_point t2 = flux_mat3x2_transform_point(tx, p2);
    pc.image_dst[0] = fminf(t0.x, t2.x);
    pc.image_dst[1] = fminf(t0.y, t2.y);
    pc.image_dst[2] = fabsf(t2.x - t0.x);
    pc.image_dst[3] = fabsf(t2.y - t0.y);
    pc.image_src[0] = src.x;
    pc.image_src[1] = src.y;
    pc.image_src[2] = src.w;
    pc.image_src[3] = src.h;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    vkCmdSetScissor(cmd, 0, 1, &c->states[c->state_top].scissor);
    vkCmdPushConstants(cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  SDF rounded rectangles / circles                                  */
/* ------------------------------------------------------------------ */

/* Draw a rounded rect (or, with stroke_hw > 0, its ring border) through the
 * SDF pipeline: resolution-independent analytic AA. The shape is evaluated in
 * screen-pixel space (translation + uniform scale; rotation is not modelled,
 * which suits axis-aligned UI). */
static void draw_sdf_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color,
                           float stroke_hw) {
    if (!c || !c->recording || r.w <= 0.0f || r.h <= 0.0f)
        return;

    VkPipelineLayout layout;
    VkPipeline pipeline;
    if (get_canvas_pipeline_id(c->device, c->color_format, CANVAS_PIPE_SDF, &layout, &pipeline) !=
        FLUX_OK) {
        return;
    }
    if (c->bound_pipeline != pipeline) {
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        c->bound_pipeline = pipeline;
    }

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float s = flux_canvas_mat3x2_pixel_scale(tx);

    flux_point center = {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
    flux_point cp = flux_mat3x2_transform_point(tx, center);
    float hx = r.w * 0.5f * s;
    float hy = r.h * 0.5f * s;
    float rad = radius * s;
    float max_r = fminf(hx, hy);
    if (rad > max_r)
        rad = max_r;
    if (rad < 0.0f)
        rad = 0.0f;
    float hw = stroke_hw > 0.0f ? stroke_hw * s : 0.0f;

    /* Quad covers the bbox plus the AA fringe and (when stroking) the ring's
     * outer half-width. Built directly in screen space — the vertex shader
     * only maps pixels to NDC. */
    float m = 1.5f + hw;
    float x0 = cp.x - hx - m, y0 = cp.y - hy - m;
    float x1 = cp.x + hx + m, y1 = cp.y + hy + m;

    flux_canvas_vertex v[6];
    const flux_point quad[6] = {
        {x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1},
    };
    for (int i = 0; i < 6; ++i) {
        v[i].pos[0] = quad[i].x;
        v[i].pos[1] = quad[i].y;
        v[i].color = color;
        v[i]._pad = 0;
    }

    flux_transient slice;
    if (flux_frame_alloc_transient(c->frame, sizeof(v), alignof(flux_canvas_vertex), &slice) !=
        FLUX_OK) {
        return;
    }
    memcpy(slice.cpu, v, sizeof(v));

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    pc.verts_address = slice.gpu_address;
    /* SDF params share the image_dst / image_src push slots (screen pixels). */
    pc.image_dst[0] = cp.x;
    pc.image_dst[1] = cp.y;
    pc.image_dst[2] = hx;
    pc.image_dst[3] = hy;
    pc.image_src[0] = rad;
    pc.image_src[1] = hw;
    pc.image_src[2] = 0.0f;
    pc.image_src[3] = 0.0f;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);
    vkCmdSetScissor(cmd, 0, 1, &c->states[c->state_top].scissor);
    vkCmdPushConstants(cmd, c->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(pc), &pc);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

void flux_canvas_fill_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color) {
    draw_sdf_rrect(c, r, radius, color, 0.0f);
}

void flux_canvas_stroke_rrect(flux_canvas *c, flux_rect r, float radius, flux_color color,
                              float width) {
    if (width <= 0.0f)
        width = 1.0f;
    draw_sdf_rrect(c, r, radius, color, width * 0.5f);
}

/* Whole-image sub-rect: sample the entire texture across dst. */
static const flux_rect FLUX_SRC_WHOLE = {0.0f, 0.0f, 1.0f, 1.0f};

void flux_canvas_draw_image(flux_canvas *c, flux_image *img, flux_rect dst,
                            const flux_paint *paint) {
    (void)paint; /* Stage 4.2.4 doesn't honour tint/blend yet */
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, sh, dst, FLUX_SRC_WHOLE, 0u, 3u);
}

void flux_canvas_draw_image_sub(flux_canvas *c, flux_image *img, flux_rect dst, flux_rect src) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, sh, dst, src, 0u, 3u);
}

void flux_canvas_draw_image_sampled(flux_canvas *c, flux_image *img, flux_sampler *sampler,
                                    flux_rect dst, const flux_paint *paint) {
    (void)paint;
    if (!c || !c->recording || !img || !sampler)
        return;
    flux_bindless_handle sh = flux_sampler_bindless_handle(sampler);
    draw_image_with_sampler_handle(c, img, sh, dst, FLUX_SRC_WHOLE, 0u, 3u);
}

void flux_canvas_draw_image_coverage(flux_canvas *c, flux_image *img, flux_rect dst,
                                     flux_color tint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, sh, dst, FLUX_SRC_WHOLE, tint, 4u);
}

void flux_canvas_draw_image_coverage_sub(flux_canvas *c, flux_image *img, flux_rect dst,
                                         flux_rect src, flux_color tint) {
    if (!c || !c->recording || !img)
        return;
    flux_bindless_handle sh = flux_device_default_sampler_handle(c->device);
    draw_image_with_sampler_handle(c, img, sh, dst, src, tint, 4u);
}

/* ------------------------------------------------------------------ */
/*  Glyph runs (ADR-0010)                                             */
/* ------------------------------------------------------------------ */

/* Pack a normalised UV pair into the vertex `_pad` field as unorm16x2
 * (canvas_solid.vert unpacks it for the glyph fragment shader). */
static uint32_t pack_uv(float u, float v) {
    uint32_t pu = (uint32_t)(fminf(fmaxf(u, 0.0f), 1.0f) * 65535.0f + 0.5f);
    uint32_t pv = (uint32_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 65535.0f + 0.5f);
    return pu | (pv << 16);
}

void flux_canvas_draw_glyph_run(flux_canvas *c, const flux_glyph_run_desc *desc) {
    if (!c || !c->recording || !desc)
        return;
    if (desc->type != FLUX_TYPE_GLYPH_RUN_DESC) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "desc->type != FLUX_TYPE_GLYPH_RUN_DESC");
        return;
    }
    if (!desc->atlas || (!desc->quads && desc->quad_count > 0)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "glyph run needs an atlas and quads");
        return;
    }
    if (desc->quad_count == 0)
        return;

    flux_bindless_handle sh = desc->sampler ? flux_sampler_bindless_handle(desc->sampler)
                                            : flux_device_default_sampler_handle(c->device);
    if (sh == FLUX_BINDLESS_INVALID || desc->atlas->bindless == FLUX_BINDLESS_INVALID)
        return;

    if (!ensure_pipeline_bound_id(c, CANVAS_PIPE_GLYPH))
        return;

    flux_mat3x2 tx = c->states[c->state_top].transform;
    float inv_w = 1.0f / (float)desc->atlas->width;
    float inv_h = 1.0f / (float)desc->atlas->height;

    flux_canvas_push pc;
    build_push(c, nullptr, &pc);
    pc.image_handle = desc->atlas->bindless;
    pc.sampler_handle = sh;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(c->frame);

    /* Chunked so a run of any length works within the scratch vertex
     * buffer; each chunk is still one draw. */
    const uint32_t max_quads = (FLUX_CANVAS_PATH_SCRATCH_CAP * 3) / 6;
    for (uint32_t base = 0; base < desc->quad_count; base += max_quads) {
        uint32_t n = desc->quad_count - base;
        if (n > max_quads)
            n = max_quads;

        flux_canvas_vertex *verts = c->scratch_verts;
        uint32_t v_count = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const flux_glyph_quad *q = &desc->quads[base + i];
            float u0 = (float)q->ax * inv_w, v0 = (float)q->ay * inv_h;
            float u1 = (float)(q->ax + q->aw) * inv_w;
            float v1 = (float)(q->ay + q->ah) * inv_h;

            flux_point p0 = {q->sx, q->sy};
            flux_point p1 = {q->sx + q->sw, q->sy};
            flux_point p2 = {q->sx + q->sw, q->sy + q->sh};
            flux_point p3 = {q->sx, q->sy + q->sh};

            push_vertex(&verts[v_count], p0, tx, q->color);
            push_vertex(&verts[v_count + 1], p1, tx, q->color);
            push_vertex(&verts[v_count + 2], p2, tx, q->color);
            push_vertex(&verts[v_count + 3], p0, tx, q->color);
            push_vertex(&verts[v_count + 4], p2, tx, q->color);
            push_vertex(&verts[v_count + 5], p3, tx, q->color);
            verts[v_count]._pad = pack_uv(u0, v0);
            verts[v_count + 1]._pad = pack_uv(u1, v0);
            verts[v_count + 2]._pad = pack_uv(u1, v1);
            verts[v_count + 3]._pad = pack_uv(u0, v0);
            verts[v_count + 4]._pad = pack_uv(u1, v1);
            verts[v_count + 5]._pad = pack_uv(u0, v1);
            v_count += 6;
        }

        flux_transient slice;
        if (flux_frame_alloc_transient(c->frame, v_count * sizeof(*verts),
                                       alignof(flux_canvas_vertex), &slice) != FLUX_OK) {
            c->dropped_draws++;
            return;
        }
        memcpy(slice.cpu, verts, v_count * sizeof(*verts));
        pc.verts_address = slice.gpu_address;

        vkCmdSetScissor(cmd, 0, 1, &c->states[c->state_top].scissor);
        vkCmdPushConstants(cmd, c->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc),
                           &pc);
        vkCmdDraw(cmd, v_count, 1, 0, 0);
    }
}

uint64_t flux_canvas_dropped_draws(const flux_canvas *c) {
    return c ? c->dropped_draws : 0;
}
