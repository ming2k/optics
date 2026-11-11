#include "plan.h"

#include "image_internal.h"
#include "internal.h"

#include <stdio.h>
#include <string.h>

void flux_plan_reset(flux_plan *p, struct flux_render_plan *render_plan, struct flux_device *device,
                     VkCommandPool pool) {
    if (!p)
        return;
    if (p->render_plan) {
        for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
            flux_render_node *n = &p->render_plan->nodes[i];
            if (n->kind != FLUX_RENDER_NODE_EFFECT)
                continue;
            for (uint32_t j = 0; j < n->effect_output_count; ++j)
                if (n->effect_outputs[j])
                    n->effect_outputs[j]->deferred_pending = false;
        }
    }
    p->epoch++;
    if (p->epoch == 0)
        p->epoch = 1;
    p->render_plan = render_plan;
    p->device = device;
    p->vk_device = device ? device->device : VK_NULL_HANDLE;
    p->pool = pool;
    p->pass_count = 0;
    p->arena_used = 0;
    p->executed_total = 0;
    p->fallback_total = 0;
    memset(p->tokens, 0, sizeof(p->tokens));
}

void flux_plan_destroy(flux_plan *p) {
    if (!p)
        return;
    if (p->render_plan) {
        for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
            flux_render_node *n = &p->render_plan->nodes[i];
            if (n->kind != FLUX_RENDER_NODE_EFFECT)
                continue;
            for (uint32_t j = 0; j < n->effect_output_count; ++j)
                if (n->effect_outputs[j])
                    n->effect_outputs[j]->deferred_pending = false;
        }
    }
    memset(p, 0, sizeof(*p));
}

void *flux_plan_alloc_params(flux_plan *p, size_t bytes, size_t alignment) {
    if (!p || bytes == 0)
        return nullptr;
    if (alignment == 0 || alignment > 16u)
        alignment = 16u;
    size_t base = (p->arena_used + (alignment - 1u)) & ~(alignment - 1u);
    if (base > FLUX_PLAN_ARENA_BYTES || bytes > FLUX_PLAN_ARENA_BYTES - base)
        return nullptr;
    void *dst = &p->arena[base];
    memset(dst, 0, bytes);
    p->arena_used = base + bytes;
    size_t align16 = (p->arena_used + 15u) & ~(size_t)15u;
    p->arena_used = align16 <= FLUX_PLAN_ARENA_BYTES ? align16 : FLUX_PLAN_ARENA_BYTES;
    return dst;
}

