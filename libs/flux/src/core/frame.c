/*
 * Per-frame command recording + dynamic-rendering pass +
 * synchronization2 submit + present.
 *
 * Layout / transition contract:
 *   begin_frame    UNDEFINED|PRESENT_SRC -> COLOR_ATTACHMENT_OPTIMAL
 *                  (transition on the acquired swapchain image)
 *   begin_pass     vkCmdBeginRendering with a single colour
 *                  attachment matching the swapchain (when desc
 *                  attachments have NULL views), or caller-supplied
 *                  views otherwise.
 *   end_pass       vkCmdEndRendering
 *   submit         COLOR_ATTACHMENT_OPTIMAL -> PRESENT_SRC, then
 *                  end recording and vkQueueSubmit2 with sync.
 *   present        vkQueuePresentKHR waiting on render_finished.
 *
 * Transient-ring, bindless heap, timestamp queries: Stage 2b.2 / 2b.3.
 */
#include "internal.h"
#include <flux/vulkan.h>

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Layout transitions (sync2 barriers)                               */
/* ------------------------------------------------------------------ */

static void barrier_to_color_attachment(VkCommandBuffer cmd, VkImage img) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .image = img,
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
}

/* End-of-frame transition. Windowed surfaces go to PRESENT_SRC for the
 * presentation engine; offscreen surfaces (ADR-0013) go to TRANSFER_SRC
 * for flux_surface_read_pixels. */
static void barrier_to_final(VkCommandBuffer cmd, VkImage img, bool offscreen) {
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
        .dstStageMask =
            offscreen ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
        .dstAccessMask = offscreen ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout =
            offscreen ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .image = img,
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
}

/* ------------------------------------------------------------------ */
/*  Frame lifecycle                                                   */
/* ------------------------------------------------------------------ */

