/*
 * Canvas — lifecycle, state stack, and public draw entry points.
 *
 * Path flattening lives in curve_flatten.c, stroking in geometry_stroke.c,
 * path-fill dispatch in canvas_dispatch.c; pipeline caching and draw
 * submission live in renderer.c.
 */
#include "backend.h"
#include "internal.h"

#include <math.h>
#include <optics/gfx/canvas_cpu.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                         */
/* ------------------------------------------------------------------ */

/* Allocation shims: a GPU canvas uses the device's tracked allocator; a
 * headless CPU canvas (device == NULL) falls back to the C allocator. Both
 * return zeroed memory. Shared by the GPU and CPU constructors. */
void *optics_gfx_canvas_alloc(optics_gfx_device *d, size_t n) {
    if (d)
        return optics_gfx_internal_alloc(d, n);
    return calloc(1, n);
}
void optics_gfx_canvas_free(optics_gfx_device *d, void *p) {
    if (d)
        optics_gfx_internal_free(d, p);
    else
        free(p);
}

static bool canvas_grow_buf(optics_gfx_device *d, void **buf, uint32_t *cap, uint32_t needed,
                            size_t elem_size) {
    if (needed <= *cap)
        return true;
    uint32_t new_cap = *cap > 0 ? *cap : 64u;
    while (new_cap < needed) {
        if (new_cap > UINT32_MAX / 2u) {
            new_cap = needed;
            break;
        }
        new_cap *= 2u;
    }
    if ((size_t)new_cap > SIZE_MAX / elem_size) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_MEMORY, "canvas scratch buffer size overflow");
        return false;
    }
    void *next = optics_gfx_canvas_alloc(d, (size_t)new_cap * elem_size);
    if (!next) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_MEMORY,
                        "canvas scratch buffer growth out of memory");
        return false;
    }
    if (*buf && *cap > 0)
        memcpy(next, *buf, (size_t)(*cap) * elem_size);
    optics_gfx_canvas_free(d, *buf);
    *buf = next;
    *cap = new_cap;
    return true;
}

bool canvas_ensure_pts(optics_gfx_canvas *c, uint32_t needed) {
    return c && canvas_grow_buf(c->device, (void **)&c->scratch_pts, &c->scratch_pts_cap, needed,
                                sizeof(*c->scratch_pts));
}

bool canvas_ensure_contours(optics_gfx_canvas *c, uint32_t needed) {
    return c && canvas_grow_buf(c->device, (void **)&c->scratch_contours, &c->scratch_contours_cap,
                                needed, sizeof(*c->scratch_contours));
}

bool canvas_ensure_verts(optics_gfx_canvas *c, uint32_t needed) {
    return c && canvas_grow_buf(c->device, (void **)&c->scratch_verts, &c->scratch_verts_cap,
                                needed, sizeof(*c->scratch_verts));
}

bool canvas_ensure_frames(optics_gfx_canvas *c, uint32_t needed) {
    return c && canvas_grow_buf(c->device, (void **)&c->scratch_frames, &c->scratch_frames_cap,
                                needed, sizeof(*c->scratch_frames));
}

bool canvas_ensure_outline(optics_gfx_canvas *c, uint32_t needed_pts, uint32_t needed_contours) {
    return c &&
           canvas_grow_buf(c->device, (void **)&c->scratch_outline_pts, &c->scratch_outline_pts_cap,
                           needed_pts, sizeof(*c->scratch_outline_pts)) &&
           canvas_grow_buf(c->device, (void **)&c->scratch_outline_contours,
                           &c->scratch_outline_contours_cap, needed_contours,
                           sizeof(*c->scratch_outline_contours));
}

static bool canvas_flatten_grow_cb(void *ctx, uint32_t need_pts, uint32_t need_cons,
                                   optics_gfx_point **pts, uint32_t *pts_cap,
                                   optics_gfx_canvas_contour **cons, uint32_t *cons_cap) {
    optics_gfx_canvas *c = ctx;
    if (need_pts > 0 && !canvas_ensure_pts(c, need_pts))
        return false;
    if (need_cons > 0 && !canvas_ensure_contours(c, need_cons))
        return false;
    *pts = c->scratch_pts;
    *pts_cap = c->scratch_pts_cap;
    *cons = c->scratch_contours;
    *cons_cap = c->scratch_contours_cap;
    return true;
}

flatten_multi canvas_flatten_path(optics_gfx_canvas *c, const optics_gfx_path *p,
                                  float pixel_scale) {
    if (!c)
        return (flatten_multi){0, 0};
    return flatten_path_to_contours_dyn(p, pixel_scale, c->scratch_pts, c->scratch_pts_cap,
                                        c->scratch_contours, c->scratch_contours_cap,
                                        canvas_flatten_grow_cb, c);
}

bool optics_gfx_canvas_alloc_scratch(optics_gfx_canvas *c) {
    optics_gfx_device *d = c->device;
    c->scratch_pts_cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP;
    c->scratch_verts_cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP * 3;
    c->scratch_contours_cap = OPTICS_GFX_CANVAS_MAX_CONTOURS;
    c->scratch_frames_cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP;
    c->scratch_pts = optics_gfx_canvas_alloc(d, c->scratch_pts_cap * sizeof(*c->scratch_pts));
    c->scratch_verts = optics_gfx_canvas_alloc(d, c->scratch_verts_cap * sizeof(*c->scratch_verts));
    c->scratch_contours =
        optics_gfx_canvas_alloc(d, c->scratch_contours_cap * sizeof(*c->scratch_contours));
    c->scratch_frames =
        optics_gfx_canvas_alloc(d, c->scratch_frames_cap * sizeof(*c->scratch_frames));
    /* A stroke body is one 4-point piece per segment; joins and caps add more,
     * and the buffers grow on demand beyond this. */
    c->scratch_outline_pts_cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP * 4;
    c->scratch_outline_contours_cap = OPTICS_GFX_CANVAS_PATH_SCRATCH_CAP * 2;
    c->scratch_outline_pts =
        optics_gfx_canvas_alloc(d, c->scratch_outline_pts_cap * sizeof(*c->scratch_outline_pts));
    c->scratch_outline_contours = optics_gfx_canvas_alloc(
        d, c->scratch_outline_contours_cap * sizeof(*c->scratch_outline_contours));
    return c->scratch_pts && c->scratch_verts && c->scratch_contours && c->scratch_frames &&
           c->scratch_outline_pts && c->scratch_outline_contours;
}

void optics_gfx_canvas_free_scratch(optics_gfx_canvas *c) {
    optics_gfx_device *d = c->device;
    optics_gfx_canvas_free(d, c->scratch_pts);
    optics_gfx_canvas_free(d, c->scratch_verts);
    optics_gfx_canvas_free(d, c->scratch_contours);
    optics_gfx_canvas_free(d, c->scratch_frames);
    optics_gfx_canvas_free(d, c->scratch_outline_pts);
    optics_gfx_canvas_free(d, c->scratch_outline_contours);
}

optics_gfx_result optics_gfx_canvas_create(const optics_gfx_canvas_desc *desc,
                                           optics_gfx_canvas **out) {
    if (!desc || !out)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_CANVAS_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_CANVAS_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }

    /* Backend selection (Skia SkSurface-style factory). AUTO => GPU when a
     * surface is given, else the software backend. */
    optics_gfx_canvas_backend_kind kind = desc->backend;
    if (kind == OPTICS_GFX_CANVAS_BACKEND_AUTO)
        kind = desc->surface ? OPTICS_GFX_CANVAS_BACKEND_GPU : OPTICS_GFX_CANVAS_BACKEND_CPU;
    if (kind == OPTICS_GFX_CANVAS_BACKEND_CPU)
        return optics_gfx_canvas_cpu_create(desc->width, desc->height, desc->scale, out);

    if (!desc->surface) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "GPU canvas requires a surface");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    *out = nullptr;

    optics_gfx_device *device = desc->surface->device;
    optics_gfx_canvas *c = optics_gfx_internal_alloc(device, sizeof(*c));
    if (!c)
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    atomic_init(&c->ref_count, 1u);
    c->device = optics_gfx_device_retain(device);
    c->surface = optics_gfx_surface_retain(desc->surface);
    c->backend = optics_gfx_canvas_backend_vk();

    /* Content scale (device-pixel ratio); the base transform applies it. */
    c->content_scale = (desc->scale > 0.0f) ? desc->scale : 1.0f;
    c->states[0].transform = optics_gfx_mat3x2_scale(c->content_scale, c->content_scale);
    c->state_top = 0;
    c->plan_node = UINT32_MAX;

    /* Backend-private GPU state: pipeline-cache warm, formats, and owned
     * working-space targets. Kept entirely inside the backend so struct
     * optics_gfx_canvas carries no Vulkan types. */
    optics_gfx_result br = c->backend->canvas_init(c->backend, c);
    if (br != OPTICS_GFX_OK) {
        optics_gfx_surface_release(c->surface);
        optics_gfx_device_release(c->device);
        optics_gfx_internal_free(device, c);
        return br;
    }

    if (!optics_gfx_canvas_alloc_scratch(c)) {
        optics_gfx_canvas_free_scratch(c);
        c->backend->canvas_destroy(c->backend, c);
        optics_gfx_surface_release(c->surface);
        optics_gfx_device_release(c->device);
        optics_gfx_internal_free(device, c);
        return OPTICS_GFX_ERROR_OUT_OF_MEMORY;
    }

    *out = c;
    return OPTICS_GFX_OK;
}