flux_result flux_frame_plan_upload(flux_frame *f, struct flux_image *destination, int32_t x,
                                   int32_t y, uint32_t width, uint32_t height,
                                   const void *staging) {
    if (!f || !destination || !staging)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (f->state != FLUX_FRAME_STATE_RECORDING)
        return FLUX_ERROR_INVALID_STATE;
    if (flux_plan_image_pending(&f->plan, destination)) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "image upload targets a deferred pass output that has not executed (ADR-0147)");
        return FLUX_ERROR_INVALID_STATE;
    }

    flux_render_region region = {.full = true};
    if (width > 0 && height > 0 && x >= 0 && y >= 0) {
        region.full = false;
        region.count = 1;
        region.rects[0] = (flux_rect){(float)x, (float)y, (float)width, (float)height};
    }
    flux_render_access accesses[2] = {
        {
            .resource = {FLUX_RENDER_RESOURCE_BUFFER, staging, 0},
            .mode = FLUX_RENDER_ACCESS_READ,
            .region = {.full = true},
        },
        {
            .resource = {FLUX_RENDER_RESOURCE_IMAGE, destination, 0},
            .mode = FLUX_RENDER_ACCESS_WRITE,
            .region = region,
        },
    };

    /* Upload submissions are recorded into their own command buffer while the
     * frame is still recording, and the frame's command buffer is submitted
     * afterwards. Every upload in this frame therefore executes *before* every
     * Canvas/effect node in the plan. Model that explicitly:
     *   - an upload orders itself after earlier UPLOAD writers of the same
     *     destination (submission call order);
     *   - every existing reader of the destination is ordered after this upload.
     * A Canvas pass that writes the destination itself is a genuine conflict
     * (one submission would have to be both before and after); validation
     * rejects it rather than guessing. */
    /* Any GPU-produced staging path must arrive here as a plan-owned producer
     * identity and be ordered explicitly. Never infer it from the destination
     * image, and do not accept a raw caller-supplied node index: an index is
     * not a liveness claim, and a dependency the backend does not honour is
     * worse than a missing one. No in-tree upload has a GPU producer today
     * (glyph coverage, icon masks and caller pixels are CPU-owned); the
     * submission owner introduces that identity together with the execution
     * switch in 1B-3B/1B-4. */
    uint32_t upload_dependency = UINT32_MAX;
    for (uint32_t i = f->render_plan.node_count; i-- > 0;) {
        const flux_render_node *n = &f->render_plan.nodes[i];
        if (n->kind != FLUX_RENDER_NODE_UPLOAD)
            continue;
        for (uint32_t a = 0; a < n->access_count; ++a) {
            const flux_render_access *access = &n->accesses[a];
            if ((access->mode == FLUX_RENDER_ACCESS_WRITE ||
                 access->mode == FLUX_RENDER_ACCESS_READ_WRITE) &&
                access->resource.kind == FLUX_RENDER_RESOURCE_IMAGE &&
                access->resource.handle == (const void *)destination) {
                upload_dependency = i;
                break;
            }
        }
        if (upload_dependency != UINT32_MAX)
            break;
    }
    uint32_t dependencies[1] = {0};
    uint32_t dependency_count = 0;
    if (upload_dependency != UINT32_MAX)
        dependencies[dependency_count++] = upload_dependency;

    /* Refuse before mutating the plan when an existing reader cannot hold the
     * ordering edge this upload needs. */
    for (uint32_t i = 0; i < f->render_plan.node_count; ++i) {
        const flux_render_node *n = &f->render_plan.nodes[i];
        for (uint32_t a = 0; a < n->access_count; ++a) {
            const flux_render_access *access = &n->accesses[a];
            if (access->mode != FLUX_RENDER_ACCESS_READ &&
                access->mode != FLUX_RENDER_ACCESS_READ_WRITE)
                continue;
            if (access->resource.kind != FLUX_RENDER_RESOURCE_IMAGE ||
                access->resource.handle != (const void *)destination)
                continue;
            if (n->dependency_count >= FLUX_RENDER_PLAN_MAX_DEPENDENCIES)
                return FLUX_ERROR_OUT_OF_RANGE;
            break;
        }
    }

    const flux_render_node_desc node = {
        .kind = FLUX_RENDER_NODE_UPLOAD,
        .label = "image.upload",
        .accesses = accesses,
        .access_count = 2,
        .dependencies = dependencies,
        .dependency_count = dependency_count,
        .required_capabilities = FLUX_RENDER_CAP_TRANSFER,
    };
    uint32_t index = 0;
    flux_result r = flux_render_plan_add_node(&f->render_plan, &node, &index);
    if (r != FLUX_OK)
        return r;

    for (uint32_t i = 0; i < index; ++i) {
        const flux_render_node *n = &f->render_plan.nodes[i];
        bool reads = false;
        for (uint32_t a = 0; a < n->access_count; ++a) {
            const flux_render_access *access = &n->accesses[a];
            if ((access->mode == FLUX_RENDER_ACCESS_READ ||
                 access->mode == FLUX_RENDER_ACCESS_READ_WRITE) &&
                access->resource.kind == FLUX_RENDER_RESOURCE_IMAGE &&
                access->resource.handle == (const void *)destination) {
                reads = true;
                break;
            }
        }
        if (reads)
            (void)flux_render_plan_node_add_dependency(&f->render_plan, i, index);
    }

    r = flux_render_plan_validate(&f->render_plan);
    if (r != FLUX_OK) {
        /* Remove the ordering edges that point at the rejected node before
         * truncating it; a dangling dependency would poison every later
         * validation of this frame. */
        for (uint32_t i = 0; i < index; ++i) {
            flux_render_node *n = &f->render_plan.nodes[i];
            for (uint32_t d = 0; d < n->dependency_count;) {
                if (n->dependencies[d] != index) {
                    d++;
                    continue;
                }
                n->dependencies[d] = n->dependencies[--n->dependency_count];
            }
        }
        if (f->render_plan.node_count == index + 1u)
            f->render_plan.node_count = index;
    }
    return r;
}

