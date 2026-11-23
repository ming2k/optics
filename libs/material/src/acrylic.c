/*
 * Acrylic material implementation.
 */

#include "material_frame.h"
#include "material_observe.h"
#include <optics/material/acrylic.h>

#include <optics/gfx/compute.h>
#include <optics/gfx/profile.h>
#include <optics/gfx/vulkan.h>

#include <math.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define ACRYLIC_MAX_GROUPS 64u

typedef struct acrylic_push {
    uint32_t input_handle;
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
    float tint_strength;
    float luminance_plate;
    float noise_intensity;
    float border_width;
    float border_alpha;
    float shadow_alpha;
    float shadow_blur;
    float shadow_offset_y;
    uint32_t tint_color;
    uint32_t group_width;
    uint32_t group_height;
    float slab_origin_x;
    float slab_origin_y;
    float capture_width;
    float capture_height;
} acrylic_push;

static_assert(sizeof(acrylic_push) == 116, "acrylic_push size mismatch");

typedef struct acrylic_filter_slot {
    optics_material__slab slab; /* the output, on the slab ladder (material_frame.h) */
    bool initialized;
    uint32_t slab_x;
    uint32_t slab_y;
    uint32_t capture_width;
    uint32_t capture_height;
    uint32_t previous_count;
    optics_material__rect previous[ACRYLIC_MAX_GROUPS];
} acrylic_filter_slot;

struct optics_material_acrylic_filter {
    atomic_uint ref_count;
    optics_gfx_device *device;
    optics_gfx_compute_pipeline *acrylic_pipelines[2];
    optics_gfx_compute_pipeline *clear_pipelines[2];
    acrylic_filter_slot slots[OPTICS_GFX_MAX_FRAMES_IN_FLIGHT];
};

optics_gfx_result optics_material_acrylic_filter_create(optics_gfx_device *device,
                                                        optics_material_acrylic_filter **out) {
    if (!device || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    *out = nullptr;
    /* Caller's allocator, attributed to material (ADR-0135). */
    optics_material_acrylic_filter *filter = nullptr;
    if (optics_gfx_device_alloc(device,
                                &(optics_gfx_device_alloc_opts){
                                    .type = OPTICS_GFX_TYPE_DEVICE_ALLOC_OPTS,
                                    .bytes = sizeof(*filter),
                                    .owner = OPTICS_MATERIAL_OBSERVE_OWNER,
                                    .kind = OPTICS_MATERIAL_OBSERVE_KIND_ACRYLIC,
                                    .scope = OPTICS_GFX_SCOPE_DEVICE,
                                },
                                (void **)&filter) != OPTICS_GFX_OK)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&filter->ref_count, 1u);
    filter->device = optics_gfx_device_retain(device);
    *out = filter;
    return OPTICS_GFX_OK;
}

optics_material_acrylic_filter *
optics_material_acrylic_filter_retain(optics_material_acrylic_filter *filter) {
    if (filter)
        atomic_fetch_add_explicit(&filter->ref_count, 1u, memory_order_relaxed);
    return filter;
}