flux_result flux_surface_begin_frame(flux_surface *s, const flux_frame_begin_desc *desc,
                                     flux_frame **out) {
    if (!s || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;

    if (s->frame_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "flux_surface_begin_frame called while a frame is already in flight");
        return FLUX_ERROR_INVALID_STATE;
    }
    if (s->extent.width == 0 || s->extent.height == 0) {
        /* Minimised — nothing to render to. */
        return FLUX_ERROR_INVALID_STATE;
    }

    uint64_t timeout =
        (desc && desc->timeout_ns) ? desc->timeout_ns : FLUX_DEFAULT_FRAME_TIMEOUT_NS;

    uint32_t slot = s->current_frame;
    flux_per_frame *pf = &s->frames[slot];
    VkDevice vkd = s->device->device;

    /* Wait for this slot's previous submit to finish. */
    VkResult vr = vkWaitForFences(vkd, 1, &pf->in_flight, VK_TRUE, timeout);
    if (vr == VK_TIMEOUT)
        return FLUX_ERROR_TIMEOUT;
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkWaitForFences failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    /* Acquire. Offscreen surfaces have no swapchain (ADR-0013): the
     * image index is the frame slot and the fence wait above already
     * serialised its reuse. */
    uint32_t image_index = slot;
    if (s->offscreen)
        goto acquired;
    vr = vkAcquireNextImageKHR(vkd, s->swapchain, timeout, pf->image_acquired, VK_NULL_HANDLE,
                               &image_index);
    if (vr == VK_ERROR_OUT_OF_DATE_KHR) {
        return FLUX_ERROR_SURFACE_LOST; /* caller should call flux_surface_resize */
    }
    if (vr == VK_TIMEOUT || vr == VK_NOT_READY) {
        /* No swapchain image became available within the caller's timeout.
         * Not an error: the presentation engine is simply not releasing images
         * (e.g. the compositor has the display asleep or the surface occluded).
         * Report it distinctly so the caller can skip the frame and retry
         * instead of treating it as a backend failure. The acquire neither
         * signals image_acquired nor consumes an image on timeout, so no
         * cleanup is required before the next begin_frame. */
        return FLUX_ERROR_TIMEOUT;
    }
    if (vr != VK_SUCCESS && vr != VK_SUBOPTIMAL_KHR) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkAcquireNextImageKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
acquired:
    s->current_image = image_index;

    /* Reset and re-record. */
    vkResetFences(vkd, 1, &pf->in_flight);
    vkResetCommandPool(vkd, pf->pool, 0);

    /* Recycle this slot's transient ring slice. */
    s->transient.cursor[slot] = 0;

    /* Collect prior-frame timestamps before resetting this slot's
     * query pool. The fence wait above guarantees they're available. */
    pf->ts_result_count = 0;
    if (pf->query_pool && pf->ts_was_submitted && pf->ts_scope_count > 0) {
        uint64_t raw[FLUX_MAX_TIMESTAMPS_PER_FRAME * 2];
        uint32_t count = pf->ts_next;
        if (count > 0 && count <= FLUX_MAX_TIMESTAMPS_PER_FRAME * 2) {
            VkResult qr = vkGetQueryPoolResults(vkd, pf->query_pool, 0, count,
                                                sizeof(raw[0]) * count, raw, sizeof(raw[0]),
                                                VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            if (qr == VK_SUCCESS) {
                double period_ns = (double)s->device->props.limits.timestampPeriod;
                uint32_t n = pf->ts_scope_count;
                if (n > FLUX_MAX_TIMESTAMPS_PER_FRAME)
                    n = FLUX_MAX_TIMESTAMPS_PER_FRAME;
                for (uint32_t i = 0; i < n; ++i) {
                    flux_timestamp_scope *sc = &pf->ts_scopes[i];
                    if (sc->end_query == UINT32_MAX)
                        continue;
                    uint64_t b = raw[sc->begin_query];
                    uint64_t e = raw[sc->end_query];
                    pf->ts_results[pf->ts_result_count++] = (flux_timestamp_result){
                        .label = sc->label,
                        .ms = (double)(e - b) * period_ns / 1.0e6,
                    };
                }
            }
        }
    }
    pf->ts_was_submitted = false;
    pf->ts_next = 0;
    pf->ts_scope_count = 0;
    pf->ts_open_top = 0;
    if (pf->query_pool) {
        vkResetQueryPool(vkd, pf->query_pool, 0, FLUX_MAX_TIMESTAMPS_PER_FRAME * 2);
    }

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    vr = vkBeginCommandBuffer(pf->cmd, &cbbi);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkBeginCommandBuffer failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    barrier_to_color_attachment(pf->cmd, s->images[image_index]);

    /* Surface-owned frame slot. Stable pointer for the caller's
     * lifetime of this frame; cleared by flux_frame_present. Safe
     * for a single thread driving multiple surfaces (the old
     * thread_local design clobbered other surfaces' slots). */
    s->frame_slot = (struct flux_frame){
        .surface = s,
        .slot = slot,
        .recording = true,
    };
    s->frame_active = true;
    *out = &s->frame_slot;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Dynamic-rendering pass                                            */
/* ------------------------------------------------------------------ */

void flux_frame_begin_pass(flux_frame *f, const flux_pass_desc *desc) {
    if (!f || !f->surface || !f->recording)
        return;
    flux_surface *s = f->surface;
    flux_per_frame *pf = &s->frames[f->slot];

    VkRenderingAttachmentInfo color = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = s->image_views[s->current_image],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = {.color = {{0.0f, 0.0f, 0.0f, 1.0f}}},
    };

    /* If the caller supplied attachment descriptors, honour the first
     * one. Stage 2b.1 supports a single colour attachment matching
     * the swapchain; multiple attachments and caller-supplied views
     * land later. */
    if (desc && desc->color_attachment_count > 0 && desc->color_attachments) {
        const flux_pass_attachment *a = &desc->color_attachments[0];
        if (a->view != VK_NULL_HANDLE)
            color.imageView = a->view;
        color.loadOp = (a->load_op == FLUX_LOAD_CLEAR)  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                       : (a->load_op == FLUX_LOAD_LOAD) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = (a->store_op == FLUX_STORE_STORE) ? VK_ATTACHMENT_STORE_OP_STORE
                                                          : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.clearValue.color = (VkClearColorValue){{
            a->clear_color.x,
            a->clear_color.y,
            a->clear_color.z,
            a->clear_color.w,
        }};
        /* MSAA: render into the caller's multisample view and resolve into the
         * swapchain image. The multisample contents need not be stored. */
        if (a->resolve_view != VK_NULL_HANDLE) {
            /* ADR-0017: resolve into a caller-supplied view (canvas target
             * capture) rather than the swapchain. */
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = a->resolve_view;
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        } else if (a->resolve_to_surface && a->view != VK_NULL_HANDLE) {
            color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            color.resolveImageView = s->image_views[s->current_image];
            color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            color.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
    }

    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {.offset = {0, 0}, .extent = s->extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color,
    };

    /* Peer-supplied depth attachment (ADR-0001). Pipelines built with
     * a depth format (scene materials) require the pass instance to
     * carry a matching attachment. */
    VkRenderingAttachmentInfo depth;
    if (desc && desc->depth && desc->depth->view != VK_NULL_HANDLE) {
        const flux_pass_depth_attachment *da = desc->depth;
        depth = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = da->view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = (da->load_op == FLUX_LOAD_CLEAR)  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                      : (da->load_op == FLUX_LOAD_LOAD) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = (da->store_op == FLUX_STORE_STORE) ? VK_ATTACHMENT_STORE_OP_STORE
                                                          : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {da->clear_depth, da->clear_stencil}},
        };
        ri.pDepthAttachment = &depth;
    }

    /* Stencil-only attachment (ADR-0014); depth fields are ignored. */
    VkRenderingAttachmentInfo stencil;
    if (desc && desc->stencil && desc->stencil->view != VK_NULL_HANDLE) {
        const flux_pass_depth_attachment *sa = desc->stencil;
        stencil = (VkRenderingAttachmentInfo){
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = sa->view,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            .loadOp = (sa->load_op == FLUX_LOAD_CLEAR)  ? VK_ATTACHMENT_LOAD_OP_CLEAR
                      : (sa->load_op == FLUX_LOAD_LOAD) ? VK_ATTACHMENT_LOAD_OP_LOAD
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .storeOp = (sa->store_op == FLUX_STORE_STORE) ? VK_ATTACHMENT_STORE_OP_STORE
                                                          : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue = {.depthStencil = {0.0f, sa->clear_stencil}},
        };
        ri.pStencilAttachment = &stencil;
    }

    vkCmdBeginRendering(pf->cmd, &ri);
    f->pass_active = true;
}

