/*
 * material/mica.c — mica foundation material implementation.
 *
 * Evaluates screen-anchored wallpaper sampling, luminosity plate balance,
 * tinting, procedural noise, border rim, drop shadow, and inactive state fallback.
 */

#include "material_frame.h"
#include "material_observe.h"
#include <optics/material/mica.h>

#include <optics/gfx.h>
#include <optics/gfx/compute.h>
#include <optics/gfx/profile.h>
#include <optics/gfx/vulkan.h>
#include <vulkan/vulkan.h>

#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MICA_MAX_GROUPS 64u

typedef struct mica_push {
    uint32_t wallpaper_handle;
    uint32_t blurred_handle;
    uint32_t sampler_handle;
    uint32_t output_handle;
    uint32_t width;
    uint32_t height;
    uint32_t origin_x;
    uint32_t origin_y;
    float bounds[4];
    float corner_radius;
    float opacity;
    float tint_opacity;
    float luminosity_plate;
    float luminosity_opacity;
    float noise_intensity;
    float border_width;
    float border_alpha;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    uint32_t group_width;
    uint32_t group_height;
    float screen_origin_x;
    float screen_origin_y;
    float screen_width;
    float screen_height;
    uint32_t fallback_color;
    float fallback_weight;
    uint32_t is_alt;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
} mica_push;

static_assert(sizeof(mica_push) == 148, "mica_push size mismatch");

typedef struct mica_filter_slot {
    optics_material__slab slab; /* the output, on the slab ladder (material_frame.h) */
    bool initialized;
    uint32_t slab_x;
    uint32_t slab_y;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t previous_count;
    optics_material__rect previous[MICA_MAX_GROUPS];
} mica_filter_slot;