optics_gfx_canvas *optics_gfx_canvas_retain(optics_gfx_canvas *c) {
    if (!c)
        return nullptr;
    atomic_fetch_add_explicit(&c->ref_count, 1u, memory_order_relaxed);
    return c;
}

void optics_gfx_canvas_release(optics_gfx_canvas *c) {
    if (!c)
        return;
    if (atomic_fetch_sub_explicit(&c->ref_count, 1u, memory_order_acq_rel) != 1u)
        return;

    if (c->recording)
        (void)optics_gfx_canvas_end(c);
    /* Recorded segments retain images; release them before the backend
     * (and its image teardown) goes away. */
    canvas_record_pool_destroy(c);
    /* Backend owns all GPU resources (and any in-flight-frame wait). */
    c->backend->canvas_destroy(c->backend, c);

    optics_gfx_device *dev = c->device;
    optics_gfx_canvas_free_scratch(c);
    if (c->surface)
        optics_gfx_surface_release(c->surface);
    optics_gfx_canvas_free(dev, c);
    if (dev)
        optics_gfx_device_release(dev);
}

/* ------------------------------------------------------------------ */
/*  Pass envelope                                                     */
/* ------------------------------------------------------------------ */

static optics_gfx_result canvas_begin_pass_impl(optics_gfx_canvas *c, optics_gfx_frame *f,
                                                optics_gfx_image *target,
                                                const optics_gfx_canvas_pass_desc *desc) {
    if (!c || !desc)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    if (desc->type != OPTICS_GFX_TYPE_CANVAS_PASS_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_CANVAS_PASS_DESC");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* Only the first rectangle of the set is used as the frame's render area
     * hint; see canvas_end_pass_impl / vk_end_pass for the per-rect handling.
     * The full set lives in the config, because a pass scoped to a region must
     * not write outside it. */
    if (desc->damage.count > 0 && !desc->damage.rects) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "Canvas damage set names a count but no rects");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    canvas_pass_config config = {
        .clear_color = desc->clear_color,
        .damage = desc->damage,
        .clear_extent = desc->clear_extent,
    };

    /* ADR-0149 decision 3: an over-capacity damage set degrades to the full
     * extent, never to a truncated list.
     *
     * This has to happen here, once, at the parse point. If it did not, the
     * per-rect helpers would silently drop the rectangles past the bound — and a
     * dropped region is never repainted, which is the stale-pixel failure the
     * whole damage pipeline exists to prevent. Over-covering costs one full
     * repaint; under-covering costs permanent corruption. */
    if (config.damage.count > OPTICS_GFX_DAMAGE_CAPACITY) {
        config.damage = (optics_gfx_damage){};
        config.clear_extent = true;
    }

    /* ADR-0149: the localisation below rests on a *persistent* content image —
     * one buffer that partial passes accumulate onto and that every acquired
     * swapchain image is then copied from. If that image could not be created
     * (OOM or an unsupported format at swapchain creation), the surface falls
     * back to rendering directly into the acquired image, which is a rotation:
     * the same partial pass then advances only one image and the compositor
     * alternates between divergent buffers. That is exactly the 31/34/35/18/18
     * divergence ADR-0144 measured.
     *
     * So the fallback is only correct if the pass covers the whole extent, and
     * this is the one place that can tell. Widening here keeps the degraded
     * path correct at the cost of rasterisation; without it the degradation is
     * silent and permanent. */
    const bool windowed_without_content = c->surface && !c->surface->offscreen &&
                                          c->surface->content.image == VK_NULL_HANDLE && !target;
    if (windowed_without_content && (config.damage.count > 0 || !config.clear_extent)) {
        config.damage = (optics_gfx_damage){};
        config.clear_extent = true;
    }

    /* Take ownership of the damage rectangles for the lifetime of the pass.
     *
     * The descriptor is a *description*, not a lease on the caller's array: the
     * pass consults its scope on every draw and again at end_pass, long after
     * `optics_gfx_canvas_begin` has returned. Bindings routinely build the rect list
     * in a temporary container (the Rust binding's `CanvasPassOptions` owns a
     * `Vec`), so retaining the pointer was a use-after-free that silently
     * produced `count == 0` encodes and left stale pixels on screen. Copying
     * here makes the API's actual contract match its documentation and keeps
     * both backends independent of caller storage. */
    if (config.damage.count > 0) {
        memcpy(c->pass_damage_rects, config.damage.rects,
               (size_t)config.damage.count * sizeof(*c->pass_damage_rects));
        config.damage.rects = c->pass_damage_rects;
    } else {
        config.damage.rects = nullptr;
    }

    /* ADR-0149: a damage-scoped pass is legitimate on every destination,
     * including a swapchain image.
     *
     * ADR-0144 forbade it because a swapchain is a *rotation* of images and a
     * partial pass advances only the one it acquired — the images then hold
     * different patch sets and the compositor alternates between them
     * (measured: one rect arriving 31/34/35/18/18 times across five images).
     * The rule was sound; the alternative it rejected — a per-image "has this
     * image ever had a complete frame" flag — was rejected because it describes
     * initialisation, not steady state.
     *
     * The epoch history in `core/frame.c` supplies what that flag could not:
     * each image records the content epoch it last presented, and the present
     * path copies exactly the union of write areas committed since then from
     * the persistent content image. The rotation therefore cannot diverge,
     * because divergence is repaired every time an image is reacquired rather
     * than assumed absent. */
    /* Unknown tagged extensions are ignored for forward compatibility,
     * matching the surface descriptor pNext parser. */
    const struct {
        optics_gfx_struct_type type;
        const void *next;
    } *extension = desc->next;
    while (extension) {
        extension = extension->next;
    }
    if (c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas pass begun twice without end");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    /* A GPU canvas records into a frame's command buffer; a CPU canvas has
     * none. `c->device != NULL` distinguishes them. */
    if (c->device && !f) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "GPU canvas requires an open frame");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    /* Ownership: the frame must belong to the canvas's surface (its command
     * buffer comes from that surface's per-slot pool) and a capture target
     * must live on the canvas's device — same check family as the effect
     * module's frame/filter/input device match. */
    if (c->device && f->surface != c->surface) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "Canvas pass frame belongs to a different surface");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (c->device && target && target->device != c->device) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "Canvas target image belongs to a different device");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    c->frame = f;
    c->state_top = 0;
    c->states[0].transform = optics_gfx_mat3x2_scale(c->content_scale, c->content_scale);
    c->pass_error = OPTICS_GFX_OK;
    /* Blend is draw-local state. Start every pass from the public Canvas
     * default so a terminal SRC/PLUS/MULTIPLY draw in the previous pass
     * cannot affect a fixed-SRC_OVER primitive in this one. */
    c->pending_blend = OPTICS_GFX_BLEND_SRC_OVER;

    optics_gfx_result r = c->backend->begin_pass(c->backend, c, f, target, &config);
    if (r != OPTICS_GFX_OK) {
        c->frame = nullptr;
        return r;
    }
    c->pass_count++;
    c->recording = true;
    c->pass_scissor = c->states[0].scissor;
    return OPTICS_GFX_OK;
}