void flux_frame_end_pass(flux_frame *f) {
    if (!f || !f->surface || !f->recording || !f->pass_active)
        return;
    flux_per_frame *pf = &f->surface->frames[f->slot];
    vkCmdEndRendering(pf->cmd);
    f->pass_active = false;
}

/* ------------------------------------------------------------------ */
/*  Submit + present                                                  */
/* ------------------------------------------------------------------ */

flux_result flux_frame_submit(flux_frame *f) {
    if (!f || !f->surface || !f->recording)
        return FLUX_ERROR_INVALID_STATE;
    flux_surface *s = f->surface;
    flux_per_frame *pf = &s->frames[f->slot];

    if (f->pass_active) {
        flux_frame_end_pass(f);
    }

    barrier_to_final(pf->cmd, s->images[s->current_image], s->offscreen);

    pf->ts_was_submitted = (pf->ts_scope_count > 0);

    VkResult vr = vkEndCommandBuffer(pf->cmd);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkEndCommandBuffer failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }

    if (s->offscreen) {
        /* No acquire/present semaphores exist to wait on or signal;
         * the fence alone orders slot reuse and readback. */
        VkCommandBufferSubmitInfo ocb = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = pf->cmd,
        };
        VkSubmitInfo2 osi = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &ocb,
        };
        pthread_mutex_lock(&s->device->queue_lock);
        vr = vkQueueSubmit2(s->device->graphics_queue, 1, &osi, pf->in_flight);
        pthread_mutex_unlock(&s->device->queue_lock);
        if (vr != VK_SUCCESS) {
            FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkQueueSubmit2 failed", vr);
            return FLUX_ERROR_BACKEND_FAILURE;
        }
        s->last_submitted_slot = f->slot;
        f->recording = false;
        return FLUX_OK;
    }

    /* Windowed path: signal the per-slot render_finished semaphore so
     * the synchronous flux_frame_present below can wait on it. */
    VkSemaphoreSubmitInfo wait = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = pf->image_acquired,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkSemaphoreSubmitInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
        .semaphore = pf->render_finished,
        .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    };
    VkCommandBufferSubmitInfo cb = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
        .commandBuffer = pf->cmd,
    };
    VkSubmitInfo2 si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
        .waitSemaphoreInfoCount = 1,
        .pWaitSemaphoreInfos = &wait,
        .commandBufferInfoCount = 1,
        .pCommandBufferInfos = &cb,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos = &signal,
    };
    pthread_mutex_lock(&s->device->queue_lock);
    vr = vkQueueSubmit2(s->device->graphics_queue, 1, &si, pf->in_flight);
    pthread_mutex_unlock(&s->device->queue_lock);
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkQueueSubmit2 failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    f->recording = false;
    return FLUX_OK;
}