struct optics_material_mica_filter {
    atomic_uint ref_count;
    optics_gfx_device *device;
    optics_gfx_compute_pipeline *mica_pipelines[2];
    optics_gfx_compute_pipeline *clear_pipelines[2];
    mica_filter_slot slots[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
};

optics_gfx_result optics_material_mica_filter_create(optics_gfx_device *device,
                                                     optics_material_mica_filter **out) {
    if (!device || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    /* Caller's allocator, attributed to material (ADR-0135). */
    optics_material_mica_filter *filter = nullptr;
    if (optics_gfx_device_alloc(device,
                                &(optics_gfx_device_alloc_opts){
                                    .type = OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS,
                                    .bytes = sizeof(*filter),
                                    .owner = OPTICS_MATERIAL_OBSERVE_OWNER,
                                    .kind = OPTICS_MATERIAL_OBSERVE_KIND_MICA,
                                    .scope = OPTICS_GFX_SCOPE_DEVICE,
                                },
                                (void **)&filter) != OPTICS_GFX_OK)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = optics_gfx_device_retain(device);
    *out = filter;
    return OPTICS_GFX_OK;
}

optics_material_mica_filter *
optics_material_mica_filter_retain(optics_material_mica_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void optics_material_mica_filter_release(optics_material_mica_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *device = filter->device;
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        optics_material__slab_release(&filter->slots[i].slab);
    }
    for (int i = 0; i < 2; ++i) {
        if (filter->mica_pipelines[i])
            optics_gfx_compute_pipeline_release(filter->mica_pipelines[i]);
        if (filter->clear_pipelines[i])
            optics_gfx_compute_pipeline_release(filter->clear_pipelines[i]);
    }
    optics_gfx_device_free(device, &(optics_gfx_device_free_opts){
                                       .type = OPTICS_GFX_TYPE_DEVICE_FREE_OPTS,
                                       .ptr = filter,
                                       .owner = OPTICS_MATERIAL_OBSERVE_OWNER,
                                       .kind = OPTICS_MATERIAL_OBSERVE_KIND_MICA,
                                       .scope = OPTICS_GFX_SCOPE_DEVICE,
                                   });
    optics_gfx_device_release(device);
}

static optics_gfx_result mica_ensure_slot(optics_material_mica_filter *filter, uint32_t index,
                                          const optics_gfx_image *input, uint32_t footprint_w,
                                          uint32_t footprint_h) {
    mica_filter_slot *slot = &filter->slots[index];
    bool recreated = false;
    optics_gfx_result r = optics_material__slab_ensure(
        filter->device, &slot->slab,
        &(optics_material__slab_opts){
            .input = input, .footprint_width = footprint_w, .footprint_height = footprint_h},
        &recreated);
    if (recreated) /* a new image starts with no previous regions to clear */
        *slot = (mica_filter_slot){.slab = slot->slab};
    return r;
}

static optics_gfx_result mica_ensure_pipelines(optics_material_mica_filter *filter, bool is16f,
                                               bool need_clear, bool need_mica) {
    const int cls = is16f ? 1 : 0;
    optics_gfx_result r = OPTICS_GFX_OK;
    if (need_clear)
        r = optics_material__pipeline_ensure(
            filter->device, &filter->clear_pipelines[cls],
            &(optics_material__pipeline_opts){
                .shader = OPTICS_MATERIAL__SHADER_STORAGE_CLEAR,
                .is16f = is16f,
                .push_constant_bytes = optics_material__storage_clear_push_bytes(),
            });
    if (r == OPTICS_GFX_OK && need_mica)
        r = optics_material__pipeline_ensure(filter->device, &filter->mica_pipelines[cls],
                                             &(optics_material__pipeline_opts){
                                                 .shader = OPTICS_MATERIAL__SHADER_MICA,
                                                 .is16f = is16f,
                                                 .push_constant_bytes = sizeof(mica_push),
                                             });
    return r;
}

optics_gfx_result optics_material_mica_filter_warmup(optics_material_mica_filter *filter,
                                                     const optics_material_mica_warmup_opts *opts) {
    if (!filter)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    bool is16f = false;
    if (opts) {
        if (opts->format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT)
            is16f = true;
    }

    return mica_ensure_pipelines(filter, is16f, true, true);
}

static bool mica_group_dispatch_bounds(const optics_material_mica_group *group,
                                       uint32_t image_width, uint32_t image_height,
                                       optics_material__rect *out) {
    if (!group || !out || image_width == 0 || image_height == 0)
        return false;
    float pad = fmaxf(group->shadow_blur * 2.0f, 2.0f);
    int64_t x0 = (int64_t)floorf(group->shape.bounds.x - pad);
    int64_t y0 = (int64_t)floorf(group->shape.bounds.y - pad);
    if (group->shadow_offset_y < 0.0f)
        y0 += (int64_t)floorf(group->shadow_offset_y);
    int64_t x1 = (int64_t)ceilf(group->shape.bounds.x + group->shape.bounds.w + pad);
    int64_t y1 = (int64_t)ceilf(group->shape.bounds.y + group->shape.bounds.h + pad);
    if (group->shadow_offset_y > 0.0f)
        y1 += (int64_t)ceilf(group->shadow_offset_y);

    if (x1 <= 0 || y1 <= 0 || x0 >= (int64_t)image_width || y0 >= (int64_t)image_height)
        return false;

    uint32_t clamped_x0 = (uint32_t)fmaxf((float)x0, 0.0f);
    uint32_t clamped_y0 = (uint32_t)fmaxf((float)y0, 0.0f);
    uint32_t clamped_x1 = (uint32_t)fminf((float)x1, (float)image_width);
    uint32_t clamped_y1 = (uint32_t)fminf((float)y1, (float)image_height);

    if (clamped_x1 <= clamped_x0 || clamped_y1 <= clamped_y0)
        return false;

    out->x = clamped_x0;
    out->y = clamped_y0;
    out->width = clamped_x1 - clamped_x0;
    out->height = clamped_y1 - clamped_y0;
    return true;
}

/* Everything one mica submission needs to replay its dispatches later
 * (ADR-0147). The per-group push constants are captured as prepared, so the
 * replay performs no per-group policy resolution and cannot disagree with what
 * was already decided here. */
typedef struct mica_plan {
    optics_gfx_device *device;
    struct optics_material_mica_filter *filter;
    optics_gfx_image *wallpaper;
    optics_gfx_image *blurred_wallpaper;
    optics_gfx_image *output;
    uint32_t output_width;
    uint32_t output_height;
    bool is16f;
    bool clear_full;
    uint32_t slab_x;
    uint32_t slab_y;
    optics_material__rect current[MICA_MAX_GROUPS];
    uint32_t current_count;
    optics_material__rect previous[MICA_MAX_GROUPS];
    uint32_t previous_count;
    mica_push pushes[MICA_MAX_GROUPS];
    uint32_t push_count;
} mica_plan;

static void mica_plan_record(void *raw, VkCommandBuffer cmd) {
    const mica_plan *p = raw;
    if (!p || cmd == VK_NULL_HANDLE || !p->filter)
        return;
    optics_material_mica_filter *filter = p->filter;
    const int cls = p->is16f ? 1 : 0;
    VkImage out_vk = optics_gfx_image_vk_image(p->output);

    optics_material__barrier_reuse_to_compute_write(cmd, out_vk);

    /* Full clear on first use or when the slab origin moved; otherwise only
     * what the previous submission drew and what this one will draw. */
    optics_gfx_compute_pipeline *clear = filter->clear_pipelines[cls];
    if (p->clear_full) {
        optics_material__clear_regions(
            &(optics_material__clear_opts){.cmd = cmd, .pipeline = clear, .image = p->output});
        if (clear)
            optics_material__barrier_compute_write_to_read_write(cmd, out_vk);
    } else if (clear) {
        optics_material__clear_regions(
            &(optics_material__clear_opts){.cmd = cmd,
                                           .pipeline = clear,
                                           .image = p->output,
                                           .regions = p->previous,
                                           .region_count = p->previous_count});
        optics_material__clear_regions(
            &(optics_material__clear_opts){.cmd = cmd,
                                           .pipeline = clear,
                                           .image = p->output,
                                           .regions = p->current,
                                           .region_count = p->current_count});
        /* Order the clears before the material dispatches; with nothing to
         * dispatch, the closing write-to-read barrier orders them instead. */
        if (p->current_count > 0u)
            optics_material__barrier_compute_write_to_read_write(cmd, out_vk);
    }

    for (uint32_t i = 0; i < p->push_count; ++i) {
        const optics_material__rect *bounds = &p->current[i];
        uint32_t gx = optics_material_groups(bounds->width);
        uint32_t gy = optics_material_groups(bounds->height);
        optics_gfx_compute_dispatch(
            &(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                .pipeline = filter->mica_pipelines[cls],
                                                .push_constants = &p->pushes[i],
                                                .push_bytes = sizeof(mica_push),
                                                .group_x = gx,
                                                .group_y = gy,
                                                .group_z = 1});
        if (i + 1 < p->push_count)
            optics_material__barrier_compute_write_to_read_write(cmd, out_vk);
    }

    optics_material__barrier_compute_write_to_read(cmd, out_vk);
}

optics_gfx_result
optics_material_mica_filter_apply(optics_material_mica_filter *filter,
                                  const optics_material_mica_filter_apply_opts *opts) {
    if (!filter || !opts || !opts->frame || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_frame *frame = opts->frame;
    const optics_material_mica_desc *desc = opts->desc;
    optics_material_output *out = opts->out;
    if (desc->type != OPTICS_MATERIAL_TYPE_MICA_DESC)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!desc->wallpaper && !desc->blurred_wallpaper)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > MICA_MAX_GROUPS)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > 0 && !desc->groups)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    *out = (optics_material_output){};
    if (optics_gfx_frame_get_state(frame) != OPTICS_GFX_FRAME_STATE_RECORDING ||
        optics_gfx_frame_has_active_pass(frame))
        return OPTICS_GFX_ERROR_INVALID_STATE;