optics_gfx_result optics_gfx_canvas_begin(optics_gfx_canvas *c,
                                          const optics_gfx_canvas_pass_desc *desc) {
    const optics_gfx_canvas_pass_desc defaults = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    if (!desc)
        desc = &defaults;
    if (!c || desc->type != OPTICS_GFX_TYPE_CANVAS_PASS_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid Canvas/pass descriptor");
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    }
    if (c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas already recording");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    c->plan_node = UINT32_MAX;
    optics_gfx_image *image = nullptr;
    optics_gfx_target *target = nullptr;
    switch (desc->attachment.kind) {
    case OPTICS_GFX_CANVAS_ATTACHMENT_DEFAULT:
        break;
    case OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE:
        image = desc->attachment.image;
        if (!image || !c->device)
            goto invalid;
        break;
    case OPTICS_GFX_CANVAS_ATTACHMENT_TARGET:
        target = desc->attachment.target;
        if (!target || target->is_cpu != (c->device == nullptr))
            goto invalid;
        if (target->in_use) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "target already in use");
            return OPTICS_GFX_ERROR_INVALID_STATE;
        }
        if (!target->is_cpu) {
            image = target->from_image;
            if (!image) {
                OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_UNSUPPORTED,
                                "Canvas requires a sampleable color target");
                return OPTICS_GFX_ERROR_UNSUPPORTED;
            }
        }
        break;
    default:
        goto invalid;
    }
    if (!c->device && desc->frame)
        goto invalid;
    /* ADR-0147: a canvas pass is a pass boundary, and the only place compute
     * may be inserted. Flush every deferred pass the previous pass's draws
     * required, so its output is written before this pass samples it. */
    if (desc->frame && desc->frame->surface)
        optics_gfx_plan_flush(&desc->frame->plan, optics_gfx_frame_vk_command_buffer(desc->frame));
    c->bound_cpu_target = target && target->is_cpu ? target : nullptr;
    /* ADR-0089 1B-1: compile the Canvas pass into the frame's RenderPlan and
     * validate its target/resource contract before the backend opens a pass.
     * The backend remains authoritative until 1B-4; this is the compile gate,
     * not a second executor. */
    if (desc->frame && desc->frame->surface) {
        optics_gfx_render_plan *plan = &desc->frame->render_plan;
        plan->target.scale = c->content_scale;
        const void *resource = image ? (const void *)image : (const void *)desc->frame->surface;
        optics_gfx_render_region region = {.full = true};
        if (!desc->clear_extent && desc->damage.count > 0 &&
            desc->damage.count <= OPTICS_GFX_RENDER_PLAN_MAX_REGION_RECTS) {
            region.full = false;
            region.count = desc->damage.count;
            for (uint32_t i = 0; i < desc->damage.count; ++i)
                region.rects[i] = desc->damage.rects[i];
        }
        optics_gfx_render_access access = {
            .resource = {OPTICS_GFX_RENDER_RESOURCE_IMAGE, resource, 0},
            .mode = OPTICS_GFX_RENDER_ACCESS_WRITE,
            .region = region,
        };
        uint32_t deps[1] = {};
        uint32_t dep_count = plan->node_count ? 1u : 0u;
        if (dep_count)
            deps[0] = plan->node_count - 1u;
        const optics_gfx_render_node_desc node = {
            .kind = OPTICS_GFX_RENDER_NODE_CANVAS,
            .label = "canvas.pass",
            .accesses = &access,
            .access_count = 1,
            .dependencies = deps,
            .dependency_count = dep_count,
            .required_capabilities = OPTICS_GFX_RENDER_CAP_COLOR_ATTACHMENT,
        };
        uint32_t node_index = UINT32_MAX;
        optics_gfx_result plan_result = optics_gfx_render_plan_add_node(plan, &node, &node_index);
        if (plan_result == OPTICS_GFX_OK)
            plan_result = optics_gfx_render_plan_validate(plan);
        if (plan_result != OPTICS_GFX_OK) {
            if (node_index != UINT32_MAX && plan->node_count == node_index + 1u)
                plan->node_count = node_index;
            c->bound_cpu_target = nullptr;
            return plan_result;
        }
        c->plan_node = node_index;
    }
    optics_gfx_result result = canvas_begin_pass_impl(c, desc->frame, image, desc);
    if (result != OPTICS_GFX_OK) {
        /* The compile gate admitted the node, but no pass was opened. Remove it
         * rather than leaving the plan describing a pass that never recorded. */
        if (c->plan_node != UINT32_MAX && desc->frame) {
            optics_gfx_render_plan *plan = &desc->frame->render_plan;
            if (plan->node_count == c->plan_node + 1u)
                plan->node_count = c->plan_node;
        }
        c->plan_node = UINT32_MAX;
        c->bound_cpu_target = nullptr;
        return result;
    }
    c->bound_target = optics_gfx_target_retain(target);
    if (target)
        target->in_use = true;
    if (image)
        (void)optics_gfx_image_retain(image);
    return OPTICS_GFX_OK;
invalid:
    OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                    "attachment/context does not match Canvas backend");
    return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
}

optics_gfx_result optics_gfx_canvas_end(optics_gfx_canvas *c) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "Canvas pass termination does not match active pass");
        return OPTICS_GFX_ERROR_INVALID_STATE;
    }
    if (c->state_top || c->record_depth) {
        if (c->pass_error == OPTICS_GFX_OK)
            c->pass_error = OPTICS_GFX_ERROR_INVALID_STATE;
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE,
                        "unbalanced Canvas state/cache stack at end");
        while (c->record_depth) {
            optics_gfx_canvas_cache_entry entry = optics_gfx_canvas_cache_end(c);
            optics_gfx_canvas_cache_release(c, entry);
        }
        while (c->state_top)
            optics_gfx_canvas_restore(c);
    }
    optics_gfx_result result = c->pass_error;
    /* ADR-0089 1B-2B: draws may have appended sampled-image reads to this
     * pass's node while it recorded. Re-validate before the output transform
     * consumes the pass's write: a read the plan cannot order fails closed. */
    if (c->plan_node != UINT32_MAX && c->frame && c->frame->surface) {
        optics_gfx_result plan_result = optics_gfx_render_plan_validate(&c->frame->render_plan);
        if (plan_result != OPTICS_GFX_OK) {
            if (c->pass_error == OPTICS_GFX_OK)
                c->pass_error = plan_result;
            result = plan_result;
            OPTICS_GFX_FAIL(plan_result, "Canvas pass cannot be described by the RenderPlan");
        }
    }
    c->plan_node = UINT32_MAX;
    c->backend->end_pass(c->backend, c);

    c->bound_cpu_target = nullptr;
    if (c->bound_target) {
        c->bound_target->in_use = false;
        optics_gfx_target_release(c->bound_target);
        c->bound_target = nullptr;
    }

    optics_gfx_image_release(c->target);
    c->target = nullptr;
    c->target_pass = false;
    c->frame = nullptr;
    c->recording = false;
    c->pass_error = OPTICS_GFX_OK;
    return result;
}

const uint8_t *optics_gfx_canvas_read_pixels(optics_gfx_canvas *c, uint32_t *width,
                                             uint32_t *height, uint32_t *stride) {
    if (!c || c->recording || !c->backend->read_pixels)
        return nullptr;
    return c->backend->read_pixels(c->backend, c, width, height, stride);
}

/* ------------------------------------------------------------------ */
/*  State stack                                                       */
/* ------------------------------------------------------------------ */

void optics_gfx_canvas_save(optics_gfx_canvas *c) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (!c)
        return;
    if (c->state_top + 1 >= OPTICS_GFX_CANVAS_MAX_STATES) {
        /* Stack overflow: previously this was a silent no-op, leaving
         * the caller's save/restore pairing stranded with no diagnostic.
         * Surface the failure through both the device logger (visible
         * in any gfx-log-equipped app) and the thread-local error
         * slot, so the caller can detect via optics_gfx_get_last_error. */
        if (c->device && c->device->log) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "optics_gfx_canvas_save: state stack overflow (depth > %d); "
                     "save was a no-op — check for unbalanced save/restore",
                     OPTICS_GFX_CANVAS_MAX_STATES);
            c->device->log(OPTICS_GFX_LOG_ERROR, "optics_gfx_canvas_save", 0, "%s", buf,
                           c->device->log_user);
        }
        canvas_fail(c, OPTICS_GFX_ERROR_OUT_OF_RANGE, "Canvas state stack overflow");
        return;
    }
    c->states[c->state_top + 1] = c->states[c->state_top];
    c->state_top++;
    c->states[c->state_top].is_layer = false;
    c->states[c->state_top].layer_opacity = 1.0f;
}

void optics_gfx_canvas_save_layer(optics_gfx_canvas *c,
                                  const optics_gfx_canvas_save_layer_opts *opts) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (!opts) {
        c->pass_error = OPTICS_GFX_ERROR_INVALID_ARGUMENT;
        return;
    }
    const optics_gfx_rect *bounds = opts->bounds;
    float opacity = opts->opacity;
    if (c->pass_error != OPTICS_GFX_OK)
        return;
    if (!isfinite(opacity) || opacity < 0.0f || opacity > 1.0f) {
        c->pass_error = OPTICS_GFX_ERROR_INVALID_ARGUMENT;
        return;
    }
    if (!c->backend->save_layer || !c->backend->restore_layer) {
        c->pass_error = OPTICS_GFX_ERROR_UNSUPPORTED;
        return;
    }
    if (c->state_top + 1 >= OPTICS_GFX_CANVAS_MAX_STATES) {
        if (c->device && c->device->log) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "optics_gfx_canvas_save_layer: state stack overflow (depth > %d); "
                     "save_layer was a no-op — check for unbalanced save/restore",
                     OPTICS_GFX_CANVAS_MAX_STATES);
            c->device->log(OPTICS_GFX_LOG_ERROR, "optics_gfx_canvas_save_layer", 0, "%s", buf,
                           c->device->log_user);
        }
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_OUT_OF_RANGE,
                        "optics_gfx_canvas_save_layer: state stack overflow");
        c->pass_error = OPTICS_GFX_ERROR_OUT_OF_RANGE;
        return;
    }
    optics_gfx_result result = c->backend->save_layer(c->backend, c, bounds, opacity);
    if (result != OPTICS_GFX_OK) {
        c->pass_error = result;
        return;
    }
    c->states[c->state_top + 1] = c->states[c->state_top];
    c->state_top++;
    c->states[c->state_top].is_layer = true;
    c->states[c->state_top].layer_opacity = opacity;

    uint32_t depth = c->state_top;
    if (depth > c->layer_depth_peak)
        c->layer_depth_peak = depth;
    size_t layer_bytes = (size_t)c->fb_width * c->fb_height * 8;
    c->transient_bytes_allocated += layer_bytes;

    /* Bounds are allocation hints. Only an explicit clip truncates content. */
}