void optics_material_acrylic_filter_release(optics_material_acrylic_filter *filter) {
    if (!filter)
        return;
    if (atomic_fetch_sub_explicit(&filter->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;
    optics_gfx_device *device = filter->device;
    for (uint32_t i = 0; i < OPTICS_GFX_MAX_FRAMES_IN_FLIGHT; ++i) {
        optics_material__slab_release(&filter->slots[i].slab);
    }
    for (int i = 0; i < 2; ++i) {
        if (filter->acrylic_pipelines[i])
            optics_gfx_compute_pipeline_release(filter->acrylic_pipelines[i]);
        if (filter->clear_pipelines[i])
            optics_gfx_compute_pipeline_release(filter->clear_pipelines[i]);
    }
    optics_gfx_device_free(device, &(optics_gfx_device_free_opts){
                                       .type = OPTICS_GFX_TYPE_DEVICE_FREE_OPTS,
                                       .ptr = filter,
                                       .owner = OPTICS_MATERIAL_OBSERVE_OWNER,
                                       .kind = OPTICS_MATERIAL_OBSERVE_KIND_ACRYLIC,
                                       .scope = OPTICS_GFX_SCOPE_DEVICE,
                                   });
    optics_gfx_device_release(device);
}

static optics_gfx_result acrylic_ensure_slot(optics_material_acrylic_filter *filter, uint32_t index,
                                             const optics_gfx_image *input, uint32_t footprint_w,
                                             uint32_t footprint_h) {
    acrylic_filter_slot *slot = &filter->slots[index];
    bool recreated = false;
    optics_gfx_result r = optics_material__slab_ensure(
        filter->device, &slot->slab,
        &(optics_material__slab_opts){
            .input = input, .footprint_width = footprint_w, .footprint_height = footprint_h},
        &recreated);
    if (recreated) /* a new image starts with no previous regions to clear */
        *slot = (acrylic_filter_slot){.slab = slot->slab};
    return r;
}

static optics_gfx_result acrylic_ensure_pipelines(optics_material_acrylic_filter *filter,
                                                  bool is16f, bool need_clear, bool need_acrylic) {
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
    if (r == OPTICS_GFX_OK && need_acrylic)
        r = optics_material__pipeline_ensure(filter->device, &filter->acrylic_pipelines[cls],
                                             &(optics_material__pipeline_opts){
                                                 .shader = OPTICS_MATERIAL__SHADER_ACRYLIC,
                                                 .is16f = is16f,
                                                 .push_constant_bytes = sizeof(acrylic_push),
                                             });
    return r;
}

optics_gfx_result
optics_material_acrylic_filter_warmup(optics_material_acrylic_filter *filter,
                                      const optics_material_acrylic_warmup_opts *opts) {
    if (!filter)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    bool is16f = false;
    if (opts) {
        if (opts->format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT)
            is16f = true;
    }

    return acrylic_ensure_pipelines(filter, is16f, true, true);
}

static bool acrylic_group_dispatch_bounds(const optics_material_acrylic_group *group,
                                          uint32_t image_width, uint32_t image_height,
                                          optics_material__rect *out) {
    if (!group || !out || image_width == 0 || image_height == 0)
        return false;
    float pad = fmaxf(group->shadow_blur * 2.0f, 2.0f);
    int64_t x0 = (int64_t)floorf(group->shape.bounds.x - pad);
    int64_t y0 = (int64_t)floorf(group->shape.bounds.y - pad);
    int64_t x1 = (int64_t)ceilf(group->shape.bounds.x + group->shape.bounds.w + pad);
    int64_t y1 = (int64_t)ceilf(group->shape.bounds.y + group->shape.bounds.h + pad +
                                fmaxf(group->shadow_offset_y, 0.0f));

    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > (int64_t)image_width ? (int64_t)image_width : x1;
    y1 = y1 > (int64_t)image_height ? (int64_t)image_height : y1;
    if (x1 <= x0 || y1 <= y0)
        return false;

    out->x = (uint32_t)x0;
    out->y = (uint32_t)y0;
    out->width = (uint32_t)(x1 - x0);
    out->height = (uint32_t)(y1 - y0);
    return true;
}

/* Everything one acrylic submission needs to replay its dispatches later
 * (ADR-0147). Copied into the frame's plan arena, so the descriptor and group
 * array may live on the caller's stack. */
typedef struct acrylic_plan {
    optics_gfx_device *device;
    struct optics_material_acrylic_filter *filter;
    optics_gfx_image *input;
    optics_gfx_image *blurred;
    optics_gfx_image *output;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t capture_width;
    uint32_t capture_height;
    bool is16f;
    float opacity;
    float tint_strength;
    float luminance_plate;
    float noise_intensity;
    float border_width;
    float border_alpha;
    uint32_t slab_x;
    uint32_t slab_y;
    uint32_t current_count;
    optics_material__rect current[ACRYLIC_MAX_GROUPS];
    optics_material__rect previous[ACRYLIC_MAX_GROUPS];
    uint32_t previous_count;
    bool clear_full;
    uint32_t group_count;
    optics_material_acrylic_group groups[ACRYLIC_MAX_GROUPS];
} acrylic_plan;

static void acrylic_plan_record(void *raw, VkCommandBuffer cmd) {
    const acrylic_plan *p = raw;
    if (!p || cmd == VK_NULL_HANDLE || !p->filter)
        return;
    optics_material_acrylic_filter *filter = p->filter;
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

    if (p->group_count > 0 && filter->acrylic_pipelines[cls]) {
        for (uint32_t i = 0; i < p->current_count; ++i) {
            const optics_material_acrylic_group *g = &p->groups[i];
            const optics_material__rect *reg = &p->current[i];
            acrylic_push pc = {
                .input_handle = optics_gfx_image_bindless_handle(p->input),
                .blurred_handle = optics_gfx_image_bindless_handle(p->blurred),
                .sampler_handle = optics_gfx_device_default_sampler_handle(p->device),
                .output_handle = optics_gfx_image_bindless_storage_handle(p->output),
                .width = p->output_width,
                .height = p->output_height,
                .origin_x = reg->x,
                .origin_y = reg->y,
                .bounds = {g->shape.bounds.x, g->shape.bounds.y, g->shape.bounds.w,
                           g->shape.bounds.h},
                .corner_radius = g->shape.corner_radius,
                .opacity = g->opacity * p->opacity,
                .tint_strength = g->tint_strength >= 0.0f ? g->tint_strength : p->tint_strength,
                .luminance_plate =
                    g->luminance_plate >= 0.0f ? g->luminance_plate : p->luminance_plate,
                .noise_intensity =
                    g->noise_intensity >= 0.0f ? g->noise_intensity : p->noise_intensity,
                .border_width = g->border_width >= 0.0f ? g->border_width : p->border_width,
                .border_alpha = g->border_alpha >= 0.0f ? g->border_alpha : p->border_alpha,
                .shadow_alpha = g->shadow_alpha,
                .shadow_blur = g->shadow_blur,
                .shadow_offset_y = g->shadow_offset_y,
                .tint_color = g->tint_color,
                .group_width = reg->width,
                .group_height = reg->height,
                .slab_origin_x = (float)p->slab_x,
                .slab_origin_y = (float)p->slab_y,
                .capture_width = (float)p->capture_width,
                .capture_height = (float)p->capture_height,
            };
            uint32_t gx = optics_material_groups(reg->width);
            uint32_t gy = optics_material_groups(reg->height);
            optics_gfx_compute_dispatch(
                &(optics_gfx_compute_dispatch_opts){.cmd = cmd,
                                                    .pipeline = filter->acrylic_pipelines[cls],
                                                    .push_constants = &pc,
                                                    .push_bytes = sizeof(pc),
                                                    .group_x = gx,
                                                    .group_y = gy,
                                                    .group_z = 1});
            if (i + 1 < p->current_count)
                optics_material__barrier_compute_write_to_read_write(cmd, out_vk);
        }
    }

    optics_material__barrier_compute_write_to_read(cmd, out_vk);
}

optics_gfx_result
optics_material_acrylic_filter_apply(optics_material_acrylic_filter *filter,
                                     const optics_material_acrylic_filter_apply_opts *opts) {
    if (!filter || !opts || !opts->frame || !opts->desc || !opts->out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_gfx_frame *frame = opts->frame;
    const optics_material_acrylic_desc *desc = opts->desc;
    optics_material_output *out = opts->out;
    if (desc->type != OPTICS_MATERIAL_TYPE_ACRYLIC_DESC)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (!desc->input || !desc->blurred_input)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > ACRYLIC_MAX_GROUPS)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->group_count > 0 && !desc->groups)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;

    *out = (optics_material_output){};
    if (optics_gfx_frame_get_state(frame) != OPTICS_GFX_FRAME_STATE_RECORDING ||
        optics_gfx_frame_has_active_pass(frame))
        return OPTICS_GFX_ERROR_INVALID_STATE;
    if (optics_gfx_image_bindless_handle(desc->input) == OPTICS_GFX_BINDLESS_INVALID ||
        optics_gfx_image_bindless_handle(desc->blurred_input) == OPTICS_GFX_BINDLESS_INVALID)
        return OPTICS_GFX_ERROR_UNSUPPORTED;

    /* ADR-0147: the blurred input is normally a deferred optics_gfx_blur_filter
     * output; materialize it so its dispatches precede ours. */
    optics_gfx_result mat = optics_gfx_frame_materialize_image(frame, desc->blurred_input);
    if (mat != OPTICS_GFX_OK)
        return mat;

    const uint32_t capture_width = optics_gfx_image_width(desc->input);
    const uint32_t capture_height = optics_gfx_image_height(desc->input);

    OPTICS_GFX_PROFILE_ZONE(z_acrylic, "material.acrylic.apply");

    uint32_t current_count = 0;
    optics_material__rect current_regions[ACRYLIC_MAX_GROUPS];
    uint32_t footprint_x0 = UINT32_MAX, footprint_y0 = UINT32_MAX;
    uint32_t footprint_x1 = 0u, footprint_y1 = 0u;

    for (uint32_t i = 0; i < desc->group_count; ++i) {
        optics_material__rect reg;
        if (acrylic_group_dispatch_bounds(&desc->groups[i], capture_width, capture_height, &reg)) {
            current_regions[current_count++] = reg;
            if (reg.x < footprint_x0)
                footprint_x0 = reg.x;
            if (reg.y < footprint_y0)
                footprint_y0 = reg.y;
            if (reg.x + reg.width > footprint_x1)
                footprint_x1 = reg.x + reg.width;
            if (reg.y + reg.height > footprint_y1)
                footprint_y1 = reg.y + reg.height;
        }
    }

    uint32_t footprint_w = 1u;
    uint32_t footprint_h = 1u;
    if (current_count > 0u) {
        footprint_w = footprint_x1 - footprint_x0;
        footprint_h = footprint_y1 - footprint_y0;
    }

    uint32_t slot_index = optics_gfx_frame_index(frame);
    optics_gfx_result r =
        acrylic_ensure_slot(filter, slot_index, desc->input, footprint_w, footprint_h);
    if (r != OPTICS_GFX_OK)
        return r;
    acrylic_filter_slot *slot = &filter->slots[slot_index];

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

    bool is16f = slot->slab.format == OPTICS_GFX_FORMAT_RGBA16_SFLOAT;
    r = acrylic_ensure_pipelines(filter, is16f, true, desc->group_count > 0);
    if (r != OPTICS_GFX_OK)
        return r;

    acrylic_plan plan = {
        .device = filter->device,
        .filter = filter,
        .input = desc->input,
        .blurred = desc->blurred_input,
        .output = slot->slab.image,
        .output_width = slot->slab.width,
        .output_height = slot->slab.height,
        .capture_width = capture_width,
        .capture_height = capture_height,
        .is16f = is16f,
        .opacity = desc->opacity,
        .tint_strength = desc->tint_strength,
        .luminance_plate = desc->luminance_plate,
        .noise_intensity = desc->noise_intensity,
        .border_width = desc->border_width,
        .border_alpha = desc->border_alpha,
        .slab_x = slab_x,
        .slab_y = slab_y,
        .current_count = current_count,
        .previous_count = slot->initialized && !slab_moved ? slot->previous_count : 0u,
        .clear_full = !slot->initialized || slab_moved,
        .group_count = desc->group_count,
    };
    memcpy(plan.current, current_regions, current_count * sizeof(optics_material__rect));
    if (plan.previous_count > 0u)
        memcpy(plan.previous, slot->previous, plan.previous_count * sizeof(optics_material__rect));
    for (uint32_t i = 0; i < desc->group_count; ++i)
        plan.groups[i] = desc->groups[i];

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
     * replays only if something requires the output. Recording eagerly on a
     * registration failure keeps correctness independent of pruning. */
    optics_material_deferred_pass pass = OPTICS_MATERIAL_DEFERRED_PASS_INIT;
    pass.output = slot->slab.image;
    pass.label = "material.acrylic";
    pass.inputs[0] = desc->input;
    pass.inputs[1] = desc->blurred_input;
    pass.input_count = 2;
    optics_gfx_deferred_token *token = nullptr;
    optics_gfx_result reg = optics_material_deferred_register(frame, &pass, acrylic_plan_record,
                                                              &plan, sizeof(plan), &token);
    if (reg != OPTICS_GFX_OK) {
        acrylic_plan_record(&plan, optics_gfx_frame_vk_command_buffer(frame));
        token = nullptr;
    }
    (void)optics_material_deferred_require(frame, token);
    OPTICS_GFX_PROFILE_ZONE_END(z_acrylic);
    return OPTICS_GFX_OK;
}