struct flux_deferred_token *flux_plan_token(flux_plan *p, uint32_t index) {
    if (!p || !p->render_plan || index >= p->render_plan->node_count)
        return nullptr;
    struct flux_deferred_token *tok = &p->tokens[index];
    tok->pass_index = index;
    tok->epoch = p->epoch;
    return tok;
}

uint32_t flux_plan_token_index(const flux_plan *p, const struct flux_deferred_token *token) {
    if (!p || !p->render_plan || !token)
        return UINT32_MAX;
    if (token < &p->tokens[0] || token >= &p->tokens[FLUX_PLAN_MAX_PASSES])
        return UINT32_MAX;
    uintptr_t offset = (uintptr_t)token - (uintptr_t)&p->tokens[0];
    if (offset % sizeof(struct flux_deferred_token) != 0)
        return UINT32_MAX;
    if (token->epoch != p->epoch || token->pass_index >= p->render_plan->node_count)
        return UINT32_MAX;
    if (p->render_plan->nodes[token->pass_index].kind != FLUX_RENDER_NODE_EFFECT)
        return UINT32_MAX;
    return token->pass_index;
}

static bool plan_build_pass(flux_plan *p, uint32_t index) {
    if (!p || !p->render_plan || index >= p->render_plan->node_count)
        return false;
    flux_render_node *pass = &p->render_plan->nodes[index];
    if (pass->kind != FLUX_RENDER_NODE_EFFECT || pass->effect_built)
        return true;

    for (uint32_t i = 0; i < pass->dependency_count; ++i) {
        uint32_t dep = pass->dependencies[i];
        if (dep >= p->render_plan->node_count ||
            p->render_plan->nodes[dep].kind != FLUX_RENDER_NODE_EFFECT)
            continue;
        p->render_plan->nodes[dep].effect_required = true;
        (void)plan_build_pass(p, dep);
    }

    if (!p->pool || !p->vk_device)
        return false;
    if (pass->effect_segment == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = p->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(p->vk_device, &cbai, &pass->effect_segment) != VK_SUCCESS)
            return false;
    }

    VkCommandBufferInheritanceInfo inherit = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
    };
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = &inherit,
    };
    if (vkBeginCommandBuffer(pass->effect_segment, &bi) != VK_SUCCESS)
        return false;

    if (pass->effect_record) {
        flux_vk_cmd_begin_label(p->device, pass->effect_segment,
                                pass->label ? pass->label : "flux.plan.pass");
        FLUX_PROFILE_ZONE(z_pass, "flux.plan.pass");
        pass->effect_record(pass->effect_params, pass->effect_segment);
        FLUX_PROFILE_ZONE_END(z_pass);
        flux_vk_cmd_end_label(p->device, pass->effect_segment);
    }
    if (vkEndCommandBuffer(pass->effect_segment) != VK_SUCCESS)
        return false;

    pass->effect_built = true;
    for (uint32_t i = 0; i < pass->effect_output_count; ++i)
        if (pass->effect_outputs[i])
            pass->effect_outputs[i]->deferred_pending = false;
    return true;
}