void optics_gfx_canvas_restore(optics_gfx_canvas *c) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (c->state_top == 0) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_STATE, "Canvas restore without save");
        return;
    }
    bool was_layer = c->states[c->state_top].is_layer;
    c->state_top--;
    c->backend->set_scissor(c->backend, c, c->states[c->state_top].scissor);
    if (was_layer && c->backend && c->backend->restore_layer) {
        c->backend->restore_layer(c->backend, c);
    }
}

void optics_gfx_canvas_translate(optics_gfx_canvas *c,
                                 const optics_gfx_canvas_translate_opts *opts) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }
    if (!opts) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    float x = opts->x;
    float y = opts->y;
    if (!(isfinite(x) && isfinite(y))) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    optics_gfx_canvas_state *s = &c->states[c->state_top];
    s->transform = optics_gfx_mat3x2_multiply(s->transform, optics_gfx_mat3x2_translate(x, y));
}

void optics_gfx_canvas_scale(optics_gfx_canvas *c, const optics_gfx_canvas_scale_opts *opts) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }
    if (!opts) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    float sx = opts->sx;
    float sy = opts->sy;
    if (!(isfinite(sx) && isfinite(sy))) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    optics_gfx_canvas_state *s = &c->states[c->state_top];
    s->transform = optics_gfx_mat3x2_multiply(s->transform, optics_gfx_mat3x2_scale(sx, sy));
}

void optics_gfx_canvas_rotate(optics_gfx_canvas *c, float radians) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (!c)
        return;
    if (!(isfinite(radians))) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    optics_gfx_canvas_state *s = &c->states[c->state_top];
    s->transform = optics_gfx_mat3x2_multiply(s->transform, optics_gfx_mat3x2_rotate(radians));
}

void optics_gfx_canvas_set_scale(optics_gfx_canvas *c, float scale) {
    if (!c)
        return;
    c->content_scale = (scale > 0.0f) ? scale : 1.0f;
    /* If set between begin/end, refresh the base transform so the change
     * takes effect this frame (callers usually set it before begin). */
    if (c->recording && c->state_top == 0)
        c->states[0].transform = optics_gfx_mat3x2_scale(c->content_scale, c->content_scale);
}

float optics_gfx_canvas_get_scale(const optics_gfx_canvas *c) {
    if (!c)
        return 1.0f;
    /* The *effective* scale: the active transform's pixel scale, so callers
     * (e.g. glyph) rasterise to match whether the scale comes from the
     * content-scale base transform or a manual optics_gfx_canvas_scale on top. */
    return optics_gfx_canvas_mat3x2_pixel_scale(c->states[c->state_top].transform);
}

void optics_gfx_canvas_transform(optics_gfx_canvas *c, optics_gfx_mat3x2 m) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (!c)
        return;
    if (!(isfinite(m.m[0]) && isfinite(m.m[1]) && isfinite(m.m[2]) && isfinite(m.m[3]) &&
          isfinite(m.m[4]) && isfinite(m.m[5]))) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    optics_gfx_canvas_state *s = &c->states[c->state_top];
    s->transform = optics_gfx_mat3x2_multiply(s->transform, m);
}

void optics_gfx_canvas_clip_rect(optics_gfx_canvas *c, optics_gfx_rect r) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "Canvas command requires an active pass");
        return;
    }

    if (!c)
        return;

    if (!(canvas_finite_rect(r))) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "non-finite Canvas transform/clip");
        return;
    }
    optics_gfx_canvas_state *state = &c->states[c->state_top];
    optics_gfx_rect transformed = optics_gfx_mat3x2_transform_rect(state->transform, r);
    optics_gfx_recti current = state->scissor;
    int64_t current_left = current.x;
    int64_t current_top = current.y;
    int64_t current_right = current_left + (int64_t)current.w;
    int64_t current_bottom = current_top + (int64_t)current.h;
    optics_gfx_recti sc = {current.x, current.y, 0, 0};

    if (isfinite(transformed.x) && isfinite(transformed.y) && isfinite(transformed.w) &&
        isfinite(transformed.h) && transformed.w > 0.0f && transformed.h > 0.0f) {
        double left_d = floor((double)transformed.x);
        double top_d = floor((double)transformed.y);
        double right_d = ceil((double)transformed.x + (double)transformed.w);
        double bottom_d = ceil((double)transformed.y + (double)transformed.h);
        int64_t left = current_left;
        int64_t top = current_top;
        int64_t right = current_right;
        int64_t bottom = current_bottom;

        if (left_d > (double)left)
            left = left_d < (double)current_right ? (int64_t)left_d : current_right;
        if (top_d > (double)top)
            top = top_d < (double)current_bottom ? (int64_t)top_d : current_bottom;
        if (right_d < (double)right)
            right = right_d > (double)current_left ? (int64_t)right_d : current_left;
        if (bottom_d < (double)bottom)
            bottom = bottom_d > (double)current_top ? (int64_t)bottom_d : current_top;

        if (right > left && bottom > top) {
            sc.x = (int32_t)left;
            sc.y = (int32_t)top;
            sc.w = (uint32_t)(right - left);
            sc.h = (uint32_t)(bottom - top);
        }
    }

    state->scissor = sc;
    c->backend->set_scissor(c->backend, c, sc);
}

/* ------------------------------------------------------------------ */
/*  Public draws                                                      */
/* ------------------------------------------------------------------ */

static bool canvas_rect_is_pixel_aligned(const optics_gfx_canvas *c, optics_gfx_rect r) {
    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    optics_gfx_point tp0 = optics_gfx_mat3x2_transform_point(tx, (optics_gfx_point){r.x, r.y});
    optics_gfx_point tp1 =
        optics_gfx_mat3x2_transform_point(tx, (optics_gfx_point){r.x + r.w, r.y});
    optics_gfx_point tp2 =
        optics_gfx_mat3x2_transform_point(tx, (optics_gfx_point){r.x + r.w, r.y + r.h});
    optics_gfx_point tp3 =
        optics_gfx_mat3x2_transform_point(tx, (optics_gfx_point){r.x, r.y + r.h});
    bool axis_aligned = (fabsf(tp0.y - tp1.y) < 1e-4f && fabsf(tp1.x - tp2.x) < 1e-4f &&
                         fabsf(tp2.y - tp3.y) < 1e-4f && fabsf(tp3.x - tp0.x) < 1e-4f) ||
                        (fabsf(tp0.x - tp1.x) < 1e-4f && fabsf(tp1.y - tp2.y) < 1e-4f &&
                         fabsf(tp2.x - tp3.x) < 1e-4f && fabsf(tp3.y - tp0.y) < 1e-4f);
    if (!axis_aligned)
        return false;
    return fabsf(tp0.x - roundf(tp0.x)) < 1e-4f && fabsf(tp0.y - roundf(tp0.y)) < 1e-4f &&
           fabsf(tp2.x - roundf(tp2.x)) < 1e-4f && fabsf(tp2.y - roundf(tp2.y)) < 1e-4f;
}

void canvas_fill_rect_internal(optics_gfx_canvas *c, optics_gfx_rect r, const canvas_paint *paint) {
    if (!c || !c->recording)
        return;
    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    optics_gfx_color vc = paint ? paint->color : 0xFF000000u;

    optics_gfx_point p0 = {r.x, r.y};
    optics_gfx_point p1 = {r.x + r.w, r.y};
    optics_gfx_point p2 = {r.x + r.w, r.y + r.h};
    optics_gfx_point p3 = {r.x, r.y + r.h};

    bool is_img = paint && paint->kind == CANVAS_PAINT_IMAGE;
    if ((is_img || !canvas_rect_is_pixel_aligned(c, r)) && c->backend && c->backend->submit_cover) {
        optics_gfx_point quad_pts[4] = {
            optics_gfx_mat3x2_transform_point(tx, p0),
            optics_gfx_mat3x2_transform_point(tx, p1),
            optics_gfx_mat3x2_transform_point(tx, p2),
            optics_gfx_mat3x2_transform_point(tx, p3),
        };
        optics_gfx_canvas_contour con = {.start = 0, .count = 4, .closed = true};
        optics_gfx_recti sc = c->states[c->state_top].scissor;
        if (c->backend->submit_cover(c->backend, c, quad_pts, &con, 1, OPTICS_GFX_FILL_NON_ZERO,
                                     paint, sc) == OPTICS_GFX_OK)
            return;
        if (is_img)
            return;
    }

    optics_gfx_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, vc);
    push_vertex(&v[1], p1, tx, vc);
    push_vertex(&v[2], p2, tx, vc);
    push_vertex(&v[3], p0, tx, vc);
    push_vertex(&v[4], p2, tx, vc);
    push_vertex(&v[5], p3, tx, vc);
    submit_triangles(c, paint, v, 6);
}

/* Pack a normalised UV pair into the vertex `_pad` field as unorm16x2.
 * canvas_solid.vert expands it into v_uv for both image quads and glyphs. */