    /* ADR-0147: `blurred_wallpaper` is normally a deferred optics_gfx_blur_filter
     * output; materialize it so its dispatches precede ours. */
    if (desc->blurred_wallpaper) {
        optics_gfx_result mat = optics_gfx_frame_materialize_image(frame, desc->blurred_wallpaper);
        if (mat != OPTICS_GFX_OK)
            return mat;
    }

    const optics_gfx_image *ref_input =
        desc->blurred_wallpaper ? desc->blurred_wallpaper : desc->wallpaper;
    const uint32_t capture_width = optics_gfx_image_width(ref_input);
    const uint32_t capture_height = optics_gfx_image_height(ref_input);
    if (capture_width == 0 || capture_height == 0)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    OPTICS_GFX_PROFILE_ZONE(z_mica, "material.mica.apply");

    uint32_t current_count = 0;
    optics_material__rect current_regions[MICA_MAX_GROUPS];
    uint32_t footprint_x0 = UINT32_MAX, footprint_y0 = UINT32_MAX;
    uint32_t footprint_x1 = 0u, footprint_y1 = 0u;

    for (uint32_t i = 0; i < desc->group_count; ++i) {
        optics_material__rect bounds;
        if (mica_group_dispatch_bounds(&desc->groups[i], capture_width, capture_height, &bounds)) {
            current_regions[current_count++] = bounds;
            if (bounds.x < footprint_x0)
                footprint_x0 = bounds.x;
            if (bounds.y < footprint_y0)
                footprint_y0 = bounds.y;
            if (bounds.x + bounds.width > footprint_x1)
                footprint_x1 = bounds.x + bounds.width;
            if (bounds.y + bounds.height > footprint_y1)
                footprint_y1 = bounds.y + bounds.height;
        }
    }