flux_result flux_frame_present(flux_frame *f) {
    if (!f || !f->surface)
        return FLUX_ERROR_INVALID_STATE;
    flux_surface *s = f->surface;
    flux_per_frame *pf = &s->frames[f->slot];

    if (s->offscreen) {
        /* Nothing to present (ADR-0013); complete the frame so the
         * caller's begin → submit → present loop works unchanged. */
        s->current_frame = (s->current_frame + 1u) % s->frames_in_flight;
        s->frame_active = false;
        return FLUX_OK;
    }

    /* Synchronous present on the calling (main) thread. Mesa's WSI
     * dispatches Wayland events inside vkQueuePresentKHR; running it on
     * the same thread as the window event loop (e.g. glfwPollEvents)
     * keeps the wl_display single-threaded. The per-slot render_finished
     * semaphore signalled by flux_frame_submit gates the present on
     * rendering completion. queue_lock serialises vs concurrent submits
     * on the same queue. */
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &pf->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &s->swapchain,
        .pImageIndices = &s->current_image,
    };
    pthread_mutex_lock(&s->device->queue_lock);
    VkResult vr = vkQueuePresentKHR(s->device->graphics_queue, &pi);
    pthread_mutex_unlock(&s->device->queue_lock);

    s->current_frame = (s->current_frame + 1u) % s->frames_in_flight;
    s->frame_active = false;

    if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR)
        return FLUX_ERROR_SURFACE_LOST;
    if (vr != VK_SUCCESS) {
        FLUX_FAIL_VK(FLUX_ERROR_BACKEND_FAILURE, "vkQueuePresentKHR failed", vr);
        return FLUX_ERROR_BACKEND_FAILURE;
    }
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Frame transient + timestamps (Stage 2b.2 / 2b.3)                  */
/* ------------------------------------------------------------------ */