static uint32_t pack_uv(float u, float v) {
    uint32_t pu = (uint32_t)(fminf(fmaxf(u, 0.0f), 1.0f) * 65535.0f + 0.5f);
    uint32_t pv = (uint32_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 65535.0f + 0.5f);
    return pu | (pv << 16);
}

/* Reject image quads fully outside the active physical-pixel scissor before
 * retaining/tracking an imported dma-buf. This is more than a draw-call
 * optimisation: foreign-image tracking records queue-family ownership
 * transfers and acquire-fence waits, all of which are unnecessary when the
 * rasterizer cannot touch a pixel of the image. Non-finite geometry preserves
 * the old backend-validation path instead of being silently culled. */
static bool image_quad_intersects_scissor(const optics_gfx_canvas *c,
                                          const optics_gfx_canvas_vertex *v) {
    optics_gfx_recti scissor = c->states[c->state_top].scissor;
    if (scissor.w == 0 || scissor.h == 0)
        return false;
    float min_x = v[0].pos[0], max_x = v[0].pos[0];
    float min_y = v[0].pos[1], max_y = v[0].pos[1];
    for (uint32_t i = 0; i < 6; ++i) {
        float x = v[i].pos[0];
        float y = v[i].pos[1];
        if (!isfinite(x) || !isfinite(y))
            return true;
        min_x = fminf(min_x, x);
        max_x = fmaxf(max_x, x);
        min_y = fminf(min_y, y);
        max_y = fmaxf(max_y, y);
    }
    float right = (float)((int64_t)scissor.x + scissor.w);
    float bottom = (float)((int64_t)scissor.y + scissor.h);
    return max_x > (float)scissor.x && min_x < right && max_y > (float)scissor.y && min_y < bottom;
}

static void *frame_retain_image(void *resource) {
    return optics_gfx_image_retain(resource);
}

static void frame_release_image(void *resource) {
    optics_gfx_image_release(resource);
}

/* ADR-0089 1B-2B: a Canvas draw that samples an image produced inside this
 * plan (an UPLOAD node, an earlier Canvas target, an EFFECT output) must name
 * that producer. External images have no writer in this plan and need no edge:
 * their upload completed before the frame began (optics_gfx_surface_begin_frame
 * flushes the upload batch). A pass that samples more than the node's access
 * table can state fails closed; silently under-declaring is exactly the hidden
 * read this stage exists to remove. */
static bool canvas_plan_note_read(optics_gfx_canvas *c, optics_gfx_image *img) {
    if (!c || !img || !c->frame || !c->frame->surface || c->plan_node == UINT32_MAX)
        return true;
    optics_gfx_render_plan *plan = &c->frame->render_plan;
    if (c->plan_node >= plan->node_count)
        return true;
    uint32_t producer = optics_gfx_render_plan_last_writer(plan, (const void *)img);
    if (producer == UINT32_MAX)
        return true;
    optics_gfx_render_node *node = &plan->nodes[c->plan_node];
    for (uint32_t i = 0; i < node->access_count; ++i) {
        if (node->accesses[i].resource.kind == OPTICS_GFX_RENDER_RESOURCE_IMAGE &&
            node->accesses[i].resource.handle == (const void *)img)
            return true;
    }
    const optics_gfx_render_access access = {
        .resource = {OPTICS_GFX_RENDER_RESOURCE_IMAGE, (const void *)img, 0},
        .mode = OPTICS_GFX_RENDER_ACCESS_READ,
        .region = {.full = true},
    };
    optics_gfx_result r = optics_gfx_render_plan_node_add_access(plan, c->plan_node, &access);
    if (r == OPTICS_GFX_OK && producer != c->plan_node)
        r = optics_gfx_render_plan_node_add_dependency(plan, c->plan_node, producer);
    if (r != OPTICS_GFX_OK) {
        canvas_fail(c, r, "Canvas sampled an image the RenderPlan cannot order");
        return false;
    }
    return true;
}

bool canvas_track_foreign_image(optics_gfx_canvas *c, optics_gfx_image *img) {
    if (!c || !img)
        return false;
    bool *foreign_owned = img->imported_memory ? &img->foreign_owned : nullptr;
    if (!optics_gfx_frame_track_foreign_image(c->frame, img->image, img, frame_retain_image,
                                              frame_release_image, foreign_owned))
        return false;
    return canvas_plan_note_read(c, img);
}

static void *frame_retain_sampler(void *resource) {
    return optics_gfx_sampler_retain(resource);
}

static void frame_release_sampler(void *resource) {
    optics_gfx_sampler_release(resource);
}

bool canvas_track_sampler(optics_gfx_canvas *c, optics_gfx_sampler *sampler) {
    if (!c || !sampler)
        return true;
    if (!c->frame)
        return true;
    return optics_gfx_frame_track_resource(c->frame, sampler, frame_retain_sampler,
                                           frame_release_sampler);
}

static void *frame_retain_display_list(void *resource) {
    return optics_gfx_display_list_retain(resource);
}

static void frame_release_display_list(void *resource) {
    optics_gfx_display_list_release(resource);
}

bool canvas_track_display_list(optics_gfx_canvas *c, const optics_gfx_display_list *list) {
    if (!c || !list)
        return true;
    if (!c->frame)
        return true;
    return optics_gfx_frame_track_resource(c->frame, (void *)list, frame_retain_display_list,
                                           frame_release_display_list);
}

void draw_image_with_sampler_handle(optics_gfx_canvas *c, optics_gfx_image *img,
                                    uint32_t image_handle, optics_gfx_sampler *sampler,
                                    optics_gfx_bindless_handle sh, optics_gfx_rect dst,
                                    optics_gfx_rect src, optics_gfx_color tint,
                                    optics_gfx_blend_mode blend, uint32_t kind,
                                    const optics_gfx_rect *rounded_clip, float radius) {
    /* Image draws need a GPU-resident texture (img->bindless or image_handle): unsupported on
     * a headless CPU canvas. */
    if (!c->device || sh == OPTICS_GFX_BINDLESS_INVALID)
        return;
    /* ADR-0147: sampling a deferred pass output is legal only after the
     * producer has been materialized — and materializing requires a pass
     * boundary, which this draw, by definition, is not inside of. A host that
     * wants a blur composed by the canvas must therefore materialize it
     * before beginning the pass that draws it. Rejecting the draw keeps the
     * failure at the caller's line instead of presenting an unwritten image. */
    if (img && c->frame && optics_gfx_plan_image_pending(&c->frame->plan, img)) {
        OPTICS_GFX_FAIL(
            OPTICS_GFX_ERROR_INVALID_STATE,
            "canvas sampled a deferred effect output inside a pass; call "
            "optics_gfx_frame_materialize_image before optics_gfx_canvas_begin (ADR-0147)");
        return;
    }
    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    optics_gfx_point p0 = {dst.x, dst.y};
    optics_gfx_point p1 = {dst.x + dst.w, dst.y};
    optics_gfx_point p2 = {dst.x + dst.w, dst.y + dst.h};
    optics_gfx_point p3 = {dst.x, dst.y + dst.h};

    /* UVs live on the vertices instead of being reconstructed from the
     * destination's screen-space AABB. That keeps sampling correct under
     * every affine canvas transform, including rotation and skew. The colour
     * carries a premultiplied tint for plain images and coverage glyphs. */
    optics_gfx_canvas_vertex v[6];
    push_vertex(&v[0], p0, tx, tint);
    push_vertex(&v[1], p1, tx, tint);
    push_vertex(&v[2], p2, tx, tint);
    push_vertex(&v[3], p0, tx, tint);
    push_vertex(&v[4], p2, tx, tint);
    push_vertex(&v[5], p3, tx, tint);
    if (!image_quad_intersects_scissor(c, v))
        return;
    if (img && !canvas_track_foreign_image(c, img))
        return;
    if (sampler && !canvas_track_sampler(c, sampler))
        return;
    v[0]._pad = pack_uv(0.0f, 0.0f);
    v[1]._pad = pack_uv(1.0f, 0.0f);
    v[2]._pad = pack_uv(1.0f, 1.0f);
    v[3]._pad = pack_uv(0.0f, 0.0f);
    v[4]._pad = pack_uv(1.0f, 1.0f);
    v[5]._pad = pack_uv(0.0f, 1.0f);

    optics_gfx_canvas_push pc;
    build_push(c, nullptr, &pc);
    /* ADR-0069: images with an explicit color-space tag carry a params
     * block (matrix/LUT in the shader); the untagged fast path decodes
     * gamma-encoded UNORM texels (8-bit, RGB10A2) as sRGB here. *_SRGB
     * images are decoded by the sampler hardware and 16F images are
     * already linear — both stay raw — and R8 coverage (kind 4) is not
     * a colour. */
    pc.color_params_address = (img && img->color) ? img->color->color_params_address : 0;
    if (img && img->color && img->color->color_params_address != 0)
        kind |= OPTICS_GFX_CANVAS_PUSH_HAS_COLOR_PARAMS;
    else if (img && kind != 4u &&
             (img->format == OPTICS_GFX_FORMAT_RGBA8_UNORM ||
              img->format == OPTICS_GFX_FORMAT_BGRA8_UNORM ||
              img->format == OPTICS_GFX_FORMAT_RGB10A2_UNORM))
        kind |= OPTICS_GFX_CANVAS_PUSH_DECODE_SRGB;
    pc.kind = kind;
    pc.image_handle = img ? img->bindless : image_handle;
    pc.sampler_handle = sh;
    pc.image_src[0] = src.x;
    pc.image_src[1] = src.y;
    pc.image_src[2] = src.w;
    pc.image_src[3] = src.h;
    if (rounded_clip) {
        float scale = optics_gfx_canvas_mat3x2_pixel_scale(tx);
        optics_gfx_point center = {rounded_clip->x + rounded_clip->w * 0.5f,
                                   rounded_clip->y + rounded_clip->h * 0.5f};
        optics_gfx_point transformed = optics_gfx_mat3x2_transform_point(tx, center);
        pc.image_dst[0] = transformed.x;
        pc.image_dst[1] = transformed.y;
        pc.image_dst[2] = rounded_clip->w * 0.5f * scale;
        pc.image_dst[3] = rounded_clip->h * 0.5f * scale;
        pc.grad_radius =
            fminf(fmaxf(radius * scale, 0.0f), fminf(pc.image_dst[2], pc.image_dst[3]));
    }

    if (img)
        canvas_record_retain_image(c, img);
    canvas_record_retain_sampler(c, sampler);
    c->pending_blend = blend;
    canvas_emit(c, CANVAS_PIPE_IMAGE, &pc, v, 6);
}

