/*
 * Vulkan implementation of the canvas rendering backend (optics_gfx_canvas_backend).
 *
 * This owns every GPU touchpoint of the canvas 2D layer: per-canvas state
 * (pipeline layout, colour formats, working-space intermediate targets), the
 * render-pass envelope (barriers, attachments, resolve), scissor, pipeline
 * binding, and the draw tail (transient upload, push constants, vkCmdDraw).
 * Consecutive state-identical submits are batched into a single vkCmdDraw;
 * see the batch block in struct optics_gfx_vk_canvas.
 * The device-level pipeline cache itself lives in renderer.c
 * (get_canvas_pipeline_id); this file consumes it.
 *
 * All state is held in optics_gfx_vk_canvas, reached through optics_gfx_canvas::backend_data
 * — struct optics_gfx_canvas holds no Vulkan types.
 */
#include "backend.h"
#include "canvas_tile.h"

#include <math.h>
#include <optics/gfx/vulkan.h>
#include <stdalign.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Backend-private per-canvas state                                  */
/* ------------------------------------------------------------------ */

typedef struct canvas_owned_image {
    VkImage image;
    optics_gfx_vk_alloc alloc;
    VkImageView view;
    VkExtent2D extent;
    VkFormat format;
    VkImageLayout layout; /* UNDEFINED before first transition */
    uint32_t bindless;    /* OPTICS_GFX_BINDLESS_INVALID unless registered */
} canvas_owned_image;

typedef struct canvas_attachment_set {
    /* ADR-0069 working-space intermediate: every canvas pass renders into
     * this RGBA16F image in linear light; vk_end_pass transforms it into
     * the destination. Sampled by the output pass, so not transient. */
    canvas_owned_image linear;
} canvas_attachment_set;

#define OPTICS_GFX_CANVAS_TARGET_ATTACHMENT_CAP 16

typedef struct canvas_target_attachment_entry {
    canvas_attachment_set attachments;
    uint32_t width;
    uint32_t height;
    uint64_t last_used_serial;
} canvas_target_attachment_entry;

typedef struct canvas_target_slot {
    canvas_target_attachment_entry entries[OPTICS_GFX_CANVAS_TARGET_ATTACHMENT_CAP];
    uint32_t count;
    uint64_t use_counter;
} canvas_target_slot;

#define OPTICS_GFX_VK_MAX_LAYERS 4

typedef struct canvas_vk_layer {
    canvas_attachment_set attachments;
    float opacity;
    optics_gfx_recti area;
    optics_gfx_pass_desc parent_pass;
    optics_gfx_pass_attachment parent_color_att;
} canvas_vk_layer;

typedef struct vk_tile_job {
    uint64_t segs_gpu;
    uint32_t seg_count;
    uint32_t max_indices;
    uint32_t tile_ox;
    uint32_t tile_oy;
    uint32_t tiles_x;
    uint32_t tiles_y;
    uint32_t fill_rule;
} vk_tile_job;

typedef enum vk_op_kind : uint32_t {
    VK_OP_DRAW = 0,
    VK_OP_SAVE_LAYER = 1,
    VK_OP_RESTORE_LAYER = 2,
} vk_op_kind;

typedef struct vk_canvas_op {
    vk_op_kind kind;
    union {
        struct {
            VkPipeline pipeline;
            optics_gfx_recti scissor;
            optics_gfx_canvas_push push;
            uint32_t vertex_count;
            uint32_t tile_job_idx; /* UINT32_MAX for non-vector draws */
        } draw;
        struct {
            uint32_t depth;
            optics_gfx_recti area;
        } layer;
    };
} vk_canvas_op;