uint32_t flux_plan_register(flux_plan *p, const flux_plan_pass_desc *desc) {
    if (!p || !p->render_plan || !desc)
        return UINT32_MAX;
    if (p->pass_count >= FLUX_PLAN_MAX_PASSES || desc->input_count > FLUX_PLAN_MAX_PASS_INPUTS ||
        desc->output_count > FLUX_PLAN_MAX_PASS_OUTPUTS ||
        desc->input_count + desc->output_count > FLUX_RENDER_PLAN_MAX_ACCESSES)
        return UINT32_MAX;

    uint32_t dependencies[FLUX_PLAN_MAX_PASS_INPUTS] = {};
    uint32_t dependency_count = 0;
    for (uint32_t i = 0; i < desc->input_count; ++i) {
        uint32_t dep = UINT32_MAX;
        if (desc->input_tokens[i]) {
            dep = flux_plan_token_index(p, desc->input_tokens[i]);
        } else if (desc->inputs[i]) {
            /* Find the node that produced this image. Canvas target writes are
             * providers too, not only deferred-effect outputs; an effect that
             * samples a Canvas target must depend on that pass. */
            for (uint32_t k = 0; k < p->render_plan->node_count && dep == UINT32_MAX; ++k) {
                const flux_render_node *n = &p->render_plan->nodes[k];
                for (uint32_t a = 0; a < n->access_count; ++a) {
                    const flux_render_access *access = &n->accesses[a];
                    if (access->mode != FLUX_RENDER_ACCESS_WRITE &&
                        access->mode != FLUX_RENDER_ACCESS_READ_WRITE)
                        continue;
                    if (access->resource.kind == FLUX_RENDER_RESOURCE_IMAGE &&
                        access->resource.handle == desc->inputs[i]) {
                        dep = k;
                        break;
                    }
                }
            }
        }
        if (dep == UINT32_MAX)
            continue;
        bool duplicate = false;
        for (uint32_t k = 0; k < dependency_count; ++k)
            if (dependencies[k] == dep)
                duplicate = true;
        if (!duplicate)
            dependencies[dependency_count++] = dep;
    }

    flux_render_access accesses[FLUX_RENDER_PLAN_MAX_ACCESSES] = {};
    uint32_t access_count = 0;
    for (uint32_t i = 0; i < desc->input_count; ++i) {
        if (!desc->inputs[i])
            continue;
        accesses[access_count++] = (flux_render_access){
            .resource = {FLUX_RENDER_RESOURCE_IMAGE, desc->inputs[i], 0},
            .mode = FLUX_RENDER_ACCESS_READ,
            .region = {.full = true},
        };
    }
    for (uint32_t i = 0; i < desc->output_count; ++i) {
        accesses[access_count++] = (flux_render_access){
            .resource = {FLUX_RENDER_RESOURCE_IMAGE, desc->outputs[i], 0},
            .mode = FLUX_RENDER_ACCESS_WRITE,
            .region = {.full = true},
        };
    }
    flux_render_effect_desc effect = {
        .record = desc->record, .params = desc->params, .output_count = desc->output_count};
    for (uint32_t i = 0; i < desc->output_count; ++i)
        effect.outputs[i] = desc->outputs[i];
    flux_render_node_desc node = {
        .kind = FLUX_RENDER_NODE_EFFECT,
        .label = desc->label,
        .accesses = accesses,
        .access_count = access_count,
        .dependencies = dependencies,
        .dependency_count = dependency_count,
        .required_capabilities = FLUX_RENDER_CAP_COMPUTE | FLUX_RENDER_CAP_STORAGE_IMAGE,
        .effect = effect,
    };
    uint32_t index = 0;
    if (flux_render_plan_add_node(p->render_plan, &node, &index) != FLUX_OK)
        return UINT32_MAX;
    for (uint32_t i = 0; i < desc->output_count; ++i)
        p->render_plan->nodes[index].effect_outputs[i]->deferred_pending = true;
    p->pass_count++;
    return index;
}