/* ------------------------------------------------------------------ */
/*  SDF rounded rectangles / circles                                  */
/* ------------------------------------------------------------------ */

/* True when the transform's linear part is a similarity: equal-length,
 * orthogonal columns with a positive determinant (rotation x uniform scale).
 * The SDF path is exact for these — a rigid motion plus a uniform scale
 * preserves distances, so shape-space evaluation and screen-space evaluation
 * agree — and it also covers the historical axis-aligned case. Shear and
 * anisotropic scale are NOT similarities (distances distort), so those go
 * through the path-coverage route rather than an approximated field. */
static bool canvas_sdf_similarity(const optics_gfx_canvas *c) {
    optics_gfx_mat3x2 t = c->states[c->state_top].transform;
    float s0 = t.m[0] * t.m[0] + t.m[1] * t.m[1];
    float s1 = t.m[2] * t.m[2] + t.m[3] * t.m[3];
    float dot = t.m[0] * t.m[2] + t.m[1] * t.m[3];
    float det = t.m[0] * t.m[3] - t.m[2] * t.m[1];
    if (!(s0 > 0.0f) || det <= 0.0f)
        return false;
    return fabsf(s0 - s1) <= 1e-6f * s0 && fabsf(dot) <= 1e-6f * s0;
}

/* Draw a rounded rect / squircle (or, with stroke_hw > 0, its ring border)
 * through the SDF pipeline: resolution-independent analytic AA. The shape is
 * evaluated in its OWN space (translation away), with the fragment mapped back
 * through the inverse of the transform's linear part, so a rotated shape gets
 * an exact field instead of an axis-aligned stand-in. The mapping is exact for
 * any similarity, which is what `canvas_sdf_similarity` admits.
 *
 * `curvature` selects the corner-arc family (ADR-0118): 0 is a circular-corner
 * rounded rectangle, 1 is the L4 "G2 squircle", and the field blends between
 * them. Curvature rides `image_src[2]`, the inverse linear part rides the
 * otherwise-unused `grad_from`/`grad_to` slots and the shape-unit pixel size
 * rides `image_src[3]`, so adding the squircle variant and rotation support
 * costs no ABI change and no extra pipeline. */
static void draw_sdf_shape(optics_gfx_canvas *c, optics_gfx_rect r, float radius, float curvature,
                           optics_gfx_color color, float stroke_hw) {
    if (!c || !c->recording || r.w <= 0.0f || r.h <= 0.0f)
        return;

    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    float det = tx.m[0] * tx.m[3] - tx.m[2] * tx.m[1];
    if (fabsf(det) < 1e-20f)
        return;

    /* Shape-space half extents and corner radius stay in canvas units: the
     * field is evaluated before the transform is applied. */
    float hx = r.w * 0.5f;
    float hy = r.h * 0.5f;
    float rad = radius;
    float max_r = fminf(hx, hy);
    if (rad > max_r)
        rad = max_r;
    if (rad < 0.0f)
        rad = 0.0f;
    float hw = stroke_hw > 0.0f ? stroke_hw : 0.0f;
    float curv = curvature;
    if (!(curv > 0.0f))
        curv = 0.0f;
    if (curv > 1.0f)
        curv = 1.0f;

    optics_gfx_point center = {r.x + hx, r.y + hy};
    optics_gfx_point cs = optics_gfx_mat3x2_transform_point(tx, center);

    /* Screen-space AABB of the transformed corner box, plus the AA fringe and
     * (when stroking) the ring's outer half-width in screen pixels. */
    float s = sqrtf(tx.m[0] * tx.m[0] + tx.m[1] * tx.m[1]);
    float fringe = 1.5f + hw * s;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (int i = 0; i < 4; ++i) {
        optics_gfx_point corner = {center.x + ((i == 1 || i == 2) ? hx : -hx),
                                   center.y + ((i >= 2) ? hy : -hy)};
        optics_gfx_point sp = optics_gfx_mat3x2_transform_point(tx, corner);
        if (sp.x < minx)
            minx = sp.x;
        if (sp.x > maxx)
            maxx = sp.x;
        if (sp.y < miny)
            miny = sp.y;
        if (sp.y > maxy)
            maxy = sp.y;
    }

    optics_gfx_canvas_vertex v[6];
    const optics_gfx_point quad[6] = {
        {minx - fringe, miny - fringe}, {maxx + fringe, miny - fringe},
        {maxx + fringe, maxy + fringe}, {minx - fringe, miny - fringe},
        {maxx + fringe, maxy + fringe}, {minx - fringe, maxy + fringe},
    };
    for (int i = 0; i < 6; ++i) {
        v[i].pos[0] = quad[i].x;
        v[i].pos[1] = quad[i].y;
        v[i].color = color;
        v[i]._pad = 0;
    }

    optics_gfx_canvas_push pc;
    build_push(c, nullptr, &pc);
    /* SDF params share the image_dst / image_src push slots. */
    pc.image_dst[0] = cs.x; /* transformed shape centre, screen space */
    pc.image_dst[1] = cs.y;
    pc.image_dst[2] = hx; /* half extents in shape units */
    pc.image_dst[3] = hy;
    pc.image_src[0] = rad; /* corner radius, shape units */
    pc.image_src[1] = hw;  /* ring half-width, shape units */
    pc.image_src[2] = curv;
    /* One screen pixel spans 1/s shape units: the coverage ramp width. */
    pc.image_src[3] = s > 0.0f ? 1.0f / s : 1.0f;
    /* Inverse of the transform's linear part, row-major [[m3,-m2],[-m1,m0]]/det.
     * The SDF shaders map a fragment back into shape space with it; no other
     * pipeline reads these slots. */
    pc.grad_from[0] = tx.m[3] / det;
    pc.grad_from[1] = -tx.m[2] / det;
    pc.grad_to[0] = -tx.m[1] / det;
    pc.grad_to[1] = tx.m[0] / det;

    /* Rounded-rect helpers do not take a paint, so their documented blend
     * mode is always SRC_OVER. Do not inherit an image/path draw's mode. */
    c->pending_blend = OPTICS_GFX_BLEND_SRC_OVER;
    canvas_emit(c, CANVAS_PIPE_SDF, &pc, v, 6);
}

/* Whole-image sub-rect: sample the entire texture across dst. */
static const optics_gfx_rect OPTICS_GFX_SRC_WHOLE = {0.0f, 0.0f, 1.0f, 1.0f};

/* ------------------------------------------------------------------ */
/*  Glyph runs (ADR-0010)                                             */
/* ------------------------------------------------------------------ */

