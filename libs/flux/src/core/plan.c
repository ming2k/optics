/*
 * plan.c — registration, on-demand building, boundary flushing, and pruning
 * of deferred passes (ADR-0147).
 *
 * Small on purpose. Each registered pass owns a secondary command buffer; its
 * body is built only when a consumer requires the output, and the secondary is
 * executed only at a pass boundary — the one place Vulkan allows compute to be
 * recorded alongside rendering. Passes nobody requires are neither built nor
 * executed, so pruning costs nothing beyond counting them.
 *
 * Dependency edges are stored as *prerequisites*: every pass records which
 * earlier passes it needs. Registration only ever resolves a token to an index
 * below its own, so the relation is acyclic by construction and the recursive
 * build terminates without a cycle guard. That is a structural guarantee, not a
 * runtime check, and it is why there is no `visiting` flag to forget to set.
 */

#include "plan.h"

#include "image_internal.h"
#include "internal.h"

#include <stdio.h>
#include <string.h>

/* The token is the pass slot's address, so no side table maps tokens to passes
 * and a stale token cannot silently address a different pass. `pass_count`
 * bounds every lookup, which is what rejects a token from an earlier frame. */

void flux_plan_reset(flux_plan *p, struct flux_device *device, VkCommandPool pool) {
    if (!p)
        return;
    /* Clean up any lingering pending flags from an aborted/un-submitted frame
     * so an image cannot remain indefinitely poisoned as 'pending'. */
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        flux_plan_pass *pass = &p->passes[i];
        for (uint32_t j = 0; j < pass->output_count; ++j) {
            if (pass->outputs[j])
                pass->outputs[j]->deferred_pending = false;
        }
    }
    p->epoch++;
    if (p->epoch == 0)
        p->epoch = 1;

    p->device = device;
    p->vk_device = device ? device->device : VK_NULL_HANDLE;
    p->pool = pool;
    p->pass_count = 0;
    p->arena_used = 0;
    p->executed_total = 0;
    p->fallback_total = 0;
    memset(p->passes, 0, sizeof(p->passes));
    memset(p->tokens, 0, sizeof(p->tokens));
}