typedef struct optics_gfx_vk_canvas {
    VkPipelineLayout layout;   /* borrowed from the device canvas cache */
    VkFormat color_format;     /* active surface/target colour format */
    VkPipeline bound_pipeline; /* active recording pipeline */

    /* Per-pass pipeline lookup memo. vk_bind_program runs for every single
     * canvas submit; routing each one through get_canvas_pipeline_id takes
     * the device-wide canvas-state mutex twice (canvas_state_get_or_init)
     * plus an 8-slot linear probe, even on a cache hit. Point-field style
     * workloads issue thousands of submits per frame (a 48x32 dot field is
     * 1536), and that lock traffic was measurable as the top CPU cost of a
     * frame. The memo caches the last (id, blend) -> (layout, pipeline)
     * resolution for the current pass; the pass-fixed linear colour format
     * is captured by resetting the memo in begin_pass, so a hit is exact.
     * Falls through to the locked slow path on a miss or a first-use build. */
    struct {
        canvas_pipe_id id;
        optics_gfx_blend_mode blend;
        VkPipelineLayout layout;
        VkPipeline pipeline;
        bool valid;
    } pipeline_memo;

    /* Open draw batch. Consecutive submits whose entire draw-visible
     * state (pipeline, scissor, every push-constant byte except the
     * vertex base address) matches — and whose transient slices land
     * back-to-back in the ring — append their vertices to one run and
     * share a single vkCmdDraw, recorded at flush time into `ops`.
     * pipeline == VK_NULL_HANDLE means no batch is open. */
    struct {
        VkPipeline pipeline;
        optics_gfx_recti scissor;
        optics_gfx_canvas_push push; /* verts_address = run base          */
        VkDeviceSize end;            /* gpu address one past the run      */
        uint32_t vertex_count;
        uint32_t tile_job_idx;
    } batch;

    /* Deferred pass recording: between vk_begin_pass and vk_end_pass, draws
     * and layer transitions queue here so vk_end_pass can batch GPU compute
     * tile binning/backdrop across the pass before opening vkCmdBeginRendering. */
    vk_canvas_op *ops;
    uint32_t ops_count;
    uint32_t ops_cap;
    vk_tile_job *tile_jobs;
    uint32_t tile_job_count;
    uint32_t tile_job_cap;
    uint32_t pending_tile_job_idx;

    /* Growable per-slot device-local tile scratch buffers and cached compute pipes. */
    optics_gfx_buffer *tile_scratch[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
    size_t tile_scratch_cap[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
    optics_gfx_compute_pipeline *tile_pipes[CANVAS_TILE_PIPE_COUNT];

    /* Attachments are isolated by frame slot and destination class. A target
     * capture and the final surface pass can have different extents/formats in
     * one command buffer; neither may destroy resources recorded by the other.
     * Target attachments are pooled per slot by dimension to avoid recreation
     * thrashing across multi-target frames (e.g. HUD, panels, backdrop blur). */
    canvas_attachment_set surface_attachments[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
    canvas_target_slot target_slots[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
    canvas_attachment_set *active_attachments;

    /* Layer stack for isolated compositing (ADR-0091) */
    canvas_vk_layer layers[OPTICS_GFX_VK_MAX_LAYERS];
    uint32_t layer_top;
    optics_gfx_pass_desc current_pass;
    optics_gfx_pass_attachment current_color_att;

    /* ADR-0069 output-transform state for the active pass, filled by
     * begin_pass and consumed by end_pass (and by the LOAD seed blit). */
    optics_gfx_output_push out_encode;
    optics_gfx_output_push out_decode;
    optics_gfx_recti out_area;
    uint32_t active_slot;
    /* The active pass's damage scope, copied from the config (ADR-0149). The
     * rect array is borrowed from the caller's descriptor. `out_area` remains
     * the bounding box because the output transform's viewport/scissor is a
     * single rect; per-rect fidelity is applied by the scissor selector and by
     * the per-rect encode loop below. */
    canvas_pass_config active_config;
} optics_gfx_vk_canvas;

static inline optics_gfx_vk_canvas *vkc(optics_gfx_canvas *c) {
    return (optics_gfx_vk_canvas *)c->backend_data;
}

/* ------------------------------------------------------------------ */
/*  Owned attachments (working-space linear intermediate)             */
/* ------------------------------------------------------------------ */

/* Release an owned attachment through the device retire queue. A previous
 * extent's image may still be referenced by batches in flight on the
 * graphics queue (a resize during recording swaps the attachment while an
 * earlier frame reads it), and optics_gfx_canvas_release runs while the surface's
 * frames are in flight; destroying the VkImage inline can fault the engine
 * mid-batch. Retiring defers destruction until the queue provably passed
 * every referencing batch — no wait, no stall. */
static void owned_image_destroy(optics_gfx_canvas *c, canvas_owned_image *owned) {
    optics_gfx_device *d = c->device;
    if (owned->view || owned->image || owned->alloc.memory) {
        uint32_t bindless = owned->bindless;
        optics_gfx_device_retire_image(d, owned->view, owned->image, &owned->alloc, VK_NULL_HANDLE,
                                       0, bindless, OPTICS_GFX_BINDLESS_INVALID);
    }
    *owned = (canvas_owned_image){};
    owned->bindless = OPTICS_GFX_BINDLESS_INVALID;
}

static void attachments_destroy(optics_gfx_canvas *c, canvas_attachment_set *attachments) {
    owned_image_destroy(c, &attachments->linear);
}

/* ADR-0069: the per-slot working-space intermediate. RGBA16F, single
 * sample, sampled by the output pass — hence a bindless handle and a
 * tracked layout (COLOR_ATTACHMENT_OPTIMAL while a pass records into
 * it, SHADER_READ_ONLY_OPTIMAL in between). */
static bool linear_ensure(optics_gfx_canvas *c, canvas_attachment_set *attachments, uint32_t w,
                          uint32_t h) {
    canvas_owned_image *owned = &attachments->linear;
    if (w == 0 || h == 0)
        return false;
    if (owned->view && owned->extent.width == w && owned->extent.height == h)
        return true;

    owned_image_destroy(c, owned);

    optics_gfx_device *d = c->device;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = OPTICS_GFX_CANVAS_LINEAR_FORMAT,
        .extent = {w, h, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (optics_gfx_vk_alloc_image(d, &ici, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &owned->image,
                                  &owned->alloc) != OPTICS_GFX_OK) {
        return false;
    }
    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = owned->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = OPTICS_GFX_CANVAS_LINEAR_FORMAT,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    if (vkCreateImageView(d->device, &ivci, nullptr, &owned->view) != VK_SUCCESS) {
        owned_image_destroy(c, owned);
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "canvas linear view failed");
        return false;
    }
    if (optics_gfx_bindless_register_image(d, owned->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                           &owned->bindless) != OPTICS_GFX_OK) {
        owned_image_destroy(c, owned);
        return false;
    }
    owned->extent = (VkExtent2D){w, h};
    owned->format = OPTICS_GFX_CANVAS_LINEAR_FORMAT;
    owned->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

/* Target attachment buckets round a requested extent up to a multiple of
 * this many pixels. An animation that grows or shrinks its capture a few
 * pixels per frame then hits one pooled entry instead of thrashing
 * vkCreateImage through the LRU tail; the render area still clamps the
 * pass to the true extent. 128 keeps bucket count small (a 4K-wide
 * animation sweeps ≤17 buckets per axis) while wasting at most ~127px of
 * an intermediate's edge. */
#define OPTICS_GFX_CANVAS_TARGET_BUCKET 128

static uint32_t bucket_up(uint32_t v) {
    return (v + (OPTICS_GFX_CANVAS_TARGET_BUCKET - 1)) &
           ~(uint32_t)(OPTICS_GFX_CANVAS_TARGET_BUCKET - 1);
}

static canvas_attachment_set *target_attachments_get(optics_gfx_canvas *c, uint32_t slot,
                                                     uint32_t w, uint32_t h) {
    optics_gfx_vk_canvas *v = vkc(c);
    if (slot >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT)
        return nullptr;
    canvas_target_slot *ts = &v->target_slots[slot];
    ts->use_counter++;
    uint64_t now = ts->use_counter;
    /* Entries are bucketed, so an exact bucket match serves every extent
     * inside it — an animation sweeping its capture size a few pixels per
     * frame keeps hitting the same entry instead of churning the LRU tail. */
    uint32_t bw = bucket_up(w), bh = bucket_up(h);

    for (uint32_t i = 0; i < ts->count; ++i) {
        if (ts->entries[i].width == bw && ts->entries[i].height == bh) {
            ts->entries[i].last_used_serial = now;
            return &ts->entries[i].attachments;
        }
    }

    if (ts->count < OPTICS_GFX_CANVAS_TARGET_ATTACHMENT_CAP) {
        uint32_t idx = ts->count++;
        canvas_target_attachment_entry *entry = &ts->entries[idx];
        *entry = (canvas_target_attachment_entry){};
        entry->attachments.linear.bindless = OPTICS_GFX_BINDLESS_INVALID;
        entry->width = bw;
        entry->height = bh;
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
    *lru = (canvas_target_attachment_entry){};
    lru->attachments.linear.bindless = OPTICS_GFX_BINDLESS_INVALID;
    lru->width = bw;
    lru->height = bh;
    lru->last_used_serial = now;
    return &lru->attachments;
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

static optics_gfx_result vk_canvas_init(const optics_gfx_canvas_backend *self,
                                        optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_device *d = c->device;
    optics_gfx_vk_canvas *v = optics_gfx_internal_alloc(d, sizeof(*v));
    if (!v)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    *v = (optics_gfx_vk_canvas){};
    c->backend_data = v;

    v->color_format = optics_gfx_surface_vk_format(c->surface);
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        v->surface_attachments[i].linear.bindless = OPTICS_GFX_BINDLESS_INVALID;
        v->target_slots[i].count = 0;
        v->target_slots[i].use_counter = 0;
    }

    /* ADR-0069: the working-space intermediate must support blending —
     * without it there is no linear-light canvas on this device. */
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(d->physical_device, OPTICS_GFX_CANVAS_LINEAR_FORMAT,
                                            &fp);
        const VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT;
        if ((fp.optimalTilingFeatures & need) != need) {
            optics_gfx_internal_free(d, v);
            c->backend_data = nullptr;
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                            "device cannot blend R16G16B16A16_SFLOAT; no linear working space");
            return OPTICS_GFX_ERROR_UNSUPPORTED;
        }
    }

    /* Warm the cache (so canvas_create can fail fast on shader/layout
     * problems rather than first-draw). Build every draw pipeline kind for
     * the working-space intermediate format up front (ADR-0069: draws no
     * longer target the surface format): the glyph pipeline in particular pulls
     * in SPIR-V JIT work on first use that can take ~0.5-1.5 s on Mesa/Intel,
     * which blows any real frame deadline on the first frame containing text.
     * Warming here keeps the cost inside canvas_create and turns first-draw
     * into a hot path. */
    for (int id = 0; id < (int)CANVAS_PIPE_COUNT; id++) {
        if (id == CANVAS_PIPE_OUTPUT)
            continue; /* destination-format keyed; warmed below */
        /* Warm the default SRC_OVER pipeline for each id; non-default
         * blend variants are built lazily on first use. */
        VkPipeline warm;
        optics_gfx_result r =
            get_canvas_pipeline_id(d, OPTICS_GFX_CANVAS_LINEAR_FORMAT, (canvas_pipe_id)id,
                                   OPTICS_GFX_BLEND_SRC_OVER, &v->layout, &warm);
        if (r != OPTICS_GFX_OK) {
            optics_gfx_internal_free(d, v);
            c->backend_data = nullptr;
            return r;
        }
    }

    /* Warm the output transform for this surface's format (1x, blend off).
     * Target-pass output variants stay lazy: their formats differ per caller
     * image. */
    {
        VkPipeline warm;
        optics_gfx_result r = get_canvas_pipeline_id(d, v->color_format, CANVAS_PIPE_OUTPUT,
                                                     OPTICS_GFX_BLEND_SRC_OVER, &v->layout, &warm);
        if (r != OPTICS_GFX_OK) {
            optics_gfx_internal_free(d, v);
            c->backend_data = nullptr;
            return r;
        }
    }
    if (canvas_get_tile_compute_pipelines(d, v->tile_pipes) != OPTICS_GFX_OK) {
        optics_gfx_internal_free(d, v);
        c->backend_data = nullptr;
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    v->pending_tile_job_idx = UINT32_MAX;
    v->batch.tile_job_idx = UINT32_MAX;
    return OPTICS_GFX_OK;
}

static void vk_canvas_destroy(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_vk_canvas *v = vkc(c);
    if (!v)
        return;
    /* Owned attachments may still be referenced by in-flight frames, so
     * they go through the device retire queue (deferred destruction) —
     * never a device-wide wait here: stalling the whole GPU at canvas
     * teardown is exactly the pause class ADR-0021/0022 removed. */
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        attachments_destroy(c, &v->surface_attachments[i]);
        canvas_target_slot *ts = &v->target_slots[i];
        for (uint32_t j = 0; j < ts->count; ++j) {
            attachments_destroy(c, &ts->entries[j].attachments);
        }
        ts->count = 0;
        if (v->tile_scratch[i]) {
            optics_gfx_buffer_release(v->tile_scratch[i]);
            v->tile_scratch[i] = nullptr;
        }
    }
    for (uint32_t i = 0; i < OPTICS_GFX_VK_MAX_LAYERS; ++i) {
        attachments_destroy(c, &v->layers[i].attachments);
    }
    optics_gfx_internal_free(c->device, v->ops);
    optics_gfx_internal_free(c->device, v->tile_jobs);
    /* Pipeline + layout are owned by the device's canvas cache; we just drop
     * the borrowed references. */
    optics_gfx_internal_free(c->device, v);
    c->backend_data = nullptr;
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

/* Defined with the submit path below; end_pass must drain any open
 * vertex batch before closing the render pass. */
static void batch_flush(optics_gfx_canvas *c);

static void unpack_clear(const optics_gfx_color *clear, optics_gfx_vec4 *out) {
    *out = (optics_gfx_vec4){0, 0, 0, 0};
    if (!clear)
        return;
    /* ADR-0069: the clear value clears the linear intermediate, so it is
     * decoded to the working space here (premultiplied linear). */
    *out = optics_gfx_color_to_linear(*clear);
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

static void mat3_to_push(const optics_gfx_mat3 *m, float out[3][4]) {
    for (int col = 0; col < 3; ++col) {
        for (int row = 0; row < 3; ++row)
            out[col][row] = m->m[col * 3 + row];
        out[col][3] = 0.0f;
    }
}

/* Record the output-transform blits for a pass's disjoint damage rectangles
 * (ADR-0069, ADR-0149) inside a single Dynamic Rendering pass. `push->flags`
 * picks the direction: encode (intermediate -> destination, the end_pass tail)
 * or decode (destination -> intermediate, the LOAD seed).
 * dest_view == VK_NULL_HANDLE targets the frame's render image.
 *
 * Issuing all `area_count` scissored draws inside one pass rather than opening
 * a separate render pass per rectangle avoids `OPTICS_GFX_LOAD_DONT_CARE` tile
 * clobbering when two damage rectangles share a GPU tile during the decode
 * seed, and eliminates inter-pass write-after-write hazards during encode. */
static void record_output_blits(optics_gfx_canvas *c, VkImageView dest_view, VkFormat dest_format,
                                const optics_gfx_recti *areas, uint32_t area_count,
                                const optics_gfx_output_push *push) {
    if (!areas || area_count == 0)
        return;

    int32_t min_x = areas[0].x;
    int32_t min_y = areas[0].y;
    int32_t max_x = areas[0].x + (int32_t)areas[0].w;
    int32_t max_y = areas[0].y + (int32_t)areas[0].h;
    for (uint32_t i = 1; i < area_count; i++) {
        const int32_t x0 = areas[i].x;
        const int32_t y0 = areas[i].y;
        const int32_t x1 = areas[i].x + (int32_t)areas[i].w;
        const int32_t y1 = areas[i].y + (int32_t)areas[i].h;
        if (x0 < min_x)
            min_x = x0;
        if (y0 < min_y)
            min_y = y0;
        if (x1 > max_x)
            max_x = x1;
        if (y1 > max_y)
            max_y = y1;
    }
    if (max_x <= min_x || max_y <= min_y)
        return;

    optics_gfx_vk_canvas *v = vkc(c);
    VkPipelineLayout layout;
    VkPipeline pipeline;
    if (get_canvas_pipeline_id(c->device, dest_format, CANVAS_PIPE_OUTPUT,
                               OPTICS_GFX_BLEND_SRC_OVER, &layout, &pipeline) != OPTICS_GFX_OK) {
        if (c->pass_error == OPTICS_GFX_OK)
            c->pass_error = OPTICS_GFX_ERROR_BACKEND_FAILURE;
        return;
    }

    optics_gfx_pass_attachment att = {
        .view = dest_view,
        .format = dest_format,
        /* Encode preserves the pixels outside `areas` (partial redraw);
         * the decode seed overwrites every region the main pass will read
         * within this single pass, so its initial load is irrelevant. */
        .load_op = (push->flags & OPTICS_GFX_OUTPUT_F_DECODE) ? OPTICS_GFX_LOAD_DONT_CARE
                                                              : OPTICS_GFX_LOAD_LOAD,
        .store_op = OPTICS_GFX_STORE_STORE,
    };
    optics_gfx_pass_desc pass = {
        .type = OPTICS_GFX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
        .width = (uint32_t)(max_x - min_x),
        .height = (uint32_t)(max_y - min_y),
        .render_offset_x = min_x,
        .render_offset_y = min_y,
    };
    optics_gfx_frame_begin_pass(c->frame, &pass);

    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(c->frame);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDescriptorSet bindless = optics_gfx_device_bindless_set(c->device);
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
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(*push), push);
    for (uint32_t i = 0; i < area_count; i++) {
        VkRect2D sc = {.offset = {areas[i].x, areas[i].y}, .extent = {areas[i].w, areas[i].h}};
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        c->recorded_draws++;
    }
    optics_gfx_frame_end_pass(c->frame);
    v->bound_pipeline = VK_NULL_HANDLE; /* the blit clobbered the canvas binding */
    v->pipeline_memo.valid = false;
}

static optics_gfx_result vk_begin_pass(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                       optics_gfx_frame *f, optics_gfx_image *target,
                                       const canvas_pass_config *config) {
    (void)self;
    optics_gfx_vk_canvas *v = vkc(c);

    optics_gfx_surface_info info;
    optics_gfx_surface_get_info(c->surface, &info);
    uint32_t w = target ? target->width : info.width;
    uint32_t h = target ? target->height : info.height;
    optics_gfx_recti area;
    if (!canvas_pass_render_bounds(config, w, h, &area)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "Canvas damage scope names no pixel of the target");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* Keep the config for end_pass (per-rect encode) and for submit (per-draw
     * scissor). The rect array is borrowed from the caller's descriptor, which
     * must outlive the pass by contract. */
    v->active_config = *config;
    v->color_format =
        target ? optics_gfx_format_to_vk(target->format) : optics_gfx_surface_vk_format(c->surface);
    if (v->color_format == VK_FORMAT_UNDEFINED)
        return OPTICS_GFX_ERROR_UNSUPPORTED;
    uint32_t slot = optics_gfx_frame_index(f);
    if (slot >= OPTICS_GFX_MAX_FRAMES_IN_FLIGHT)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;
    canvas_attachment_set *attachments =
        target ? target_attachments_get(c, slot, w, h) : &v->surface_attachments[slot];
    if (!attachments) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_MEMORY,
                        "canvas target attachment pool unavailable");
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }
    v->active_attachments = attachments;

    /* Bucketed target pool (see target_attachments_get): the pooled
     * attachments — including the working-space intermediate — are sized
     * to the entry's bucket; the render area keeps every pass clamped to
     * the true extent and the output transform scales its sampling UV to
     * the written sub-rect. Surface passes keep the exact extent: the
     * intermediate is reallocated on swapchain resize either way. */
    uint32_t alloc_w = w, alloc_h = h;
    if (target) {
        alloc_w = bucket_up(w);
        alloc_h = bucket_up(h);
    }

    /* ADR-0069: every pass renders into the per-slot working-space
     * intermediate; the destination only sees pixels through the output
     * transform at end_pass. */
    if (!linear_ensure(c, attachments, alloc_w, alloc_h)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE, "canvas linear intermediate unavailable");
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    canvas_owned_image *linear = &attachments->linear;

    /* Destination colour space: what pixels are written in — the
     * negotiated surface space, or the display profile override
     * (ADR-0069 legacy-platform path) — or the render target's implied
     * content space: 16F targets are working-space linear, 8-bit targets
     * are sRGB content. */
    optics_gfx_color_space dest_space;
    if (target)
        dest_space = target->format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT
                         ? (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SCRGB
                         : (optics_gfx_color_space)OPTICS_GFX_COLOR_SPACE_SRGB;
    else
        dest_space = c->surface->content_space;

    const optics_gfx_color_space working = OPTICS_GFX_COLOR_SPACE_SCRGB;
    optics_gfx_mat3 enc, dec;
    optics_gfx_color_space_transform_matrix(working, dest_space, &enc);
    optics_gfx_color_space_transform_matrix(dest_space, working, &dec);

    uint32_t encode_flags = 0;
    float dither_levels = 255.0f;
    if (v->color_format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        encode_flags |= OPTICS_GFX_OUTPUT_F_NO_DITHER; /* float destination keeps full precision */
        dither_levels = 0.0f;
    } else if (v->color_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        dither_levels = 1023.0f;
    }

    v->out_encode = (optics_gfx_output_push){};
    mat3_to_push(&enc, v->out_encode.primaries);
    v->out_encode.image_handle = linear->bindless;
    v->out_encode.sampler_handle = optics_gfx_device_default_sampler_handle(c->device);
    v->out_encode.transfer = (uint32_t)dest_space.transfer;
    v->out_encode.flags = encode_flags;
    v->out_encode.gamma = dest_space.gamma;
    v->out_encode.dither_levels = dither_levels;
    /* BT.2408 graphics white by default; the surface's HDR desc wins. */
    float sdr_white = 203.0f;
    if (!target && c->surface->sdr_white_nits > 0.0f)
        sdr_white = c->surface->sdr_white_nits;
    v->out_encode.sdr_white_nits = sdr_white;
    v->out_encode.uv_scale[0] = (float)w / (float)alloc_w;
    v->out_encode.uv_scale[1] = (float)h / (float)alloc_h;

    v->out_decode = v->out_encode;
    mat3_to_push(&dec, v->out_decode.primaries);
    v->out_decode.flags = OPTICS_GFX_OUTPUT_F_DECODE | OPTICS_GFX_OUTPUT_F_NO_DITHER;
    v->out_decode.image_handle = OPTICS_GFX_BINDLESS_INVALID; /* filled by the seed blit */
    /* The seed samples the destination (exact pass extent), not the
     * bucketed intermediate — no sub-rect scaling on that side. */
    v->out_decode.uv_scale[0] = 1.0f;
    v->out_decode.uv_scale[1] = 1.0f;
    v->out_area = area;
    v->active_slot = slot;
    /* fb size feeds the seed blit's viewport, so publish it before the
     * LOAD block (it is re-asserted with the rest of the pass state below). */
    c->fb_width = w;
    c->fb_height = h;

    optics_gfx_vec4 cc;
    unpack_clear(config->clear_color, &cc);

    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(f);

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
     * pixels, decoded into the working space (ADR-0069).
     *
     * ADR-0149: a damage-scoped pass seeds only the damage rectangles rather
     * than the whole bounding box. The pixels between them belong to other
     * widgets and are preserved by the encode step below — seeding them here
     * would cost a full-extent read for every regional update, and copying them
     * back would overwrite whatever else changed in the destination since. */
    if (!config->clear_color) {
        uint32_t src = OPTICS_GFX_BINDLESS_INVALID;
        if (target) {
            /* First use has undefined contents — nothing meaningful to
             * load, matching the legacy load-from-undefined behaviour. */
            if (target->current_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                src = target->bindless;
        } else {
            optics_gfx_surface *s = c->surface;
            VkImage img = optics_gfx_surface_render_image(s);
            VkImageLayout *layout = optics_gfx_surface_render_layout_slot(s);
            color_barrier2(cmd, img, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                           *layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            src = optics_gfx_surface_render_bindless(s);
        }
        if (src != OPTICS_GFX_BINDLESS_INVALID) {
            v->out_decode.image_handle = src;
            /* Seed the disjoint damage rectangles (ADR-0149) inside one pass.
             * An empty set means the whole extent, which is the single-rect case. */
            optics_gfx_recti seed_areas[OPTICS_GFX_DAMAGE_CAPACITY];
            const uint32_t seed_count =
                canvas_pass_areas(config, w, h, seed_areas, OPTICS_GFX_DAMAGE_CAPACITY);
            record_output_blits(c, linear->view, OPTICS_GFX_CANVAS_LINEAR_FORMAT, seed_areas,
                                seed_count, &v->out_decode);
            /* The seed's attachment writes must be visible to the main
             * pass's load-op read of the same image. */
            color_barrier2(
                cmd, linear->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }
        if (!target) {
            optics_gfx_surface *s = c->surface;
            VkImage img = optics_gfx_surface_render_image(s);
            VkImageLayout *layout = optics_gfx_surface_render_layout_slot(s);
            color_barrier2(
                cmd, img, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            *layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }

    /* Single-sample 1x working-space intermediate with continuous analytic AA
     * (ADR-0116 / ADR-0120). */
    optics_gfx_pass_attachment att = {
        .load_op = config->clear_color ? OPTICS_GFX_LOAD_CLEAR : OPTICS_GFX_LOAD_LOAD,
        .store_op = OPTICS_GFX_STORE_STORE,
        .clear_color = cc,
        .view = linear->view,
        .format = OPTICS_GFX_CANVAS_LINEAR_FORMAT,
    };

    if (target) {
        /* The target image is no longer rendered here — only sampled by the
         * seed (above) and written by the output transform at end_pass.
         * First use starts from UNDEFINED; later passes arrive in the
         * sampleable layout established by the preceding target pass. */
        if (target->current_layout != VK_IMAGE_LAYOUT_UNDEFINED &&
            target->current_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                            "canvas target is neither new nor sampleable before begin");
            return OPTICS_GFX_ERROR_INVALID_STATE;
        }
    }

    optics_gfx_pass_desc pass = {
        .type = OPTICS_GFX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &att,
        /* The bounding box of the damage scope. The renderArea bounds the
         * load/store; the per-draw scissor narrows what is actually written. */
        .width = area.w,
        .height = area.h,
        .render_offset_x = area.x,
        .render_offset_y = area.y,
    };

    v->current_color_att = att;
    v->current_pass = pass;
    v->current_pass.color_attachments = &v->current_color_att;

    c->pass_active = true;
    f->canvas_pass_active = true;
    c->target_pass = (target != nullptr);
    c->target = target;
    c->fb_width = w;
    c->fb_height = h;
    v->active_slot = slot;
    c->states[0].scissor = area;

    v->layer_top = 0;
    v->ops_count = 0;
    v->tile_job_count = 0;
    v->pending_tile_job_idx = UINT32_MAX;
    v->bound_pipeline = VK_NULL_HANDLE;
    v->pipeline_memo.valid = false;
    v->batch.pipeline = VK_NULL_HANDLE;
    v->batch.vertex_count = 0;
    v->batch.tile_job_idx = UINT32_MAX;
    return OPTICS_GFX_OK;
}

static void vk_mem_barrier2(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage,
                            VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                            VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 mb = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = src_stage,
        .srcAccessMask = src_access,
        .dstStageMask = dst_stage,
        .dstAccessMask = dst_access,
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &mb,
    };
    vkCmdPipelineBarrier2(cmd, &di);
}

static bool vk_push_op(optics_gfx_canvas *c, optics_gfx_vk_canvas *v, const vk_canvas_op *op) {
    if (v->ops_count >= v->ops_cap) {
        uint32_t new_cap = v->ops_cap ? v->ops_cap * 2u : 64u;
        vk_canvas_op *next = optics_gfx_internal_alloc(c->device, (size_t)new_cap * sizeof(*next));
        if (!next) {
            if (c->pass_error == OPTICS_GFX_OK)
                c->pass_error = OPTICS_GFX_ERROR_OUT_OF_MEMORY;
            return false;
        }
        if (v->ops && v->ops_count > 0)
            memcpy(next, v->ops, (size_t)v->ops_count * sizeof(*next));
        optics_gfx_internal_free(c->device, v->ops);
        v->ops = next;
        v->ops_cap = new_cap;
    }
    v->ops[v->ops_count++] = *op;
    return true;
}

static bool vk_push_tile_job(optics_gfx_canvas *c, optics_gfx_vk_canvas *v, const vk_tile_job *job,
                             uint32_t *out_idx) {
    if (v->tile_job_count >= v->tile_job_cap) {
        uint32_t new_cap = v->tile_job_cap ? v->tile_job_cap * 2u : 16u;
        vk_tile_job *next = optics_gfx_internal_alloc(c->device, (size_t)new_cap * sizeof(*next));
        if (!next)
            return false;
        if (v->tile_jobs && v->tile_job_count > 0)
            memcpy(next, v->tile_jobs, (size_t)v->tile_job_count * sizeof(*next));
        optics_gfx_internal_free(c->device, v->tile_jobs);
        v->tile_jobs = next;
        v->tile_job_cap = new_cap;
    }
    *out_idx = v->tile_job_count;
    v->tile_jobs[v->tile_job_count++] = *job;
    return true;
}

static bool ensure_tile_scratch(optics_gfx_canvas *c, optics_gfx_vk_canvas *v, uint32_t slot,
                                size_t needed) {
    if (needed <= v->tile_scratch_cap[slot] && v->tile_scratch[slot])
        return true;
    size_t new_cap = v->tile_scratch_cap[slot] ? v->tile_scratch_cap[slot] : 65536u;
    while (new_cap < needed)
        new_cap *= 2u;

    optics_gfx_buffer_desc bdesc = {
        .type = OPTICS_GFX_TYPE_BUFFER_DESC,
        .size = new_cap,
        .usage = OPTICS_GFX_BUFFER_USAGE_STORAGE | OPTICS_GFX_BUFFER_USAGE_TRANSFER_DST,
        .location = OPTICS_GFX_BUFFER_GPU_LOCAL,
        .device_address = true,
    };
    optics_gfx_buffer *buf = nullptr;
    optics_gfx_result r = optics_gfx_buffer_create(c->device, &bdesc, &buf);
    if (r != OPTICS_GFX_OK)
        return false;
    if (v->tile_scratch[slot])
        optics_gfx_buffer_release(v->tile_scratch[slot]);
    v->tile_scratch[slot] = buf;
    v->tile_scratch_cap[slot] = new_cap;
    return true;
}

static size_t align256(size_t x) {
    return (x + 255u) & ~(size_t)255u;
}

static bool vk_run_tile_compute(optics_gfx_canvas *c, optics_gfx_vk_canvas *v,
                                VkCommandBuffer cmd) {
    if (v->tile_job_count == 0)
        return true;

    size_t clear_bytes = 0;
    size_t total_bytes = 0;
    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        const vk_tile_job *j = &v->tile_jobs[i];
        size_t num_tiles = (size_t)j->tiles_x * j->tiles_y;
        clear_bytes += align256(num_tiles * sizeof(optics_gfx_tile_desc));
        clear_bytes += align256(num_tiles * OPTICS_GFX_TILE_SIZE * sizeof(int32_t));
    }
    total_bytes = clear_bytes;
    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        const vk_tile_job *j = &v->tile_jobs[i];
        total_bytes += align256(((size_t)j->max_indices + 1u) * sizeof(uint32_t));
    }

    uint32_t slot = v->active_slot;
    if (!ensure_tile_scratch(c, v, slot, total_bytes)) {
        if (c->pass_error == OPTICS_GFX_OK)
            c->pass_error = OPTICS_GFX_ERROR_OUT_OF_MEMORY;
        return false;
    }

    optics_gfx_transient disp_slice;
    optics_gfx_result dr = optics_gfx_frame_alloc_transient(
        c->frame, &(optics_gfx_frame_alloc_transient_opts){
                      .bytes = (size_t)v->tile_job_count * sizeof(optics_gfx_gpu_tile_dispatch),
                      .alignment = 16,
                      .out = &disp_slice});
    if (dr != OPTICS_GFX_OK) {
        if (c->pass_error == OPTICS_GFX_OK)
            c->pass_error = dr;
        return false;
    }

    uint64_t scratch_gpu = optics_gfx_buffer_device_address(v->tile_scratch[slot]);
    VkBuffer scratch_vk = optics_gfx_buffer_vk_buffer(v->tile_scratch[slot]);
    optics_gfx_gpu_tile_dispatch *disps = disp_slice.cpu;

    size_t hdr_cursor = 0;
    size_t idx_cursor = clear_bytes;
    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        const vk_tile_job *j = &v->tile_jobs[i];
        size_t num_tiles = (size_t)j->tiles_x * j->tiles_y;
        size_t hdr_sz = align256(num_tiles * sizeof(optics_gfx_tile_desc));
        size_t bd_sz = align256(num_tiles * OPTICS_GFX_TILE_SIZE * sizeof(int32_t));
        size_t idx_sz = align256(((size_t)j->max_indices + 1u) * sizeof(uint32_t));

        disps[i] = (optics_gfx_gpu_tile_dispatch){
            .segs = j->segs_gpu,
            .tile_headers = scratch_gpu + hdr_cursor,
            .row_backdrops = scratch_gpu + hdr_cursor + hdr_sz,
            .tile_indices = scratch_gpu + idx_cursor,
            .seg_count = j->seg_count,
            .max_indices = j->max_indices,
            .tile_ox = j->tile_ox,
            .tile_oy = j->tile_oy,
            .tiles_x = j->tiles_x,
            .tiles_y = j->tiles_y,
            .fill_rule = j->fill_rule,
        };
        hdr_cursor += hdr_sz + bd_sz;
        idx_cursor += idx_sz;
    }

    for (uint32_t i = 0; i < v->ops_count; ++i) {
        vk_canvas_op *op = &v->ops[i];
        if (op->kind == VK_OP_DRAW && op->draw.tile_job_idx != UINT32_MAX) {
            op->draw.push.vector.tile_dispatch =
                disp_slice.gpu_address +
                (uint64_t)op->draw.tile_job_idx * sizeof(optics_gfx_gpu_tile_dispatch);
        }
    }

    /* Zero all tile headers and row backdrops in one device-side fill. */
    if (clear_bytes > 0)
        vkCmdFillBuffer(cmd, scratch_vk, 0, clear_bytes, 0u);
    vk_mem_barrier2(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        uint64_t addr = disp_slice.gpu_address + (uint64_t)i * sizeof(optics_gfx_gpu_tile_dispatch);
        uint32_t gx = (v->tile_jobs[i].seg_count + 63u) / 64u;
        optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){
            .cmd = cmd,
            .pipeline = v->tile_pipes[CANVAS_TILE_PIPE_BIN_COUNT],
            .push_constants = &addr,
            .push_bytes = sizeof(addr),
            .group_x = gx > 0 ? gx : 1u,
            .group_y = 1u,
            .group_z = 1u,
        });
    }
    vk_mem_barrier2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        uint64_t addr = disp_slice.gpu_address + (uint64_t)i * sizeof(optics_gfx_gpu_tile_dispatch);
        optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){
            .cmd = cmd,
            .pipeline = v->tile_pipes[CANVAS_TILE_PIPE_SCAN_OFFSETS],
            .push_constants = &addr,
            .push_bytes = sizeof(addr),
            .group_x = 1u,
            .group_y = 1u,
            .group_z = 1u,
        });
        uint32_t row_gx = (v->tile_jobs[i].tiles_y * OPTICS_GFX_TILE_SIZE + 63u) / 64u;
        optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){
            .cmd = cmd,
            .pipeline = v->tile_pipes[CANVAS_TILE_PIPE_SCAN_BACKDROP],
            .push_constants = &addr,
            .push_bytes = sizeof(addr),
            .group_x = row_gx > 0 ? row_gx : 1u,
            .group_y = 1u,
            .group_z = 1u,
        });
    }
    vk_mem_barrier2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

    for (uint32_t i = 0; i < v->tile_job_count; ++i) {
        uint64_t addr = disp_slice.gpu_address + (uint64_t)i * sizeof(optics_gfx_gpu_tile_dispatch);
        uint32_t gx = (v->tile_jobs[i].seg_count + 63u) / 64u;
        optics_gfx_compute_dispatch(&(optics_gfx_compute_dispatch_opts){
            .cmd = cmd,
            .pipeline = v->tile_pipes[CANVAS_TILE_PIPE_BIN_SCATTER],
            .push_constants = &addr,
            .push_bytes = sizeof(addr),
            .group_x = gx > 0 ? gx : 1u,
            .group_y = 1u,
            .group_z = 1u,
        });
    }
    vk_mem_barrier2(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
    return true;
}