void flux_plan_require_index(flux_plan *p, uint32_t index) {
    if (!p || !p->render_plan || index >= p->render_plan->node_count ||
        p->render_plan->nodes[index].kind != FLUX_RENDER_NODE_EFFECT)
        return;
    p->render_plan->nodes[index].effect_required = true;
}

bool flux_plan_require_image(flux_plan *p, struct flux_image *image) {
    if (!p || !p->render_plan || !image)
        return true;
    for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
        flux_render_node *n = &p->render_plan->nodes[i];
        if (n->kind != FLUX_RENDER_NODE_EFFECT)
            continue;
        for (uint32_t j = 0; j < n->effect_output_count; ++j)
            if (n->effect_outputs[j] == image) {
                n->effect_required = true;
                return true;
            }
    }
    return true;
}

bool flux_plan_image_pending(const flux_plan *p, const struct flux_image *image) {
    (void)p;
    return image ? image->deferred_pending : false;
}

void flux_plan_flush(flux_plan *p, VkCommandBuffer cmd) {
    if (!p || !p->render_plan || cmd == VK_NULL_HANDLE)
        return;
    for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
        flux_render_node *n = &p->render_plan->nodes[i];
        if (n->kind == FLUX_RENDER_NODE_EFFECT && n->effect_required)
            (void)plan_build_pass(p, i);
    }
    for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
        flux_render_node *n = &p->render_plan->nodes[i];
        if (n->kind != FLUX_RENDER_NODE_EFFECT || !n->effect_required || n->effect_executed)
            continue;
        if (!plan_build_pass(p, i))
            continue;
        vkCmdExecuteCommands(cmd, 1, &n->effect_segment);
        n->effect_executed = true;
        p->executed_total++;
    }
}

uint32_t flux_plan_finalize(flux_plan *p, VkCommandBuffer cmd) {
    if (!p || !p->render_plan)
        return 0;
    flux_plan_flush(p, cmd);
    uint32_t pruned = 0;
    for (uint32_t i = 0; i < p->render_plan->node_count; ++i) {
        flux_render_node *n = &p->render_plan->nodes[i];
        if (n->kind != FLUX_RENDER_NODE_EFFECT || n->effect_executed)
            continue;
        pruned++;
        for (uint32_t j = 0; j < n->effect_output_count; ++j)
            if (n->effect_outputs[j])
                n->effect_outputs[j]->deferred_pending = false;
    }
    return pruned;
}

/* ------------------------------------------------------------------ */
/*  Public seam (ADR-0147)                                            */
/* ------------------------------------------------------------------ */

/* The descriptor's `record` takes a Vulkan command buffer, which is a backend
 * detail. This adapter registers against the plan's own typedef and validates
 * the public descriptor's invariants, keeping the plan free of public-header
 * knowledge. */