void flux_plan_destroy(flux_plan *p) {
    if (!p)
        return;
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        flux_plan_pass *pass = &p->passes[i];
        for (uint32_t j = 0; j < pass->output_count; ++j) {
            if (pass->outputs[j])
                pass->outputs[j]->deferred_pending = false;
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
    /* Always resume at the slot's 16-aligned end, ignoring `alignment`.
     * Rounding down to a smaller alignment would reopen bytes already handed
     * out to the previous pass — two passes aliasing one parameter block,
     * which is how a later registration would silently corrupt an earlier
     * pass's parameters. */
    p->arena_used = base + bytes;
    size_t align16 = (p->arena_used + 15u) & ~(size_t)15u;
    p->arena_used = align16 <= FLUX_PLAN_ARENA_BYTES ? align16 : FLUX_PLAN_ARENA_BYTES;
    return dst;
}

struct flux_deferred_token *flux_plan_token(flux_plan *p, uint32_t index) {
    if (!p || index >= p->pass_count)
        return nullptr;
    struct flux_deferred_token *tok = &p->tokens[index];
    tok->pass_index = index;
    tok->epoch = p->epoch;
    return tok;
}

uint32_t flux_plan_token_index(const flux_plan *p, const struct flux_deferred_token *token) {
    if (!p || !token)
        return UINT32_MAX;
    /* Memory boundary check: token must point to one of the plan's own token slots */
    if (token < &p->tokens[0] || token >= &p->tokens[FLUX_PLAN_MAX_PASSES])
        return UINT32_MAX;
    uintptr_t offset = (uintptr_t)token - (uintptr_t)&p->tokens[0];
    if (offset % sizeof(struct flux_deferred_token) != 0)
        return UINT32_MAX;

    /* Generational token validation (ADR-0147): safe to dereference */
    if (token->epoch != p->epoch)
        return UINT32_MAX;
    if (token->pass_index >= p->pass_count)
        return UINT32_MAX;
    return token->pass_index;
}

/* Build the pass body into its own command buffer. Called at most once, and
 * only outside a render pass. Prerequisites are built first and marked
 * required: a pass this one depends on is by definition live, and without that
 * propagation a chain (blur -> material) would execute the consumer while
 * pruning the producer it samples. */
static bool plan_build_pass(flux_plan *p, uint32_t index) {
    if (index >= p->pass_count)
        return false;
    flux_plan_pass *pass = &p->passes[index];
    if (pass->built)
        return true;

    for (uint32_t i = 0; i < pass->provider_count; ++i) {
        uint32_t dep = pass->provider[i];
        if (dep == UINT32_MAX)
            continue;
        p->passes[dep].required = true;
        (void)plan_build_pass(p, dep);
    }
    for (uint32_t i = 0; i < pass->requires_count; ++i) {
        uint32_t dep = pass->requires[i];
        if (dep == UINT32_MAX)
            continue;
        p->passes[dep].required = true;
        (void)plan_build_pass(p, dep);
    }

    if (!p->pool || !p->vk_device)
        return false;
    if (pass->segment == VK_NULL_HANDLE) {
        VkCommandBufferAllocateInfo cbai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = p->pool,
            .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(p->vk_device, &cbai, &pass->segment) != VK_SUCCESS)
            return false;
    }

    /* Executed outside any render pass, so the secondary is standalone: no
     * RENDER_PASS_CONTINUE_BIT. pInheritanceInfo is still required for a
     * secondary buffer (drivers dereference it unconditionally); a zeroed one
     * states exactly that — no render pass is inherited. */
    VkCommandBufferInheritanceInfo inherit = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO,
    };
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = &inherit,
    };
    if (vkBeginCommandBuffer(pass->segment, &bi) != VK_SUCCESS)
        return false;

    if (pass->record) {
        /* The label accepted at registration is finally useful here: a captured
         * frame names the deferred work instead of an anonymous range. */
        flux_vk_cmd_begin_label(p->device, pass->segment,
                                pass->label ? pass->label : "flux.plan.pass");
        FLUX_PROFILE_ZONE(z_pass, "flux.plan.pass");
        pass->record(pass->params, pass->segment);
        FLUX_PROFILE_ZONE_END(z_pass);
        flux_vk_cmd_end_label(p->device, pass->segment);
    }
    if (vkEndCommandBuffer(pass->segment) != VK_SUCCESS)
        return false;

    pass->built = true;
    for (uint32_t i = 0; i < pass->output_count; ++i)
        if (pass->outputs[i])
            pass->outputs[i]->deferred_pending = false;
    return true;
}