void optics_gfx_canvas_draw_glyph_run(optics_gfx_canvas *c, const optics_gfx_glyph_run_desc *desc) {
    if (!c || !c->recording || !desc)
        return;
    if (desc->type != OPTICS_GFX_TYPE_GLYPH_RUN_DESC) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "desc->type != OPTICS_GFX_TYPE_GLYPH_RUN_DESC");
        return;
    }
    if (desc->quad_count == 0)
        return;
    if (!desc->quads) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT, "glyph run needs quads");
        return;
    }

    /* Two atlas sources (ADR-0019, extended by ADR-0112):
     *   - GPU:   `atlas` (optics_gfx_image, bindless). Requires a device.
     *   - Host:  `host_coverage` (R8 buffer). Device-less CPU canvas.
     * When a caller provides both (e.g. an icon mask cache or multi-backend
     * display list), the canvas backend chooses its native source rather than
     * rejecting the dual payload. */
    const bool host = (c->device == nullptr);

    /* Optional producer generation for a host coverage buffer
     * (optics_gfx_glyph_run_host_atlas_desc): recorded with each batch so replay
     * of a segment captured before an in-place atlas rearrange is refused
     * instead of sampling moved texels. Unknown extensions are ignored for
     * forward compatibility, matching the pass-desc pNext parser. */
    uint64_t host_atlas_gen = OPTICS_GFX_HOST_ATLAS_UNVERSIONED;
    const optics_gfx_glyph_run_host_atlas_desc *ext = desc->next;
    while (ext) {
        if (ext->type == OPTICS_GFX_TYPE_GLYPH_RUN_HOST_ATLAS_DESC) {
            host_atlas_gen = ext->generation;
            break;
        }
        ext = ext->next;
    }

    optics_gfx_bindless_handle sh = OPTICS_GFX_BINDLESS_INVALID;
    uint32_t atlas_w = 0, atlas_h = 0;

    if (host) {
        if (!desc->host_coverage || !desc->host_atlas_w || !desc->host_atlas_h) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                            "device-less canvas requires host_coverage in glyph run");
            return;
        }
        atlas_w = desc->host_atlas_w;
        atlas_h = desc->host_atlas_h;
        c->pending_host_atlas = desc->host_coverage;
        c->pending_host_atlas_w = atlas_w;
        c->pending_host_atlas_h = atlas_h;
        c->pending_host_atlas_gen = host_atlas_gen;
        if (host_atlas_gen != OPTICS_GFX_HOST_ATLAS_UNVERSIONED)
            canvas_host_atlas_gen_track(c, desc->host_coverage, host_atlas_gen);
    } else {
        /* GPU path needs a device + a bindless atlas image. */
        if (!desc->atlas) {
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                            "GPU canvas requires atlas image in glyph run");
            return;
        }
        /* Ownership: the atlas must live on the canvas's device, else the
         * batch would carry a bindless handle from a foreign heap. Void
         * entry point, so the failure surfaces through the pass's sticky
         * error (first error wins) as well as optics_gfx_get_last_error. */
        if (desc->atlas->device != c->device) {
            if (c->pass_error == OPTICS_GFX_OK)
                c->pass_error = OPTICS_GFX_ERROR_INVALID_ARGUMENT;
            OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                            "glyph run atlas belongs to a different device than the canvas");
            return;
        }
        sh = desc->sampler ? optics_gfx_sampler_bindless_handle(desc->sampler)
                           : optics_gfx_device_default_sampler_handle(c->device);
        if (sh == OPTICS_GFX_BINDLESS_INVALID ||
            desc->atlas->bindless == OPTICS_GFX_BINDLESS_INVALID)
            return;
        atlas_w = desc->atlas->width;
        atlas_h = desc->atlas->height;
        if (!canvas_track_foreign_image(c, desc->atlas))
            return;
        if (desc->sampler && !canvas_track_sampler(c, desc->sampler))
            return;
    }

    optics_gfx_mat3x2 tx = c->states[c->state_top].transform;
    float inv_w = 1.0f / (float)atlas_w;
    float inv_h = 1.0f / (float)atlas_h;

    optics_gfx_canvas_push pc;
    build_push(c, nullptr, &pc);
    if (!host) {
        pc.image_handle = desc->atlas->bindless;
        pc.sampler_handle = sh;
    }

    /* Chunked so a run of any length works within the scratch vertex
     * buffer; each chunk is still one draw. */
    const uint32_t max_quads = (c->scratch_verts_cap) / 6;
    for (uint32_t base = 0; base < desc->quad_count; base += max_quads) {
        uint32_t n = desc->quad_count - base;
        if (n > max_quads)
            n = max_quads;

        optics_gfx_canvas_vertex *verts = c->scratch_verts;
        uint32_t v_count = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const optics_gfx_glyph_quad *q = &desc->quads[base + i];
            float u0 = (float)q->ax * inv_w, v0 = (float)q->ay * inv_h;
            float u1 = (float)(q->ax + q->aw) * inv_w;
            float v1 = (float)(q->ay + q->ah) * inv_h;

            optics_gfx_point p0 = {q->sx, q->sy};
            optics_gfx_point p1 = {q->sx + q->sw, q->sy};
            optics_gfx_point p2 = {q->sx + q->sw, q->sy + q->sh};
            optics_gfx_point p3 = {q->sx, q->sy + q->sh};

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

        /* Glyph quads likewise have fixed SRC_OVER semantics; the tint only
         * controls colour/coverage and carries no blend mode. */
        c->pending_blend = OPTICS_GFX_BLEND_SRC_OVER;
        canvas_emit(c, CANVAS_PIPE_GLYPH, &pc, verts, v_count);
    }

    if (!host) {
        canvas_record_retain_image(c, desc->atlas);
        canvas_record_retain_sampler(c, desc->sampler);
    }

    if (host) {
        /* Drop the borrow so a subsequent non-glyph draw never sees a stale
         * host atlas pointer. */
        c->pending_host_atlas = nullptr;
        c->pending_host_atlas_w = 0;
        c->pending_host_atlas_h = 0;
        c->pending_host_atlas_gen = OPTICS_GFX_HOST_ATLAS_UNVERSIONED;
    }
}

uint64_t optics_gfx_canvas_dropped_draws(const optics_gfx_canvas *c) {
    return c ? c->dropped_draws : 0;
}

uint64_t optics_gfx_canvas_submit_calls(const optics_gfx_canvas *c) {
    return c ? c->submit_calls : 0;
}

uint64_t optics_gfx_canvas_recorded_draws(const optics_gfx_canvas *c) {
    return c ? c->recorded_draws : 0;
}

void canvas_fail(optics_gfx_canvas *c, optics_gfx_result result, const char *message) {
    if (c->pass_error == OPTICS_GFX_OK) {
        c->pass_error = result;
        OPTICS_GFX_FAIL(result, message);
    }
}

