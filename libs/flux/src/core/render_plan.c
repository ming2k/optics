#include "render_plan.h"

#include <math.h>
#include <string.h>

static bool region_valid(const flux_render_region *region) {
    if (region->full)
        return true;
    if (region->count == 0 || region->count > FLUX_RENDER_PLAN_MAX_REGION_RECTS)
        return false;
    for (uint32_t i = 0; i < region->count; ++i) {
        const flux_rect *r = &region->rects[i];
        if (!isfinite(r->x) || !isfinite(r->y) || !isfinite(r->w) || !isfinite(r->h) || r->w < 0 ||
            r->h < 0)
            return false;
    }
    return true;
}

static bool target_valid(const flux_render_target *t) {
    if (!t || t->width == 0 || t->height == 0 || !isfinite(t->scale) || t->scale <= 0.0f)
        return false;
    if (t->format == FLUX_FORMAT_UNDEFINED)
        return false;
    if (t->sample_count == 0 || (t->sample_count & (t->sample_count - 1u)) != 0 ||
        t->sample_count > 64)
        return false;
    return true;
}

static bool same_resource(const flux_render_resource *a, const flux_render_resource *b) {
    return a->kind == b->kind && a->handle == b->handle && a->version == b->version;
}

static bool is_writer(flux_render_access_mode m) {
    return m == FLUX_RENDER_ACCESS_WRITE || m == FLUX_RENDER_ACCESS_READ_WRITE;
}
static bool is_reader(flux_render_access_mode m) {
    return m == FLUX_RENDER_ACCESS_READ || m == FLUX_RENDER_ACCESS_READ_WRITE;
}

/* Reachability through explicit dependencies. Node order is not an ordering
 * guarantee: a dependency edge is the only permission to rely on a write. */
static bool depends_on(const flux_render_plan *plan, uint32_t from, uint32_t target,
                       uint8_t *state) {
    if (from == target)
        return true;
    if (state[from] == 1)
        return false;
    if (state[from] == 2)
        return false;
    state[from] = 1;
    const flux_render_node *n = &plan->nodes[from];
    for (uint32_t i = 0; i < n->dependency_count; ++i) {
        uint32_t dep = n->dependencies[i];
        if (dep < plan->node_count && depends_on(plan, dep, target, state))
            return true;
    }
    state[from] = 2;
    return false;
}

flux_result flux_render_plan_validate(const flux_render_plan *plan) {
    if (!plan || !target_valid(&plan->target))
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (plan->node_count > FLUX_RENDER_PLAN_MAX_NODES)
        return FLUX_ERROR_OUT_OF_RANGE;

    size_t total_transient = 0;
    for (uint32_t i = 0; i < plan->node_count; ++i) {
        const flux_render_node *n = &plan->nodes[i];
        if (n->kind < FLUX_RENDER_NODE_UPLOAD || n->kind > FLUX_RENDER_NODE_OUTPUT ||
            n->access_count > FLUX_RENDER_PLAN_MAX_ACCESSES ||
            n->dependency_count > FLUX_RENDER_PLAN_MAX_DEPENDENCIES)
            return FLUX_ERROR_INVALID_ARGUMENT;
        if ((n->required_capabilities & ~plan->target.capabilities) != 0)
            return FLUX_ERROR_UNSUPPORTED;
        if (n->transient_bytes > SIZE_MAX - total_transient)
            return FLUX_ERROR_OUT_OF_RANGE;
        total_transient += n->transient_bytes;
        for (uint32_t a = 0; a < n->access_count; ++a) {
            const flux_render_access *access = &n->accesses[a];
            if (access->resource.kind != FLUX_RENDER_RESOURCE_IMAGE &&
                access->resource.kind != FLUX_RENDER_RESOURCE_BUFFER)
                return FLUX_ERROR_INVALID_ARGUMENT;
            if (!access->resource.handle || !region_valid(&access->region))
                return FLUX_ERROR_INVALID_ARGUMENT;
            if (access->mode < FLUX_RENDER_ACCESS_READ ||
                access->mode > FLUX_RENDER_ACCESS_READ_WRITE)
                return FLUX_ERROR_INVALID_ARGUMENT;
        }
        for (uint32_t d = 0; d < n->dependency_count; ++d) {
            uint32_t dep = n->dependencies[d];
            if (dep >= plan->node_count || dep == i)
                return FLUX_ERROR_INVALID_STATE;
            for (uint32_t k = 0; k < d; ++k)
                if (n->dependencies[k] == dep)
                    return FLUX_ERROR_INVALID_STATE;
        }
    }
    if (plan->target.transient_budget_bytes &&
        total_transient > plan->target.transient_budget_bytes)
        return FLUX_ERROR_OUT_OF_RANGE;

    /* Cycle detection and ordering:
     *   - a write must not be consumed without a dependency path;
     *   - two writers of one version need an ordering path;
     *   - a cycle is a dependency path back to itself. */
    for (uint32_t i = 0; i < plan->node_count; ++i) {
        for (uint32_t d = 0; d < plan->nodes[i].dependency_count; ++d) {
            uint8_t state[FLUX_RENDER_PLAN_MAX_NODES] = {0};
            if (depends_on(plan, plan->nodes[i].dependencies[d], i, state))
                return FLUX_ERROR_INVALID_STATE;
        }
        for (uint32_t a = 0; a < plan->nodes[i].access_count; ++a) {
            const flux_render_access *read = &plan->nodes[i].accesses[a];
            if (!is_reader(read->mode))
                continue;
            for (uint32_t j = 0; j < plan->node_count; ++j) {
                if (j == i)
                    continue;
                for (uint32_t b = 0; b < plan->nodes[j].access_count; ++b) {
                    const flux_render_access *write = &plan->nodes[j].accesses[b];
                    if (!is_writer(write->mode) ||
                        !same_resource(&read->resource, &write->resource))
                        continue;
                    uint8_t state[FLUX_RENDER_PLAN_MAX_NODES] = {0};
                    if (!depends_on(plan, i, j, state))
                        return FLUX_ERROR_INVALID_STATE;
                }
            }
        }
    }
    for (uint32_t i = 0; i < plan->node_count; ++i) {
        for (uint32_t a = 0; a < plan->nodes[i].access_count; ++a) {
            const flux_render_access *wa = &plan->nodes[i].accesses[a];
            if (!is_writer(wa->mode))
                continue;
            for (uint32_t j = i + 1; j < plan->node_count; ++j) {
                for (uint32_t b = 0; b < plan->nodes[j].access_count; ++b) {
                    const flux_render_access *wb = &plan->nodes[j].accesses[b];
                    if (!is_writer(wb->mode) || !same_resource(&wa->resource, &wb->resource))
                        continue;
                    uint8_t state[FLUX_RENDER_PLAN_MAX_NODES] = {0};
                    if (!depends_on(plan, j, i, state))
                        return FLUX_ERROR_INVALID_STATE;
                }
            }
        }
    }
    return FLUX_OK;
}