flux_result flux_frame_alloc_transient(flux_frame *f, size_t bytes, size_t alignment,
                                       flux_transient *out) {
    if (!f || !f->surface || !out || !f->recording)
        return FLUX_ERROR_INVALID_STATE;
    if (alignment == 0)
        alignment = 16; /* sensible default */

    flux_transient_ring *r = &f->surface->transient;
    VkDeviceSize base = (VkDeviceSize)f->slot * r->per_frame_size;
    VkDeviceSize mask = (VkDeviceSize)alignment - 1;
    VkDeviceSize cur = r->cursor[f->slot];
    VkDeviceSize abs = base + cur;
    VkDeviceSize alg = (abs + mask) & ~mask;
    VkDeviceSize end = alg + (VkDeviceSize)bytes;
    if (end > base + r->per_frame_size) {
        *out = (flux_transient){0};
        FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY, "transient ring exhausted for this frame");
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    r->cursor[f->slot] = end - base;

    *out = (flux_transient){
        .cpu = r->mapped + alg,
        .gpu_address = r->device_address + alg,
        .size = bytes,
        .alignment = alignment,
    };
    return FLUX_OK;
}

uint32_t flux_frame_index(const flux_frame *f) {
    return f ? f->slot : 0;
}

void flux_frame_timestamp_begin(flux_frame *f, const char *label) {
    if (!f || !f->surface || !f->recording)
        return;
    flux_per_frame *pf = &f->surface->frames[f->slot];
    if (!pf->query_pool)
        return;
    if (pf->ts_next + 1 >= FLUX_MAX_TIMESTAMPS_PER_FRAME * 2)
        return;
    if (pf->ts_scope_count >= FLUX_MAX_TIMESTAMPS_PER_FRAME)
        return;
    if (pf->ts_open_top >= sizeof(pf->ts_open_stack) / sizeof(pf->ts_open_stack[0]))
        return;

    uint32_t scope_idx = pf->ts_scope_count++;
    pf->ts_scopes[scope_idx] = (flux_timestamp_scope){
        .label = label,
        .begin_query = pf->ts_next++,
        .end_query = UINT32_MAX,
    };
    pf->ts_open_stack[pf->ts_open_top++] = scope_idx;
    vkCmdWriteTimestamp(pf->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pf->query_pool,
                        pf->ts_scopes[scope_idx].begin_query);
}

void flux_frame_timestamp_end(flux_frame *f) {
    if (!f || !f->surface || !f->recording)
        return;
    flux_per_frame *pf = &f->surface->frames[f->slot];
    if (!pf->query_pool || pf->ts_open_top == 0)
        return;
    if (pf->ts_next + 1 > FLUX_MAX_TIMESTAMPS_PER_FRAME * 2)
        return;

    uint32_t scope_idx = pf->ts_open_stack[--pf->ts_open_top];
    pf->ts_scopes[scope_idx].end_query = pf->ts_next++;
    vkCmdWriteTimestamp(pf->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pf->query_pool,
                        pf->ts_scopes[scope_idx].end_query);
}

flux_result flux_frame_collect_timestamps(flux_frame *f, flux_timestamp_result *out,
                                          uint32_t *inout_count) {
    if (!f || !f->surface || !inout_count)
        return FLUX_ERROR_INVALID_ARGUMENT;
    flux_per_frame *pf = &f->surface->frames[f->slot];
    uint32_t cap = *inout_count;
    uint32_t n = pf->ts_result_count < cap ? pf->ts_result_count : cap;
    if (out)
        memcpy(out, pf->ts_results, n * sizeof(*out));
    *inout_count = pf->ts_result_count;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Raw VK accessors (vulkan.h)                                       */
/* ------------------------------------------------------------------ */

VkCommandBuffer flux_frame_vk_command_buffer(const flux_frame *f) {
    if (!f || !f->surface)
        return VK_NULL_HANDLE;
    return f->surface->frames[f->slot].cmd;
}

VkImage flux_frame_vk_image(const flux_frame *f) {
    if (!f || !f->surface)
        return VK_NULL_HANDLE;
    return f->surface->images[f->surface->current_image];
}

VkImageView flux_frame_vk_image_view(const flux_frame *f) {
    if (!f || !f->surface)
        return VK_NULL_HANDLE;
    return f->surface->image_views[f->surface->current_image];
}

VkBuffer flux_frame_vk_transient_buffer(const flux_frame *f) {
    return (f && f->surface) ? f->surface->transient.buffer : VK_NULL_HANDLE;
}