void optics_gfx_canvas_draw_geometry(optics_gfx_canvas *c,
                                     const optics_gfx_canvas_draw_geometry_opts *opts) {
    if (!c || !c->recording) {
        OPTICS_GFX_FAIL(OPTICS_GFX_ERROR_INVALID_STATE, "draw requires an active Canvas pass");
        return;
    }
    if (!opts || !opts->geom) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "draw requires geometry");
        return;
    }
    const optics_gfx_geometry *geom = opts->geom;
    const optics_gfx_brush *brush = opts->brush;
    if (!isfinite(geom->stroke.width) || geom->stroke.width < 0.0f) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid stroke width");
        return;
    }

    optics_gfx_brush b = brush ? *brush : optics_gfx_brush_solid(0xFF000000u);
    if (!canvas_valid_geometry(geom) || !canvas_valid_brush(&b)) {
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid geometry or brush");
        return;
    }
    float alpha_scale = b.opacity;

    canvas_paint p = canvas_paint_default();
    p.blend = b.blend;
    p.stroke_width = geom->stroke.width;
    p.miter_limit = geom->stroke.miter_limit;
    p.cap = geom->stroke.cap;
    p.join = geom->stroke.join;

    if (b.kind == OPTICS_GFX_BRUSH_IMAGE_PATTERN) {
        if (!b.image.image) {
            canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "image brush requires an image");
            return;
        }
        if (!c->device) {
            canvas_fail(c, OPTICS_GFX_ERROR_UNSUPPORTED, "image draws require GPU canvas");
            return;
        }
        if (b.image.image->device != c->device ||
            (b.image.sampler && optics_gfx_sampler_owner(b.image.sampler) != c->device)) {
            canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "image/sampler device mismatch");
            return;
        }

        optics_gfx_rect src = b.image.src_rect;
        if (src.w == 0.0f && src.h == 0.0f)
            src = OPTICS_GFX_SRC_WHOLE;
        if (src.w <= 0.0f || src.h <= 0.0f) {
            canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "invalid image source rectangle");
            return;
        }

        optics_gfx_rect uv_dst;
        switch (geom->kind) {
        case OPTICS_GFX_GEOM_RECT:
            uv_dst = geom->rect.rect;
            break;
        case OPTICS_GFX_GEOM_RRECT:
            uv_dst = geom->rrect.rect;
            break;
        case OPTICS_GFX_GEOM_SQUIRCLE:
            uv_dst = geom->squircle.rect;
            break;
        case OPTICS_GFX_GEOM_CIRCLE:
            uv_dst = (optics_gfx_rect){geom->circle.cx - geom->circle.radius,
                                       geom->circle.cy - geom->circle.radius,
                                       geom->circle.radius * 2.0f, geom->circle.radius * 2.0f};
            break;
        default:
            uv_dst = (optics_gfx_rect){0.0f, 0.0f, (float)b.image.image->width,
                                       (float)b.image.image->height};
            break;
        }

        optics_gfx_bindless_handle sh = b.image.sampler
                                            ? optics_gfx_sampler_bindless_handle(b.image.sampler)
                                            : optics_gfx_device_default_sampler_handle(c->device);
        uint32_t tint_color = b.image.tint;
        if (alpha_scale < 1.0f) {
            uint32_t a = (uint32_t)(((tint_color >> 24) & 0xFF) * alpha_scale + 0.5f);
            uint32_t r = (uint32_t)(((tint_color >> 16) & 0xFF) * alpha_scale + 0.5f);
            uint32_t g = (uint32_t)(((tint_color >> 8) & 0xFF) * alpha_scale + 0.5f);
            uint32_t bl = (uint32_t)((tint_color & 0xFF) * alpha_scale + 0.5f);
            tint_color = (a << 24) | (r << 16) | (g << 8) | bl;
        }

        bool clipped = b.image.clip_rect.w > 0.0f && b.image.clip_rect.h > 0.0f;
        /* Fast path for unclipped, unrotated, integer-aligned filled rects. */
        if (geom->kind == OPTICS_GFX_GEOM_RECT && geom->stroke.width <= 0.0f && !clipped &&
            canvas_rect_is_pixel_aligned(c, geom->rect.rect)) {
            uint32_t kind = b.image.opaque_only ? 6u : 3u;
            draw_image_with_sampler_handle(c, b.image.image, b.image.image->bindless,
                                           b.image.sampler, sh, geom->rect.rect, src, tint_color,
                                           b.image.opaque_only ? OPTICS_GFX_BLEND_SRC : b.blend,
                                           kind, nullptr, 0.0f);
            return;
        }

        p.kind = CANVAS_PAINT_IMAGE;
        p.color = tint_color;
        p.gradient.image.image = b.image.image;
        p.gradient.image.sampler = b.image.sampler;
        p.gradient.image.dst_rect = uv_dst;
        p.gradient.image.src_rect = src;
        p.gradient.image.sampler_handle = sh;
        p.gradient.image.opaque_only = b.image.opaque_only;

        if (clipped && geom->stroke.width <= 0.0f &&
            (geom->kind == OPTICS_GFX_GEOM_RECT || geom->kind == OPTICS_GFX_GEOM_RRECT)) {
            optics_gfx_path_segment segs[16];
            optics_gfx_path path = {.segments = segs, .capacity = 16};
            optics_gfx_path_add_round_rect(&path, b.image.clip_rect, b.image.clip_radius);
            canvas_fill_path_internal(c, &path, &p);
            return;
        }
    } else if (b.kind == OPTICS_GFX_BRUSH_SOLID) {
        p.kind = CANVAS_PAINT_SOLID;
        uint32_t color = b.solid.color;
        if (alpha_scale < 1.0f) {
            uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
            uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
            uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
            uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
            p.color = (a << 24) | (r << 16) | (g << 8) | bl;
        } else {
            p.color = color;
        }
    } else if (b.kind == OPTICS_GFX_BRUSH_LINEAR_GRADIENT) {
        p.kind = CANVAS_PAINT_LINEAR_GRADIENT;
        p.gradient.linear.from = b.gradient.start;
        p.gradient.linear.to = b.gradient.end;
        p.gradient.linear.stops = b.gradient.stops;
        if (alpha_scale < 1.0f) {
            for (uint32_t i = 0; i < p.gradient.linear.stops.count && i < 8; ++i) {
                uint32_t color = p.gradient.linear.stops.stops[i].color;
                uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
                uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
                uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
                uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
                p.gradient.linear.stops.stops[i].color = (a << 24) | (r << 16) | (g << 8) | bl;
            }
        }
    } else if (b.kind == OPTICS_GFX_BRUSH_RADIAL_GRADIENT) {
        p.kind = CANVAS_PAINT_RADIAL_GRADIENT;
        p.gradient.radial.center = b.gradient.start;
        p.gradient.radial.radius = b.gradient.radius;
        p.gradient.radial.stops = b.gradient.stops;
        if (alpha_scale < 1.0f) {
            for (uint32_t i = 0; i < p.gradient.radial.stops.count && i < 8; ++i) {
                uint32_t color = p.gradient.radial.stops.stops[i].color;
                uint32_t a = (uint32_t)(((color >> 24) & 0xFF) * alpha_scale + 0.5f);
                uint32_t r = (uint32_t)(((color >> 16) & 0xFF) * alpha_scale + 0.5f);
                uint32_t g = (uint32_t)(((color >> 8) & 0xFF) * alpha_scale + 0.5f);
                uint32_t bl = (uint32_t)((color & 0xFF) * alpha_scale + 0.5f);
                p.gradient.radial.stops.stops[i].color = (a << 24) | (r << 16) | (g << 8) | bl;
            }
        }
    }

    switch (geom->kind) {
    case OPTICS_GFX_GEOM_RECT:
        if (geom->stroke.width <= 0.0f) {
            canvas_fill_rect_internal(c, geom->rect.rect, &p);
        } else {
            optics_gfx_path_segment segs[5];
            optics_gfx_path path = {.segments = segs, .capacity = 5};
            optics_gfx_path_add_rect(&path, geom->rect.rect);
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    case OPTICS_GFX_GEOM_RRECT: {
        optics_gfx_rect r = geom->rrect.rect;
        float radius = geom->rrect.radius;
        if (p.kind == CANVAS_PAINT_SOLID && p.blend == OPTICS_GFX_BLEND_SRC_OVER &&
            canvas_sdf_similarity(c)) {
            draw_sdf_shape(c, r, radius, 0.0f, p.color, geom->stroke.width * 0.5f);
            return;
        }
        optics_gfx_path_segment segs[16];
        optics_gfx_path path = {.segments = segs, .capacity = 16};
        optics_gfx_path_add_round_rect(&path, r, radius);
        if (geom->stroke.width <= 0.0f) {
            canvas_fill_path_internal(c, &path, &p);
        } else {
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    }
    case OPTICS_GFX_GEOM_SQUIRCLE: {
        optics_gfx_rect r = geom->squircle.rect;
        /* A continuous-curvature corner has an exact distance field
         * (optics_gfx_sdf_squircle), so it takes the analytic path on the same
         * terms as a rounded rectangle: no flattening, no coverage pass, and
         * resolution-independent coverage (ADR-0118 INV-SDF-03). */
        if (p.kind == CANVAS_PAINT_SOLID && p.blend == OPTICS_GFX_BLEND_SRC_OVER &&
            canvas_sdf_similarity(c)) {
            draw_sdf_shape(c, r, geom->squircle.radius, geom->squircle.curvature, p.color,
                           geom->stroke.width * 0.5f);
            return;
        }
        optics_gfx_path_segment segs[40];
        optics_gfx_path path = {.segments = segs, .capacity = 40};
        optics_gfx_path_add_squircle(&path, r, geom->squircle.radius, geom->squircle.curvature);
        if (geom->stroke.width <= 0.0f) {
            canvas_fill_path_internal(c, &path, &p);
        } else {
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    }
    case OPTICS_GFX_GEOM_CIRCLE: {
        float cx = geom->circle.cx;
        float cy = geom->circle.cy;
        float rad = geom->circle.radius;
        optics_gfx_rect r = {cx - rad, cy - rad, rad * 2.0f, rad * 2.0f};
        if (p.kind == CANVAS_PAINT_SOLID && p.blend == OPTICS_GFX_BLEND_SRC_OVER &&
            canvas_sdf_similarity(c)) {
            draw_sdf_shape(c, r, rad, 0.0f, p.color, geom->stroke.width * 0.5f);
            return;
        }
        optics_gfx_path_segment segs[16];
        optics_gfx_path path = {.segments = segs, .capacity = 16};
        optics_gfx_path_add_circle(&path, cx, cy, rad);
        if (geom->stroke.width <= 0.0f) {
            canvas_fill_path_internal(c, &path, &p);
        } else {
            canvas_stroke_path_internal(c, &path, &p);
        }
        break;
    }
    case OPTICS_GFX_GEOM_LINE: {
        if (geom->stroke.width <= 0.0f) {
            canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT,
                        "line requires positive stroke width");
            return;
        }
        optics_gfx_path_segment segs[3];
        optics_gfx_path path = {.segments = segs, .capacity = 3};
        optics_gfx_path_move_to(&path, geom->line.x0, geom->line.y0);
        optics_gfx_path_line_to(&path, geom->line.x1, geom->line.y1);
        p.stroke_width = geom->stroke.width;
        canvas_stroke_path_internal(c, &path, &p);
        break;
    }
    case OPTICS_GFX_GEOM_PATH:
        if (!geom->path.path) {
            canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "path geometry requires a path");
            return;
        }
        p.fill_rule = geom->path.fill_rule;
        if (geom->stroke.width <= 0.0f) {
            canvas_fill_path_internal(c, geom->path.path, &p);
        } else {
            canvas_stroke_path_internal(c, geom->path.path, &p);
        }
        break;
    default:
        canvas_fail(c, OPTICS_GFX_ERROR_INVALID_ARGUMENT, "unknown geometry kind");
        break;
    }
}

optics_gfx_render_plan_stats optics_gfx_canvas_get_plan_stats(const optics_gfx_canvas *c) {
    if (!c)
        return (optics_gfx_render_plan_stats){};
    return (optics_gfx_render_plan_stats){
        .pass_count = c->pass_count,
        .layer_depth_peak = c->layer_depth_peak,
        .transient_bytes_allocated = c->transient_bytes_allocated,
        .transient_bytes_aliased_saved = c->transient_bytes_aliased_saved,
    };
}