    uint32_t footprint_w = 1u;
    uint32_t footprint_h = 1u;
    if (current_count > 0u) {
        footprint_w = footprint_x1 - footprint_x0;
        footprint_h = footprint_y1 - footprint_y0;
    }

    uint32_t slot_idx = optics_gfx_frame_index(frame);
    optics_gfx_result r = mica_ensure_slot(filter, slot_idx, ref_input, footprint_w, footprint_h);
    if (r != OPTICS_GFX_OK)
        return r;

    mica_filter_slot *slot = &filter->slots[slot_idx];

    const uint32_t slab_x = current_count > 0u ? footprint_x0 : 0u;
    const uint32_t slab_y = current_count > 0u ? footprint_y0 : 0u;
    bool slab_moved = slot->initialized && (slot->slab_x != slab_x || slot->slab_y != slab_y);

    for (uint32_t i = 0; i < current_count; ++i) {
        current_regions[i].x -= slab_x;
        current_regions[i].y -= slab_y;
    }
    slot->slab_x = slab_x;
    slot->slab_y = slab_y;
    slot->capture_width = capture_width;
    slot->capture_height = capture_height;

    bool is16f = (slot->slab.format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT);

    r = mica_ensure_pipelines(filter, is16f, slot->initialized || desc->group_count > 0,
                              desc->group_count > 0);
    if (r != OPTICS_GFX_OK)
        return r;

    mica_plan plan = {
        .device = filter->device,
        .filter = filter,
        .wallpaper = desc->wallpaper,
        .blurred_wallpaper = desc->blurred_wallpaper,
        .output = slot->slab.image,
        .output_width = slot->slab.width,
        .output_height = slot->slab.height,
        .is16f = is16f,
        .clear_full = !slot->initialized || slab_moved,
        .slab_x = slab_x,
        .slab_y = slab_y,
        .current_count = current_count,
        .previous_count = (!slot->initialized || slab_moved) ? 0u : slot->previous_count,
    };
    memcpy(plan.current, current_regions, current_count * sizeof(optics_material__rect));
    if (plan.previous_count > 0u)
        memcpy(plan.previous, slot->previous, plan.previous_count * sizeof(optics_material__rect));

    uint32_t wp_handle =
        desc->wallpaper ? optics_gfx_image_bindless_handle(desc->wallpaper) : 0xFFFFFFFFu;
    uint32_t bl_handle = desc->blurred_wallpaper
                             ? optics_gfx_image_bindless_handle(desc->blurred_wallpaper)
                             : 0xFFFFFFFFu;
    uint32_t samp_handle = optics_gfx_device_default_sampler_handle(filter->device);
    uint32_t out_handle = optics_gfx_image_bindless_storage_handle(slot->slab.image);

