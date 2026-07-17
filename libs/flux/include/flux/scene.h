/*
 * flux/scene.h — 3D scene primitives.
 *
 * Scope: camera, mesh, material, depth-tested renderer. No scene
 * graph, no PBR, no animation in the base library.
 */

#ifndef FLUX_SCENE_H
#define FLUX_SCENE_H

#include <flux/core.h>
#include <flux/math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Camera (value type)                                               */
/* ------------------------------------------------------------------ */

typedef struct flux_camera {
    flux_mat4 view;
    flux_mat4 projection;
} flux_camera;

FLUX_API void flux_camera_perspective(flux_camera *cam, float fov_y_rad, float aspect, float z_near,
                                      float z_far);

FLUX_API void flux_camera_look_at(flux_camera *cam, flux_vec3 eye, flux_vec3 center, flux_vec3 up);

/* ------------------------------------------------------------------ */
/*  Mesh (refcounted GPU buffers)                                     */
/* ------------------------------------------------------------------ */

typedef struct flux_mesh flux_mesh;

typedef struct flux_vertex {
    float position[3];
    float normal[3];
    float uv[2];
} flux_vertex;

typedef struct flux_mesh_desc {
    flux_struct_type type; /* FLUX_TYPE_MESH_DESC */
    const void *next;
    const flux_vertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices; /* may be NULL for non-indexed */
    uint32_t index_count;
} flux_mesh_desc;

#define FLUX_MESH_DESC_INIT {.type = FLUX_TYPE_MESH_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_mesh_create(flux_device *d, const flux_mesh_desc *desc,
                                                     flux_mesh **out);
FLUX_NODISCARD FLUX_API flux_mesh *flux_mesh_retain(flux_mesh *m);
/* Destruction is deferred: a released mesh's vertex/index buffers may
 * still be bound by in-flight frames, so they are destroyed by the
 * device retire queue only after the GPU provably passed every batch
 * that could reference them. */
FLUX_API void flux_mesh_release(flux_mesh *m);

/* ------------------------------------------------------------------ */
/*  Material (built-in pipelines; bindless-aware)                     */
/* ------------------------------------------------------------------ */

typedef struct flux_material flux_material;

typedef enum flux_material_kind {
    FLUX_MATERIAL_UNLIT = 0,
    /* Blinn-Phong with one directional light per draw. Light data is
     * supplied at draw time via flux_scene_draw_mesh_lit; draws made
     * through flux_scene_draw_mesh use FLUX_SCENE_LIGHT_DEFAULT. */
    FLUX_MATERIAL_PHONG = 1,
    /* PBR will be added at the end when it ships. Append-only —
     * never repurpose existing values. */
} flux_material_kind;

typedef struct flux_material_desc {
    flux_struct_type type; /* FLUX_TYPE_MATERIAL_DESC */
    const void *next;
    flux_material_kind kind;
    flux_vec4 base_color;

    /* Render-target formats the pipeline must be compatible with.
     * Translate via flux_format_from_vk(flux_surface_vk_format(...))
     * for the colour target; pass the depth format you'll attach in
     * flux_pass_desc.depth, or FLUX_FORMAT_UNDEFINED for no depth. */
    flux_format color_format;
    flux_format depth_format;

    /* FLUX_MATERIAL_PHONG only; ignored by UNLIT.
     * shininess: Blinn-Phong specular exponent. <= 0 selects the
     *            library default (32).
     * specular:  specular strength in [0, 1]. 0 (the zero-init value)
     *            disables the highlight entirely. */
    float shininess;
    float specular;
} flux_material_desc;

#define FLUX_MATERIAL_DESC_INIT {.type = FLUX_TYPE_MATERIAL_DESC}

FLUX_NODISCARD FLUX_API flux_result flux_material_create(flux_device *d,
                                                         const flux_material_desc *desc,
                                                         flux_material **out);
FLUX_NODISCARD FLUX_API flux_material *flux_material_retain(flux_material *m);
FLUX_API void flux_material_release(flux_material *m);

/* ------------------------------------------------------------------ */
/*  Light (value type)                                                */
/* ------------------------------------------------------------------ */

/* One directional light, world space. `direction` is the direction
 * the light travels (sunlight vector, surface-bound); it is
 * normalised internally, and a zero vector falls back to the default
 * light's direction. `ambient` scales `color` for the unlit term. */
typedef struct flux_scene_light {
    flux_vec3 direction;
    flux_vec3 color; /* linear RGB intensity */
    float ambient;   /* ambient = base_color * ambient * color */
} flux_scene_light;

#define FLUX_SCENE_LIGHT_DEFAULT                                                                   \
    {.direction = {-0.4f, -0.8f, -0.45f}, .color = {1.0f, 1.0f, 1.0f}, .ambient = 0.08f}

/* ------------------------------------------------------------------ */
/*  Scene draw (records into the current pass)                        */
/* ------------------------------------------------------------------ */

/* Records a single mesh+material draw into the frame's currently
 * active pass. Must be called between flux_frame_begin_pass and
 * flux_frame_end_pass on the same frame, and the active pass's
 * attachments must match the material's color_format and depth_format
 * exactly. No-op if any argument is NULL. PHONG materials are lit
 * with FLUX_SCENE_LIGHT_DEFAULT. */
FLUX_API void flux_scene_draw_mesh(flux_frame *f, const flux_camera *cam, flux_mat4 world,
                                   flux_mesh *mesh, flux_material *material);

/* Same as flux_scene_draw_mesh with an explicit light. `light` may be
 * NULL for FLUX_SCENE_LIGHT_DEFAULT; UNLIT materials ignore it. The
 * light is consumed during the call — it does not need to outlive
 * the frame. */
FLUX_API void flux_scene_draw_mesh_lit(flux_frame *f, const flux_camera *cam, flux_mat4 world,
                                       flux_mesh *mesh, flux_material *material,
                                       const flux_scene_light *light);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_SCENE_H */
