/*
 * render_plan.h — target-specific RenderPlan IR (ADR-0089 / ADR-0092).
 *
 * This is the planning authority, not an executor. A node declares what it
 * reads and writes, the region it touches, the capabilities it needs, its
 * transient budget, and the nodes it depends on. Validation rejects missing
 * ordering, cycles, write/write conflicts, out-of-budget plans, and invalid
 * target contracts before a backend can emit a command.
 */
#ifndef OPTICS_GFX_CORE_RENDER_PLAN_H
#define OPTICS_GFX_CORE_RENDER_PLAN_H

#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>
#include <vulkan/vulkan.h>

struct optics_gfx_image;

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OPTICS_GFX_RENDER_PLAN_MAX_NODES 64u
#define OPTICS_GFX_RENDER_PLAN_MAX_ACCESSES 16u
#define OPTICS_GFX_RENDER_PLAN_MAX_DEPENDENCIES 8u
#define OPTICS_GFX_RENDER_PLAN_MAX_REGION_RECTS 8u

typedef enum optics_gfx_render_node_kind : uint32_t {
    OPTICS_GFX_RENDER_NODE_UPLOAD = 1,
    OPTICS_GFX_RENDER_NODE_CANVAS = 2,
    OPTICS_GFX_RENDER_NODE_SCENE = 3,
    OPTICS_GFX_RENDER_NODE_COMPUTE = 4,
    OPTICS_GFX_RENDER_NODE_EFFECT = 5,
    OPTICS_GFX_RENDER_NODE_OUTPUT = 6,
} optics_gfx_render_node_kind;

typedef enum optics_gfx_render_resource_kind : uint32_t {
    OPTICS_GFX_RENDER_RESOURCE_IMAGE = 1,
    OPTICS_GFX_RENDER_RESOURCE_BUFFER = 2,
} optics_gfx_render_resource_kind;

typedef enum optics_gfx_render_access_mode : uint32_t {
    OPTICS_GFX_RENDER_ACCESS_READ = 1,
    OPTICS_GFX_RENDER_ACCESS_WRITE = 2,
    OPTICS_GFX_RENDER_ACCESS_READ_WRITE = 3,
} optics_gfx_render_access_mode;

/* Capability bits a target or node may require. Backends map these to their
 * own feature checks; the plan validator only enforces subset relationships. */
enum {
    OPTICS_GFX_RENDER_CAP_SAMPLED_IMAGE = 1ull << 0,
    OPTICS_GFX_RENDER_CAP_STORAGE_IMAGE = 1ull << 1,
    OPTICS_GFX_RENDER_CAP_STORAGE_BUFFER = 1ull << 2,
    OPTICS_GFX_RENDER_CAP_TRANSFER = 1ull << 3,
    OPTICS_GFX_RENDER_CAP_COLOR_ATTACHMENT = 1ull << 4,
    OPTICS_GFX_RENDER_CAP_COMPUTE = 1ull << 5,
    OPTICS_GFX_RENDER_CAP_SCENE = 1ull << 6,
};

typedef struct optics_gfx_render_region {
    optics_gfx_rect rects[OPTICS_GFX_RENDER_PLAN_MAX_REGION_RECTS];
    uint32_t count;
    bool full; /* whole target extent; rects ignored */
    uint8_t _pad[3];
} optics_gfx_render_region;

/* A resource identity is handle + immutable version. Version 0 means the
 * caller has no version and the plan treats every access as the same
 * unversioned identity; versioned resources must use nonzero values. */
typedef struct optics_gfx_render_resource {
    optics_gfx_render_resource_kind kind;
    const void *handle;
    uint64_t version;
} optics_gfx_render_resource;

typedef struct optics_gfx_render_access {
    optics_gfx_render_resource resource;
    optics_gfx_render_access_mode mode;
    optics_gfx_render_region region;
} optics_gfx_render_access;

typedef struct optics_gfx_render_target {
    uint32_t width;
    uint32_t height;
    float scale;
    optics_gfx_format format;
    optics_gfx_color_space color_space;
    uint32_t sample_count;
    bool load;
    bool store;
    uint8_t _pad[2];
    uint64_t capabilities;
    size_t transient_budget_bytes; /* zero = unbounded */
} optics_gfx_render_target;

typedef void (*optics_gfx_render_record_fn)(void *params, VkCommandBuffer cmd);

typedef struct optics_gfx_render_effect_desc {
    optics_gfx_render_record_fn record;
    void *params;
    struct optics_gfx_image *outputs[2];
    uint32_t output_count;
} optics_gfx_render_effect_desc;

typedef struct optics_gfx_render_node_desc {
    optics_gfx_render_node_kind kind;
    const char *label; /* borrowed; must outlive validation if diagnostics read it */
    const optics_gfx_render_access *accesses;
    uint32_t access_count;
    const uint32_t *dependencies;
    uint32_t dependency_count;
    size_t transient_bytes;
    uint64_t required_capabilities;
    optics_gfx_render_effect_desc effect;
} optics_gfx_render_node_desc;

typedef struct optics_gfx_render_node {
    optics_gfx_render_node_kind kind;
    const char *label;
    optics_gfx_render_access accesses[OPTICS_GFX_RENDER_PLAN_MAX_ACCESSES];
    uint32_t access_count;
    uint32_t dependencies[OPTICS_GFX_RENDER_PLAN_MAX_DEPENDENCIES];
    uint32_t dependency_count;
    size_t transient_bytes;
    uint64_t required_capabilities;
    optics_gfx_render_record_fn effect_record;
    void *effect_params;
    struct optics_gfx_image *effect_outputs[2];
    uint32_t effect_output_count;
    VkCommandBuffer effect_segment;
    bool effect_required;
    bool effect_built;
    bool effect_executed;
} optics_gfx_render_node;

typedef struct optics_gfx_render_plan {
    optics_gfx_render_target target;
    optics_gfx_render_node nodes[OPTICS_GFX_RENDER_PLAN_MAX_NODES];
    uint32_t node_count;
} optics_gfx_render_plan;

void optics_gfx_render_plan_init(optics_gfx_render_plan *plan,
                                 const optics_gfx_render_target *target);
optics_gfx_result optics_gfx_render_plan_add_node(optics_gfx_render_plan *plan,
                                                  const optics_gfx_render_node_desc *desc,
                                                  uint32_t *out_index);
/* Validate the complete plan. Pure: no device, no allocation. */
optics_gfx_result optics_gfx_render_plan_validate(const optics_gfx_render_plan *plan);

/* Index of the last node that writes `handle` as an image, or UINT32_MAX.
 * Callers name a producer through this instead of keeping a second last-writer
 * pointer beside the plan. */
uint32_t optics_gfx_render_plan_last_writer(const optics_gfx_render_plan *plan, const void *handle);

/* Append one access or dependency to an existing node. A Canvas pass discovers
 * sampled images while it records, after the node was created; these helpers
 * keep that mutation inside the IR instead of teaching every caller the array
 * layout. Duplicate dependencies are a no-op. Capacity overflow fails closed. */
optics_gfx_result optics_gfx_render_plan_node_add_access(optics_gfx_render_plan *plan,
                                                         uint32_t node_index,
                                                         const optics_gfx_render_access *access);
optics_gfx_result optics_gfx_render_plan_node_add_dependency(optics_gfx_render_plan *plan,
                                                             uint32_t node_index,
                                                             uint32_t dependency);

#endif /* FLUX_CORE_RENDER_PLAN_H */