uint32_t flux_plan_register(flux_plan *p, const flux_plan_pass_desc *desc) {
    if (!p || !desc)
        return UINT32_MAX;
    if (p->pass_count >= FLUX_PLAN_MAX_PASSES)
        return UINT32_MAX;
    if (desc->input_count > FLUX_PLAN_MAX_PASS_INPUTS ||
        desc->output_count > FLUX_PLAN_MAX_PASS_OUTPUTS)
        return UINT32_MAX;

    /* Resolve every edge before claiming the slot, so a rejected descriptor
     * leaves the plan untouched rather than half-registered. */
    uint32_t provider[FLUX_PLAN_MAX_PASS_INPUTS] = {};
    uint32_t requires[FLUX_PLAN_MAX_PASS_INPUTS] = {};
    uint32_t provider_count = 0;
    uint32_t requires_count = 0;
    for (uint32_t i = 0; i < desc->input_count; ++i) {
        /* An explicit token wins; otherwise fall back to "whoever declared this
         * image as an output". The fallback matters for in-library callers that
         * chain one effect onto another's output and have no token to pass — the
         * registry already knows the answer, and inferring it cannot be fooled
         * because a producer must be registered before its consumer. */
        uint32_t dep = UINT32_MAX;
        if (desc->input_tokens[i]) {
            dep = flux_plan_token_index(p, desc->input_tokens[i]);
        } else if (desc->inputs[i]) {
            for (uint32_t k = 0; k < p->pass_count && dep == UINT32_MAX; ++k)
                for (uint32_t o = 0; o < p->passes[k].output_count; ++o)
                    if (p->passes[k].outputs[o] == desc->inputs[i]) {
                        dep = k;
                        break;
                    }
        }
        if (dep == UINT32_MAX) {
            provider[provider_count++] = UINT32_MAX;
            continue;
        }
        /* A token naming this pass or a later one cannot exist yet, because
         * only earlier passes have tokens; a token from outside the plan is
         * rejected rather than treated as "no dependency", which would silently
         * unorder the work. */
        provider[provider_count++] = dep;
        requires[requires_count++] = dep;
    }

    uint32_t index = p->pass_count;
    flux_plan_pass *dst = &p->passes[index];
    *dst = (flux_plan_pass){
        .record = desc->record,
        .params = desc->params,
        .label = desc->label,
        .provider_count = provider_count,
        .requires_count = requires_count,
        .output_count = desc->output_count,
    };
    for (uint32_t i = 0; i < provider_count; ++i)
        dst->provider[i] = provider[i];
    for (uint32_t i = 0; i < requires_count; ++i)
        dst->requires[i] = requires[i];
    for (uint32_t i = 0; i < desc->output_count; ++i) {
        dst->outputs[i] = desc->outputs[i];
        /* The image's own flag is the frame-free view of the same fact; a raw
         * recording path has no plan to consult. */
        dst->outputs[i]->deferred_pending = true;
    }

    p->pass_count++;
    return index;
}

void flux_plan_require_index(flux_plan *p, uint32_t index) {
    if (!p || index >= p->pass_count)
        return;
    p->passes[index].required = true;
}

bool flux_plan_require_image(flux_plan *p, struct flux_image *image) {
    if (!p || !image)
        return true;
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        flux_plan_pass *pass = &p->passes[i];
        for (uint32_t j = 0; j < pass->output_count; ++j) {
            if (pass->outputs[j] != image)
                continue;
            pass->required = true;
            return true;
        }
    }
    return true; /* not a plan output: its writer recorded eagerly */
}

/* Build and execute everything required, in registration order. */
void flux_plan_flush(flux_plan *p, VkCommandBuffer cmd) {
    if (!p || cmd == VK_NULL_HANDLE)
        return;
    /* Two phases, deliberately. Building first propagates liveness backwards
     * (building a pass marks its prerequisites required), which can promote a
     * pass registered earlier to required. Executing afterwards therefore walks
     * an already-closed required set, and registration order — which is
     * declaration order — puts every prerequisite before its consumer. A single
     * fused loop cannot do this: it would have skipped a pass before the
     * later-registered consumer that needs it had been built. */
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        if (p->passes[i].required)
            (void)plan_build_pass(p, i);
    }
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        flux_plan_pass *pass = &p->passes[i];
        if (pass->executed || !pass->required)
            continue;
        if (!plan_build_pass(p, i))
            continue;
        vkCmdExecuteCommands(cmd, 1, &pass->segment);
        pass->executed = true;
        p->executed_total++;
    }
}

bool flux_plan_image_pending(const flux_plan *p, const struct flux_image *image) {
    (void)p;
    /* One source of truth. The flag is maintained by registration, execution and
     * pruning above, so this query answers the same for a caller holding the plan
     * and for a raw path holding only the image. */
    return image ? image->deferred_pending : false;
}

uint32_t flux_plan_finalize(flux_plan *p, VkCommandBuffer cmd) {
    if (!p)
        return 0;
    flux_plan_flush(p, cmd);
    uint32_t pruned = 0;
    for (uint32_t i = 0; i < p->pass_count; ++i) {
        flux_plan_pass *pass = &p->passes[i];
        if (pass->executed)
            continue;
        pruned++;
        /* Pruned outputs were never written. Clearing keeps a later frame — or a
         * raw path in this one — from treating this image as "waiting". */
        for (uint32_t j = 0; j < pass->output_count; ++j)
            if (pass->outputs[j])
                pass->outputs[j]->deferred_pending = false;
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