void flux_render_plan_init(flux_render_plan *plan, const flux_render_target *target) {
    if (!plan)
        return;
    memset(plan, 0, sizeof(*plan));
    if (target)
        plan->target = *target;
}

flux_result flux_render_plan_add_node(flux_render_plan *plan, const flux_render_node_desc *desc,
                                      uint32_t *out_index) {
    if (!plan || !desc)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (plan->node_count >= FLUX_RENDER_PLAN_MAX_NODES ||
        desc->access_count > FLUX_RENDER_PLAN_MAX_ACCESSES ||
        desc->dependency_count > FLUX_RENDER_PLAN_MAX_DEPENDENCIES ||
        (desc->access_count && !desc->accesses) || (desc->dependency_count && !desc->dependencies))
        return FLUX_ERROR_OUT_OF_RANGE;
    flux_render_node *node = &plan->nodes[plan->node_count];
    memset(node, 0, sizeof(*node));
    node->kind = desc->kind;
    node->label = desc->label;
    node->transient_bytes = desc->transient_bytes;
    node->required_capabilities = desc->required_capabilities;
    node->effect_record = desc->effect.record;
    node->effect_params = desc->effect.params;
    node->effect_output_count = desc->effect.output_count;
    if (node->effect_output_count)
        memcpy(node->effect_outputs, desc->effect.outputs,
               (size_t)node->effect_output_count * sizeof(*node->effect_outputs));
    node->access_count = desc->access_count;
    if (node->access_count)
        memcpy(node->accesses, desc->accesses,
               (size_t)node->access_count * sizeof(*node->accesses));
    node->dependency_count = desc->dependency_count;
    if (node->dependency_count)
        memcpy(node->dependencies, desc->dependencies,
               (size_t)node->dependency_count * sizeof(*node->dependencies));
    if (out_index)
        *out_index = plan->node_count;
    plan->node_count++;
    return FLUX_OK;
}

uint32_t flux_render_plan_last_writer(const flux_render_plan *plan, const void *handle) {
    if (!plan || !handle)
        return UINT32_MAX;
    for (uint32_t i = plan->node_count; i-- > 0;) {
        const flux_render_node *node = &plan->nodes[i];
        for (uint32_t a = 0; a < node->access_count; ++a) {
            const flux_render_access *access = &node->accesses[a];
            if (is_writer(access->mode) && access->resource.kind == FLUX_RENDER_RESOURCE_IMAGE &&
                access->resource.handle == handle)
                return i;
        }
    }
    return UINT32_MAX;
}

flux_result flux_render_plan_node_add_access(flux_render_plan *plan, uint32_t node_index,
                                             const flux_render_access *access) {
    if (!plan || !access || node_index >= plan->node_count)
        return FLUX_ERROR_INVALID_ARGUMENT;
    flux_render_node *node = &plan->nodes[node_index];
    if (node->access_count >= FLUX_RENDER_PLAN_MAX_ACCESSES)
        return FLUX_ERROR_OUT_OF_RANGE;
    node->accesses[node->access_count++] = *access;
    return FLUX_OK;
}

flux_result flux_render_plan_node_add_dependency(flux_render_plan *plan, uint32_t node_index,
                                                 uint32_t dependency) {
    if (!plan || node_index >= plan->node_count)
        return FLUX_ERROR_INVALID_ARGUMENT;
    if (dependency >= plan->node_count || dependency == node_index)
        return FLUX_ERROR_INVALID_STATE;
    flux_render_node *node = &plan->nodes[node_index];
    for (uint32_t i = 0; i < node->dependency_count; ++i)
        if (node->dependencies[i] == dependency)
            return FLUX_OK;
    if (node->dependency_count >= FLUX_RENDER_PLAN_MAX_DEPENDENCIES)
        return FLUX_ERROR_OUT_OF_RANGE;
    node->dependencies[node->dependency_count++] = dependency;
    return FLUX_OK;
}