    for (uint32_t i = 0; i < current_count; ++i) {
        const optics_material_mica_group *g = &desc->groups[i];
        const optics_material__rect *bounds = &plan.current[i];

        float eff_tint_opacity = (g->tint_opacity >= 0.0f) ? g->tint_opacity : desc->tint_opacity;
        float eff_lum_plate =
            (g->luminosity_plate >= 0.0f) ? g->luminosity_plate : desc->luminosity_plate;
        float eff_lum_opacity =
            (g->luminosity_opacity >= 0.0f) ? g->luminosity_opacity : desc->luminosity_opacity;
        float eff_noise = (g->noise_intensity >= 0.0f) ? g->noise_intensity : desc->noise_intensity;
        float eff_bwidth = (g->border_width >= 0.0f) ? g->border_width : desc->border_width;
        float eff_balpha = (g->border_alpha >= 0.0f) ? g->border_alpha : desc->border_alpha;

        plan.pushes[plan.push_count++] = (mica_push){
            .wallpaper_handle = wp_handle,
            .blurred_handle = bl_handle,
            .sampler_handle = samp_handle,
            .output_handle = out_handle,
            .width = slot->slab.width,
            .height = slot->slab.height,
            .origin_x = bounds->x,
            .origin_y = bounds->y,
            .bounds = {g->shape.bounds.x, g->shape.bounds.y, g->shape.bounds.w, g->shape.bounds.h},
            .corner_radius = g->shape.corner_radius,
            .opacity = g->opacity * desc->opacity,
            .tint_opacity = eff_tint_opacity,
            .luminosity_plate = eff_lum_plate,
            .luminosity_opacity = eff_lum_opacity,
            .noise_intensity = eff_noise,
            .border_width = eff_bwidth,
            .border_alpha = eff_balpha,
            .shadow_alpha = g->shadow_alpha,
            .shadow_blur = g->shadow_blur,
            .shadow_offset_y = g->shadow_offset_y,
            .tint_color = g->tint_color,
            .group_width = bounds->width,
            .group_height = bounds->height,
            .screen_origin_x = desc->screen_origin_x,
            .screen_origin_y = desc->screen_origin_y,
            .screen_width = desc->screen_width,
            .screen_height = desc->screen_height,
            .fallback_color = desc->fallback_color,
            .fallback_weight = desc->fallback_weight,
            .is_alt = (desc->kind == OPTICS_MATERIAL_MICA_ALT) ? 1u : 0u,
            .slab_origin_x = (float)slab_x,
            .slab_origin_y = (float)slab_y,
            .capture_width = (float)capture_width,
            .capture_height = (float)capture_height,
        };
    }

    slot->initialized = true;
    slot->previous_count = current_count;
    memcpy(slot->previous, current_regions, current_count * sizeof(optics_material__rect));

    *out = (optics_material_output){
        .image = slot->slab.image,
        .origin_x = (float)slot->slab_x,
        .origin_y = (float)slot->slab_y,
        .width = slot->slab.width,
        .height = slot->slab.height,
    };

    /* Defer the dispatches (ADR-0147). The handle above is valid now; the work
     * replays only if something requires the output. mica reads the wallpaper as
     * an input so the plan orders it after any wallpaper-producing pass. */
    optics_material_deferred_pass pass = OPTICS_MATERIAL_DEFERRED_PASS_INIT;
    pass.output = slot->slab.image;
    pass.label = "material.mica";
    if (desc->wallpaper) {
        pass.inputs[pass.input_count] = desc->wallpaper;
        pass.input_count++;
    }
    if (desc->blurred_wallpaper && desc->blurred_wallpaper != desc->wallpaper) {
        pass.inputs[pass.input_count] = desc->blurred_wallpaper;
        pass.input_count++;
    }
    optics_gfx_deferred_token *token = nullptr;
    optics_gfx_result reg = optics_material_deferred_register(frame, &pass, mica_plan_record, &plan,
                                                              sizeof(plan), &token);
    if (reg != OPTICS_GFX_OK) {
        mica_plan_record(&plan, optics_gfx_frame_vk_command_buffer(frame));
        token = nullptr;
    }
    (void)optics_material_deferred_require(frame, token);
    OPTICS_GFX_PROFILE_ZONE_END(z_mica);
    return OPTICS_GFX_OK;
}