static void begin_replay_pass_state(optics_gfx_canvas *c, optics_gfx_vk_canvas *v,
                                    VkCommandBuffer cmd, VkPipeline *cur_pipe) {
    VkViewport vp = {.x = 0.0f,
                     .y = 0.0f,
                     .width = (float)c->fb_width,
                     .height = (float)c->fb_height,
                     .minDepth = 0.0f,
                     .maxDepth = 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    VkDescriptorSet bindless = optics_gfx_device_bindless_set(c->device);
    if (bindless != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout, 0, 1, &bindless, 0,
                                nullptr);
    }
    *cur_pipe = VK_NULL_HANDLE;
}

static void vk_end_pass(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    if (!c->pass_active)
        return;
    batch_flush(c);

    optics_gfx_vk_canvas *v = vkc(c);
    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(c->frame);

    bool compute_ok = vk_run_tile_compute(c, v, cmd);

    optics_gfx_pass_desc saved_pass[OPTICS_GFX_VK_MAX_LAYERS];
    optics_gfx_pass_attachment saved_col[OPTICS_GFX_VK_MAX_LAYERS];

    optics_gfx_pass_desc active_pass = v->current_pass;
    optics_gfx_pass_attachment active_col = v->current_color_att;
    active_pass.color_attachments = &active_col;

    optics_gfx_frame_begin_pass(c->frame, &active_pass);
    VkPipeline cur_pipe = VK_NULL_HANDLE;
    begin_replay_pass_state(c, v, cmd, &cur_pipe);

    if (compute_ok) {
        for (uint32_t i = 0; i < v->ops_count; ++i) {
            const vk_canvas_op *op = &v->ops[i];
            switch (op->kind) {
            case VK_OP_DRAW: {
                if (op->draw.pipeline != cur_pipe) {
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, op->draw.pipeline);
                    cur_pipe = op->draw.pipeline;
                }
                VkRect2D sc = {.offset = {op->draw.scissor.x, op->draw.scissor.y},
                               .extent = {op->draw.scissor.w, op->draw.scissor.h}};
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdPushConstants(cmd, v->layout,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(op->draw.push), &op->draw.push);
                vkCmdDraw(cmd, op->draw.vertex_count, 1, 0, 0);
                c->recorded_draws++;
                break;
            }
            case VK_OP_SAVE_LAYER: {
                uint32_t depth = op->layer.depth;
                optics_gfx_frame_end_pass(c->frame);
                saved_pass[depth] = active_pass;
                saved_col[depth] = active_col;

                canvas_owned_image *layer_linear = &v->layers[depth].attachments.linear;
                color_barrier2(cmd, layer_linear->image,
                               layer_linear->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? 0
                                   : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                               layer_linear->layout == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? 0
                                   : VK_ACCESS_2_SHADER_READ_BIT,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, layer_linear->layout,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                layer_linear->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

                active_col = (optics_gfx_pass_attachment){
                    .load_op = OPTICS_GFX_LOAD_CLEAR,
                    .store_op = OPTICS_GFX_STORE_STORE,
                    .clear_color = {0.0f, 0.0f, 0.0f, 0.0f},
                    .view = layer_linear->view,
                    .format = OPTICS_GFX_CANVAS_LINEAR_FORMAT,
                };
                active_pass = (optics_gfx_pass_desc){
                    .type = OPTICS_GFX_TYPE_PASS_DESC,
                    .color_attachment_count = 1,
                    .color_attachments = &active_col,
                    .width = op->layer.area.w > 0 ? op->layer.area.w : 1u,
                    .height = op->layer.area.h > 0 ? op->layer.area.h : 1u,
                    .render_offset_x = op->layer.area.x,
                    .render_offset_y = op->layer.area.y,
                };
                optics_gfx_frame_begin_pass(c->frame, &active_pass);
                begin_replay_pass_state(c, v, cmd, &cur_pipe);
                break;
            }
            case VK_OP_RESTORE_LAYER: {
                uint32_t depth = op->layer.depth;
                optics_gfx_frame_end_pass(c->frame);
                canvas_owned_image *layer_linear = &v->layers[depth].attachments.linear;
                color_barrier2(cmd, layer_linear->image,
                               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                               layer_linear->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                layer_linear->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                active_pass = saved_pass[depth];
                active_col = saved_col[depth];
                active_col.load_op = OPTICS_GFX_LOAD_LOAD;
                active_pass.color_attachments = &active_col;
                optics_gfx_frame_begin_pass(c->frame, &active_pass);
                begin_replay_pass_state(c, v, cmd, &cur_pipe);
                break;
            }
            }
        }
    }

    optics_gfx_frame_end_pass(c->frame);
    c->pass_active = false;
    c->frame->canvas_pass_active = false;
    v->ops_count = 0;
    v->tile_job_count = 0;
    v->layer_top = 0;

    canvas_attachment_set *attachments = v->active_attachments;
    if (!attachments) {
        attachments = c->target
                          ? target_attachments_get(c, v->active_slot, c->fb_width, c->fb_height)
                          : &v->surface_attachments[v->active_slot];
    }
    v->active_attachments = nullptr;
    if (!attachments)
        return;
    canvas_owned_image *linear = &attachments->linear;

    /* Intermediate: attachment -> sampled by the output transform. */
    color_barrier2(cmd, linear->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    linear->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    optics_gfx_image *t = c->target;
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

    /* Output transform into the destination (NULL view = the frame's render
     * image, filled by optics_gfx_frame_begin_pass).
     *
     * ADR-0149: encode each damage rectangle, not their bounding box. Writing
     * the box would copy the intermediate's between-band pixels — which were
     * never seeded with this destination's contents — over whatever the other
     * widgets had put there, which is precisely the corruption a naive
     * "one scissor for the union" scheme produces. A pass that covers its whole
     * destination has exactly one area, so this is one blit as before. */
    {
        optics_gfx_recti areas[OPTICS_GFX_DAMAGE_CAPACITY];
        const uint32_t n = canvas_pass_areas(&v->active_config, c->fb_width, c->fb_height, areas,
                                             OPTICS_GFX_DAMAGE_CAPACITY);
        record_output_blits(c, t ? t->view : VK_NULL_HANDLE, v->color_format, areas, n,
                            &v->out_encode);
    }

    if (!t)
        return;

    /* Trailing barrier: COLOR_ATTACHMENT -> SHADER_READ_ONLY so a following
     * optics_gfx_effect_blur / draw_image needs no caller sync. */
    color_barrier2(cmd, t->image, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                   VK_ACCESS_2_SHADER_READ_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

static void vk_set_scissor(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                           optics_gfx_recti clip) {
    (void)self;
    (void)c;
    (void)clip;
}

/* ------------------------------------------------------------------ */
/*  Program bind + batch submit                                       */
/* ------------------------------------------------------------------ */

static bool vk_bind_program(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                            canvas_pipe_id id) {
    (void)self;
    optics_gfx_vk_canvas *v = vkc(c);
    VkPipelineLayout layout;
    VkPipeline pipeline;
    /* ADR-0069: draws always render into the working-space intermediate,
     * so pipelines are keyed by its format — never the destination's.
     * The memo answers repeats of the current pass's last resolution
     * without touching the device-wide pipeline cache lock. */
    if (v->pipeline_memo.valid && v->pipeline_memo.id == id &&
        v->pipeline_memo.blend == c->pending_blend) {
        layout = v->pipeline_memo.layout;
        pipeline = v->pipeline_memo.pipeline;
    } else if (get_canvas_pipeline_id(c->device, OPTICS_GFX_CANVAS_LINEAR_FORMAT, id,
                                      c->pending_blend, &layout, &pipeline) != OPTICS_GFX_OK) {
        return false;
    } else {
        v->pipeline_memo.id = id;
        v->pipeline_memo.blend = c->pending_blend;
        v->pipeline_memo.layout = layout;
        v->pipeline_memo.pipeline = pipeline;
        v->pipeline_memo.valid = true;
    }
    if (v->bound_pipeline != pipeline) {
        /* A pipeline change ends the open batch: its op must be
         * queued before the new pipeline, so batched vertices execute
         * under the pipeline they were submitted with. */
        batch_flush(c);
        v->bound_pipeline = pipeline;
    }
    return true;
}

/* Queue the open batch as one VK_OP_DRAW op and close the batch. */
static void batch_flush(optics_gfx_canvas *c) {
    optics_gfx_vk_canvas *v = vkc(c);
    if (v->batch.pipeline == VK_NULL_HANDLE || v->batch.vertex_count == 0) {
        v->batch.pipeline = VK_NULL_HANDLE;
        v->batch.vertex_count = 0;
        v->batch.tile_job_idx = UINT32_MAX;
        return;
    }
    vk_push_op(c, v,
               &(vk_canvas_op){
                   .kind = VK_OP_DRAW,
                   .draw =
                       {
                           .pipeline = v->batch.pipeline,
                           .scissor = v->batch.scissor,
                           .push = v->batch.push,
                           .vertex_count = v->batch.vertex_count,
                           .tile_job_idx = v->batch.tile_job_idx,
                       },
               });
    v->batch.pipeline = VK_NULL_HANDLE;
    v->batch.vertex_count = 0;
    v->batch.tile_job_idx = UINT32_MAX;
}

/* Every push-constant byte after verts_address must match for two
 * submits to share a draw: paint kind, gradient stops, image/sampler
 * bindless handles, UV remap. build_push zero-initialises the whole
 * block, so padding compares deterministically; a false mismatch only
 * splits a batch, never corrupts one. */
static bool push_params_equal(const optics_gfx_canvas_push *a, const optics_gfx_canvas_push *b) {
    return memcmp((const char *)a + offsetof(optics_gfx_canvas_push, inv_window_size),
                  (const char *)b + offsetof(optics_gfx_canvas_push, inv_window_size),
                  sizeof(optics_gfx_canvas_push) -
                      offsetof(optics_gfx_canvas_push, inv_window_size)) == 0;
}

/* The scissor one draw must use: its logical clip narrowed to the pass's damage
 * scope. Returns false when the draw touches no damage and must be dropped.
 *
 * Doing this per draw (rather than relying on the pass's bounding-box scissor)
 * is what makes a regional update actually regional: within one pass the
 * bounding box of a top band and a bottom dock spans nearly the whole output,
 * and a scissor of that box would write the widgets in between. */
static bool vk_draw_scissor(optics_gfx_canvas *c, optics_gfx_recti clip, optics_gfx_recti *out) {
    optics_gfx_vk_canvas *v = vkc(c);
    return canvas_damage_select(&v->active_config, clip, out);
}

static void vk_submit(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                      canvas_pipe_id id, const optics_gfx_canvas_push *push,
                      const optics_gfx_canvas_vertex *verts, uint32_t vertex_count) {
    optics_gfx_vk_canvas *v = vkc(c);
    uint32_t tile_job_idx = v->pending_tile_job_idx;
    v->pending_tile_job_idx = UINT32_MAX;

    if (!c->recording || vertex_count == 0)
        return;
    if (!vk_bind_program(self, c, id))
        return;
    c->submit_calls++;

    /* Cull before touching the transient ring: a draw whose entire extent lies
     * outside the pass's damage scope writes pixels nobody will read, and the
     * destination already holds a complete frame there by contract. */
    optics_gfx_recti clip;
    if (!vk_draw_scissor(c, c->states[c->state_top].scissor, &clip)) {
        c->dropped_draws++;
        return;
    }

    /* Vertices travel through the frame's transient ring; the shader reads
     * them by buffer-reference address (push.verts_address). */
    optics_gfx_transient slice;
    optics_gfx_result r = optics_gfx_frame_alloc_transient(
        c->frame, &(optics_gfx_frame_alloc_transient_opts){
                      .bytes = vertex_count * sizeof(optics_gfx_canvas_vertex),
                      .alignment = alignof(optics_gfx_canvas_vertex),
                      .out = &slice,
                  });
    if (r != OPTICS_GFX_OK) {
        c->dropped_draws++;
        return;
    }
    memcpy(slice.cpu, verts, vertex_count * sizeof(optics_gfx_canvas_vertex));

    optics_gfx_canvas_push pc = *push;
    pc.verts_address = slice.gpu_address;

    /* Append to the open batch when all draw-visible state matches and
     * the new slice continues the batch's ring run. The ring is a bump
     * allocator and optics_gfx_canvas_vertex is 16-byte/4-aligned, so slices
     * from back-to-back canvas submits are exactly contiguous; any
     * interleaved allocation or a wrap breaks contiguity and flushes. */
    if (tile_job_idx == UINT32_MAX && v->batch.tile_job_idx == UINT32_MAX &&
        v->batch.pipeline == v->bound_pipeline && v->batch.end == slice.gpu_address &&
        v->batch.scissor.x == clip.x && v->batch.scissor.y == clip.y &&
        v->batch.scissor.w == clip.w && v->batch.scissor.h == clip.h &&
        push_params_equal(&v->batch.push, &pc)) {
        v->batch.vertex_count += vertex_count;
        v->batch.end += vertex_count * sizeof(optics_gfx_canvas_vertex);
        return;
    }

    /* State changed or the ring broke the run: queue what accumulated,
     * then open a new batch. */
    batch_flush(c);
    v->batch.pipeline = v->bound_pipeline;
    v->batch.scissor = clip;
    v->batch.push = pc;
    v->batch.end = slice.gpu_address + vertex_count * sizeof(optics_gfx_canvas_vertex);
    v->batch.vertex_count = vertex_count;
    v->batch.tile_job_idx = tile_job_idx;
}

static optics_gfx_result vk_save_layer(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                                       const optics_gfx_rect *bounds, float opacity) {
    (void)self;
    (void)bounds;
    optics_gfx_vk_canvas *v = vkc(c);
    if (!v || !c->frame || !v->active_attachments)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (v->layer_top >= OPTICS_GFX_VK_MAX_LAYERS)
        return OPTICS_GFX_ERROR_OUT_OF_RANGE;

    batch_flush(c);

    /* ADR-0149: a layer inside a damage-scoped pass owns only the damaged part
     * of its clip. */
    optics_gfx_recti area;
    if (!vk_draw_scissor(c, c->states[c->state_top].scissor, &area)) {
        area = (optics_gfx_recti){0, 0, 0, 0};
    }
    uint32_t w = c->fb_width;
    uint32_t h = c->fb_height;
    if (area.x < 0) {
        area.w = area.w > (uint32_t)(-area.x) ? area.w - (uint32_t)(-area.x) : 0;
        area.x = 0;
    }
    if (area.y < 0) {
        area.h = area.h > (uint32_t)(-area.y) ? area.h - (uint32_t)(-area.y) : 0;
        area.y = 0;
    }
    if ((uint32_t)area.x >= w || (uint32_t)area.y >= h) {
        area.x = 0;
        area.y = 0;
        area.w = 0;
        area.h = 0;
    } else {
        if ((uint32_t)area.x + area.w > w)
            area.w = w - (uint32_t)area.x;
        if ((uint32_t)area.y + area.h > h)
            area.h = h - (uint32_t)area.y;
    }

    uint32_t layer_idx = v->layer_top;
    canvas_vk_layer *layer = &v->layers[layer_idx];
    layer->opacity = opacity;
    layer->area = area;

    bool was_cached = layer->attachments.linear.image != VK_NULL_HANDLE;
    if (!linear_ensure(c, &layer->attachments, w, h)) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_BACKEND_FAILURE,
                        "canvas layer linear attachment unavailable");
        return OPTICS_GFX_ERROR_BACKEND_FAILURE;
    }
    if (was_cached)
        c->transient_bytes_aliased_saved += (size_t)w * h * 8;

    vk_push_op(c, v,
               &(vk_canvas_op){
                   .kind = VK_OP_SAVE_LAYER,
                   .layer = {.depth = layer_idx, .area = area},
               });

    v->pipeline_memo.valid = false;
    v->bound_pipeline = VK_NULL_HANDLE;
    v->layer_top++;
    return OPTICS_GFX_OK;
}

static void vk_restore_layer(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c) {
    (void)self;
    optics_gfx_vk_canvas *v = vkc(c);
    if (!v || v->layer_top == 0 || !c->frame)
        return;

    batch_flush(c);

    v->layer_top--;
    uint32_t layer_idx = v->layer_top;
    canvas_vk_layer *layer = &v->layers[layer_idx];
    canvas_owned_image *layer_linear = &layer->attachments.linear;

    vk_push_op(c, v,
               &(vk_canvas_op){
                   .kind = VK_OP_RESTORE_LAYER,
                   .layer = {.depth = layer_idx},
               });

    /* Resume parent pass recording state */
    v->pipeline_memo.valid = false;
    v->bound_pipeline = VK_NULL_HANDLE;

    /* Composite scissor-bounded layer region onto parent with layer->opacity modulation */
    if (layer->area.w > 0 && layer->area.h > 0 && c->fb_width > 0 && c->fb_height > 0) {
        float opacity = fminf(fmaxf(layer->opacity, 0.0f), 1.0f);
        uint32_t a = (uint32_t)(255.0f * opacity + 0.5f);
        uint32_t tint = (a << 24) | (a << 16) | (a << 8) | a;

        optics_gfx_mat3x2 saved_tx = c->states[c->state_top].transform;
        c->states[c->state_top].transform = optics_gfx_mat3x2_identity();

        optics_gfx_rect dst = {
            (float)layer->area.x,
            (float)layer->area.y,
            (float)layer->area.w,
            (float)layer->area.h,
        };
        optics_gfx_rect src = {
            (float)layer->area.x / (float)c->fb_width,
            (float)layer->area.y / (float)c->fb_height,
            (float)layer->area.w / (float)c->fb_width,
            (float)layer->area.h / (float)c->fb_height,
        };

        draw_image_with_sampler_handle(c, nullptr, layer_linear->bindless, nullptr,
                                       optics_gfx_device_default_sampler_handle(c->device), dst,
                                       src, tint, OPTICS_GFX_BLEND_SRC_OVER, 3u);
        batch_flush(c);

        c->states[c->state_top].transform = saved_tx;
    }
}

/* ------------------------------------------------------------------ */
/*  Tiled analytic coverage route (ADR-0116 / ADR-0120)                */
/* ------------------------------------------------------------------ */

static bool cover_paint_supported(const canvas_paint *paint) {
    return paint->kind == CANVAS_PAINT_SOLID || paint->kind == CANVAS_PAINT_LINEAR_GRADIENT ||
           paint->kind == CANVAS_PAINT_RADIAL_GRADIENT || paint->kind == CANVAS_PAINT_IMAGE;
}

static optics_gfx_result cover_prepare_image(optics_gfx_canvas *c, const canvas_paint *paint) {
    if (paint->kind != CANVAS_PAINT_IMAGE)
        return OPTICS_GFX_OK;
    optics_gfx_image *img = paint->gradient.image.image;
    optics_gfx_sampler *sampler = paint->gradient.image.sampler;
    if (img && c->frame && optics_gfx_plan_image_pending(&c->frame->plan, img)) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_STATE,
            "canvas sampled a deferred effect output inside a pass; call "
            "optics_gfx_frame_materialize_image before optics_gfx_canvas_begin (ADR-0147)");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (img && !canvas_track_foreign_image(c, img))
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    if (sampler && !canvas_track_sampler(c, sampler))
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    if (img)
        canvas_record_retain_image(c, img);
    if (sampler)
        canvas_record_retain_sampler(c, sampler);
    return OPTICS_GFX_OK;
}

/* Pixel rectangle covered by `bounds`, intersected with `clip` and [0, fb). */
static bool cover_area(const optics_gfx_canvas *c, optics_gfx_rect bounds, optics_gfx_recti clip,
                       int *x0, int *y0, int *x1, int *y1) {
    *x0 = (int)floorf(bounds.x);
    *y0 = (int)floorf(bounds.y);
    *x1 = (int)ceilf(bounds.x + bounds.w);
    *y1 = (int)ceilf(bounds.y + bounds.h);
    const int min_x = clip.x > 0 ? clip.x : 0;
    const int min_y = clip.y > 0 ? clip.y : 0;
    int max_x = clip.x + (int)clip.w;
    int max_y = clip.y + (int)clip.h;
    if (max_x > (int)c->fb_width)
        max_x = (int)c->fb_width;
    if (max_y > (int)c->fb_height)
        max_y = (int)c->fb_height;
    if (*x0 < min_x)
        *x0 = min_x;
    if (*y0 < min_y)
        *y0 = min_y;
    if (*x1 > max_x)
        *x1 = max_x;
    if (*y1 > max_y)
        *y1 = max_y;
    return *x1 > *x0 && *y1 > *y0;
}

/* Queue a tiled vector draw over [x0, x1) x [y0, y1) for the segment list
 * at (`segs_cpu`, `segs_address`). GPU compute bins & computes per-row backdrops
 * for all queued draws in one batch at vk_end_pass. */
static void cover_draw(const optics_gfx_canvas_backend *self, optics_gfx_canvas *c,
                       const canvas_paint *paint, optics_gfx_color solid,
                       const optics_gfx_tile_segment *segs_cpu, uint64_t segs_address,
                       uint32_t n_segs, optics_gfx_fill_rule rule, int x0, int y0, int x1, int y1) {
    optics_gfx_vk_canvas *v = vkc(c);

    const uint32_t tile_ox = (uint32_t)x0 / OPTICS_GFX_TILE_SIZE;
    const uint32_t tile_oy = (uint32_t)y0 / OPTICS_GFX_TILE_SIZE;
    const uint32_t tiles_x =
        ((uint32_t)(x1 + (int)OPTICS_GFX_TILE_SIZE - 1) / OPTICS_GFX_TILE_SIZE) - tile_ox;
    const uint32_t tiles_y =
        ((uint32_t)(y1 + (int)OPTICS_GFX_TILE_SIZE - 1) / OPTICS_GFX_TILE_SIZE) - tile_oy;
    const uint32_t max_indices =
        canvas_tile_estimate_max_indices(segs_cpu, n_segs, tile_ox, tile_oy, tiles_x, tiles_y);

    uint32_t job_idx = 0;
    if (!vk_push_tile_job(c, v,
                          &(vk_tile_job){
                              .segs_gpu = segs_address,
                              .seg_count = n_segs,
                              .max_indices = max_indices,
                              .tile_ox = tile_ox,
                              .tile_oy = tile_oy,
                              .tiles_x = tiles_x,
                              .tiles_y = tiles_y,
                              .fill_rule = (rule == OPTICS_GFX_FILL_EVEN_ODD) ? 1u : 0u,
                          },
                          &job_idx)) {
        return;
    }

    optics_gfx_canvas_push pc;
    build_push(c, paint, &pc);
    pc.vector.tile_dispatch = (uint64_t)job_idx;

    const uint32_t color = (paint->kind == CANVAS_PAINT_SOLID || paint->kind == CANVAS_PAINT_IMAGE)
                               ? solid
                               : 0xFFFFFFFFu;
    const float fx0 = (float)x0, fy0 = (float)y0, fx1 = (float)x1, fy1 = (float)y1;
    const optics_gfx_canvas_vertex quad[6] = {
        {{fx0, fy0}, color, 0}, {{fx1, fy0}, color, 0}, {{fx1, fy1}, color, 0},
        {{fx0, fy0}, color, 0}, {{fx1, fy1}, color, 0}, {{fx0, fy1}, color, 0},
    };
    c->pending_blend = paint->blend;
    v->pending_tile_job_idx = job_idx;
    self->submit(self, c, CANVAS_PIPE_VECTOR, &pc, quad, 6);
}

static optics_gfx_result vk_submit_cover(const optics_gfx_canvas_backend *self,
                                         optics_gfx_canvas *c, const optics_gfx_point *pts,
                                         const optics_gfx_canvas_contour *cons, uint32_t ncons,
                                         optics_gfx_fill_rule rule, const canvas_paint *paint,
                                         optics_gfx_recti clip) {
    if (!c || !pts || !cons || ncons == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (clip.w <= 0 || clip.h <= 0)
        return OPTICS_GFX_OK;
    if (!cover_paint_supported(paint))
        return OPTICS_GFX_ERROR_UNSUPPORTED;

    optics_gfx_recti draw_clip;
    if (!vk_draw_scissor(c, clip, &draw_clip))
        return OPTICS_GFX_OK;

    optics_gfx_rect bounds;
    const uint32_t n_segs = canvas_tile_extract_segments(pts, cons, ncons, nullptr, 0, &bounds);
    if (n_segs == 0)
        return OPTICS_GFX_OK;

    int x0, y0, x1, y1;
    if (!cover_area(c, bounds, draw_clip, &x0, &y0, &x1, &y1))
        return OPTICS_GFX_OK;

    optics_gfx_result ir = cover_prepare_image(c, paint);
    if (ir != OPTICS_GFX_OK)
        return ir;

    /* Segments go straight into the frame's transient ring: no heap, no
     * intermediate copy, released with the frame. */
    optics_gfx_transient slice;
    optics_gfx_result r = optics_gfx_frame_alloc_transient(
        c->frame, &(optics_gfx_frame_alloc_transient_opts){.bytes = (size_t)n_segs *
                                                                    sizeof(optics_gfx_tile_segment),
                                                           .alignment = 16,
                                                           .out = &slice});
    if (r != OPTICS_GFX_OK)
        return r;
    (void)canvas_tile_extract_segments(pts, cons, ncons, slice.cpu, n_segs, nullptr);

    cover_draw(self, c, paint, paint->color, slice.cpu, slice.gpu_address, n_segs, rule, x0, y0, x1,
               y1);
    return OPTICS_GFX_OK;
}

static const optics_gfx_canvas_backend vk_backend = {
    .name = "vulkan",
    .canvas_init = vk_canvas_init,
    .canvas_destroy = vk_canvas_destroy,
    .begin_pass = vk_begin_pass,
    .end_pass = vk_end_pass,
    .set_scissor = vk_set_scissor,
    .save_layer = vk_save_layer,
    .restore_layer = vk_restore_layer,
    .bind_program = vk_bind_program,
    .submit = vk_submit,
    .submit_cover = vk_submit_cover,
};

const optics_gfx_canvas_backend *optics_gfx_canvas_backend_vk(void) {
    return &vk_backend;
}
