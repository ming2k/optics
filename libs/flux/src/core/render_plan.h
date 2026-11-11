/*
 * render_plan.h — target-specific RenderPlan IR (ADR-0089 / ADR-0092).
 *
 * This is the planning authority, not an executor. A node declares what it
 * reads and writes, the region it touches, the capabilities it needs, its
 * transient budget, and the nodes it depends on. Validation rejects missing
 * ordering, cycles, write/write conflicts, out-of-budget plans, and invalid
 * target contracts before a backend can emit a command.
 */
#ifndef FLUX_CORE_RENDER_PLAN_H
#define FLUX_CORE_RENDER_PLAN_H

#include <flux/canvas.h>
#include <flux/core.h>
#include <vulkan/vulkan.h>

struct flux_image;

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FLUX_RENDER_PLAN_MAX_NODES 64u
#define FLUX_RENDER_PLAN_MAX_ACCESSES 16u
#define FLUX_RENDER_PLAN_MAX_DEPENDENCIES 8u
#define FLUX_RENDER_PLAN_MAX_REGION_RECTS 8u

typedef enum flux_render_node_kind {
    FLUX_RENDER_NODE_UPLOAD = 1,
    FLUX_RENDER_NODE_CANVAS = 2,
    FLUX_RENDER_NODE_SCENE = 3,
    FLUX_RENDER_NODE_COMPUTE = 4,
    FLUX_RENDER_NODE_EFFECT = 5,
    FLUX_RENDER_NODE_OUTPUT = 6,
} flux_render_node_kind;

typedef enum flux_render_resource_kind {
    FLUX_RENDER_RESOURCE_IMAGE = 1,
    FLUX_RENDER_RESOURCE_BUFFER = 2,
} flux_render_resource_kind;

typedef enum flux_render_access_mode {
    FLUX_RENDER_ACCESS_READ = 1,
    FLUX_RENDER_ACCESS_WRITE = 2,
    FLUX_RENDER_ACCESS_READ_WRITE = 3,
} flux_render_access_mode;

/* Capability bits a target or node may require. Backends map these to their
 * own feature checks; the plan validator only enforces subset relationships. */
enum {
    FLUX_RENDER_CAP_SAMPLED_IMAGE = 1ull << 0,
    FLUX_RENDER_CAP_STORAGE_IMAGE = 1ull << 1,
    FLUX_RENDER_CAP_STORAGE_BUFFER = 1ull << 2,
    FLUX_RENDER_CAP_TRANSFER = 1ull << 3,
    FLUX_RENDER_CAP_COLOR_ATTACHMENT = 1ull << 4,
    FLUX_RENDER_CAP_COMPUTE = 1ull << 5,
    FLUX_RENDER_CAP_SCENE = 1ull << 6,
};

typedef struct flux_render_region {
    flux_rect rects[FLUX_RENDER_PLAN_MAX_REGION_RECTS];
    uint32_t count;
    bool full; /* whole target extent; rects ignored */
    uint8_t _pad[3];
} flux_render_region;

/* A resource identity is handle + immutable version. Version 0 means the
 * caller has no version and the plan treats every access as the same
 * unversioned identity; versioned resources must use nonzero values. */
typedef struct flux_render_resource {
    flux_render_resource_kind kind;
    const void *handle;
    uint64_t version;
} flux_render_resource;

typedef struct flux_render_access {
    flux_render_resource resource;
    flux_render_access_mode mode;
    flux_render_region region;
} flux_render_access;

typedef struct flux_render_target {
    uint32_t width;
    uint32_t height;
    float scale;
    flux_format format;
    flux_color_space color_space;
    uint32_t sample_count;
    bool load;
    bool store;
    uint8_t _pad[2];
    uint64_t capabilities;
    size_t transient_budget_bytes; /* zero = unbounded */
} flux_render_target;

typedef void (*flux_render_record_fn)(void *params, VkCommandBuffer cmd);

typedef struct flux_render_effect_desc {
    flux_render_record_fn record;
    void *params;
    struct flux_image *outputs[2];
    uint32_t output_count;
} flux_render_effect_desc;

typedef struct flux_render_node_desc {
    flux_render_node_kind kind;
    const char *label; /* borrowed; must outlive validation if diagnostics read it */
    const flux_render_access *accesses;
    uint32_t access_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    size_t transient_bytes;
    uint64_t required_capabilities;
    flux_render_effect_desc effect;
} flux_render_node_desc;

typedef struct flux_render_node {
    flux_render_node_kind kind;
    const char *label;
    flux_render_access accesses[FLUX_RENDER_PLAN_MAX_ACCESSES];
    uint32_t access_count;
    uint32_t dependencies[FLUX_RENDER_PLAN_MAX_DEPENDENCIES];
    uint32_t dependency_count;
    size_t transient_bytes;
    uint64_t required_capabilities;
    flux_render_record_fn effect_record;
    void *effect_params;
    struct flux_image *effect_outputs[2];
    uint32_t effect_output_count;
    VkCommandBuffer effect_segment;
    bool effect_required;
    bool effect_built;
    bool effect_executed;
} flux_render_node;

typedef struct flux_render_plan {
    flux_render_target target;
    flux_render_node nodes[FLUX_RENDER_PLAN_MAX_NODES];
    uint32_t node_count;
} flux_render_plan;

void flux_render_plan_init(flux_render_plan *plan, const flux_render_target *target);
flux_result flux_render_plan_add_node(flux_render_plan *plan, const flux_render_node_desc *desc,
                                      uint32_t *out_index);
/* Validate the complete plan. Pure: no device, no allocation. */
flux_result flux_render_plan_validate(const flux_render_plan *plan);

/* Index of the last node that writes `handle` as an image, or UINT32_MAX.
 * Callers name a producer through this instead of keeping a second last-writer
 * pointer beside the plan. */
uint32_t flux_render_plan_last_writer(const flux_render_plan *plan, const void *handle);

/* Append one access or dependency to an existing node. A Canvas pass discovers
 * sampled images while it records, after the node was created; these helpers
 * keep that mutation inside the IR instead of teaching every caller the array
 * layout. Duplicate dependencies are a no-op. Capacity overflow fails closed. */
flux_result flux_render_plan_node_add_access(flux_render_plan *plan, uint32_t node_index,
                                             const flux_render_access *access);
flux_result flux_render_plan_node_add_dependency(flux_render_plan *plan, uint32_t node_index,
                                                 uint32_t dependency);

#endif /* FLUX_CORE_RENDER_PLAN_H */