flux_result flux_frame_register_pass(flux_frame *f, const flux_deferred_pass_desc *desc,
                                     flux_deferred_token **out_token) {
    if (out_token)
        *out_token = nullptr;
    if (!f || !desc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->type != FLUX_TYPE_DEFERRED_PASS_DESC || desc->next)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!desc->record)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (desc->input_count > FLUX_DEFERRED_MAX_INPUTS ||
        desc->output_count > FLUX_DEFERRED_MAX_OUTPUTS || desc->output_count == 0u)
        return FLUX_ERROR_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < desc->output_count; ++i)
        if (!desc->outputs[i])
            return FLUX_ERROR_INVALID_ARGUMENT;
    if (!f->surface || f->state != FLUX_FRAME_STATE_RECORDING) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "registering a pass requires a recording frame");
        return FLUX_ERROR_INVALID_STATE;
    }
    /* Compute cannot be recorded inside a render pass, and a pass body is
     * recorded outside one by construction. Rejecting here keeps the failure at
     * the caller's line rather than at vkCmdExecuteCommands time. */
    if (f->pass_active) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "registering a pass requires a pass boundary; a canvas pass is active");
        return FLUX_ERROR_INVALID_STATE;
    }

    void *params = nullptr;
    if (desc->params && desc->params_size) {
        params = flux_plan_alloc_params(&f->plan, desc->params_size, alignof(max_align_t));
        if (!params) {
            f->plan.fallback_total++;
            flux_device *d = f->surface ? f->surface->device : nullptr;
            if (d && d->log) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "deferred pass '%s' arena exhausted (%zu bytes requested, %zu/%u used)",
                         desc->label ? desc->label : "unnamed", desc->params_size,
                         f->plan.arena_used, (uint32_t)FLUX_PLAN_ARENA_BYTES);
                d->log(FLUX_LOG_WARN, "plan", 0, "%s", buf, d->log_user);
            }
            FLUX_FAIL(FLUX_ERROR_OUT_OF_MEMORY,
                      "frame plan arena cannot hold this pass's parameters");
            return FLUX_ERROR_OUT_OF_MEMORY;
        }
        memcpy(params, desc->params, desc->params_size);
    }

    flux_plan_pass_desc internal = {
        .record = (flux_plan_record_fn)(void (*)(void *, VkCommandBuffer))desc->record,
        .params = params,
        .label = desc->label,
        .input_count = desc->input_count,
        .output_count = desc->output_count,
    };
    for (uint32_t i = 0; i < desc->input_count; ++i) {
        internal.inputs[i] = desc->inputs[i];
        internal.input_tokens[i] = desc->input_tokens[i];
    }
    for (uint32_t i = 0; i < desc->output_count; ++i)
        internal.outputs[i] = desc->outputs[i];

    uint32_t index = flux_plan_register(&f->plan, &internal);
    if (index == UINT32_MAX) {
        f->plan.fallback_total++;
        flux_device *d = f->surface ? f->surface->device : nullptr;
        if (d && d->log) {
            char buf[128];
            snprintf(buf, sizeof(buf), "deferred pass '%s' plan full (%u/%u passes registered)",
                     desc->label ? desc->label : "unnamed", f->plan.pass_count,
                     (uint32_t)FLUX_PLAN_MAX_PASSES);
            d->log(FLUX_LOG_WARN, "plan", 0, "%s", buf, d->log_user);
        }
        FLUX_FAIL(FLUX_ERROR_OUT_OF_RANGE, "frame plan is full");
        return FLUX_ERROR_OUT_OF_RANGE;
    }
    if (out_token)
        *out_token = flux_plan_token(&f->plan, index);
    return FLUX_OK;
}

flux_result flux_frame_require_pass(flux_frame *f, flux_deferred_token *token) {
    if (!f || !token)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!f->surface || f->state != FLUX_FRAME_STATE_RECORDING) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "requiring a pass requires a recording frame");
        return FLUX_ERROR_INVALID_STATE;
    }
    uint32_t index = flux_plan_token_index(&f->plan, token);
    if (index == UINT32_MAX) {
        FLUX_FAIL(FLUX_ERROR_INVALID_ARGUMENT, "pass token does not belong to this frame");
        return FLUX_ERROR_INVALID_ARGUMENT;
    }
    flux_plan_require_index(&f->plan, index);
    return FLUX_OK;
}

flux_result flux_frame_reject_pending_inputs(flux_frame *f, const flux_image *const *images,
                                             uint32_t count) {
    if (!f || (count > 0u && !images))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (!f->surface || f->state != FLUX_FRAME_STATE_RECORDING) {
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE, "rejecting pending inputs requires a recording frame");
        return FLUX_ERROR_INVALID_STATE;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!flux_plan_image_pending(&f->plan, images[i]))
            continue;
        FLUX_FAIL(FLUX_ERROR_INVALID_STATE,
                  "input is a deferred pass output that has not executed; materialize it first "
                  "(ADR-0147)");
        return FLUX_ERROR_INVALID_STATE;
    }
    return FLUX_OK;
}
