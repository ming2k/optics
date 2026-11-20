/*
 * flux/scene.h — 3D scene primitives.
 *
 * Scope: camera, mesh, material, depth-tested renderer. No scene
 * graph, no PBR, no animation in the base library.
 */

#ifndef OPTICS_GFX_SCENE_H
#define OPTICS_GFX_SCENE_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Camera (value type)                                               */
/* ------------------------------------------------------------------ */

typedef struct optics_gfx_camera {
    optics_gfx_mat4 view;
    optics_gfx_mat4 projection;
} optics_gfx_camera;

typedef struct optics_gfx_camera_perspective_opts {
    float fov_y_rad; /* vertical field of view, radians */
    float aspect;    /* width / height                  */
    float z_near;
    float z_far;
} optics_gfx_camera_perspective_opts;

OPTICS_GFX_API void optics_gfx_camera_perspective(optics_gfx_camera *cam,
                                                  const optics_gfx_camera_perspective_opts *opts);

OPTICS_GFX_API void optics_gfx_camera_look_at(optics_gfx_camera *cam, optics_gfx_vec3 eye,
                                              optics_gfx_vec3 center, optics_gfx_vec3 up);

/* ------------------------------------------------------------------ */
/*  Mesh (refcounted GPU buffers)                                     */
/* ------------------------------------------------------------------ */

typedef struct optics_gfx_mesh optics_gfx_mesh;

typedef struct optics_gfx_vertex {
    float position[3];
    float normal[3];
    float uv[2];
} optics_gfx_vertex;

typedef struct optics_gfx_mesh_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_MESH_DESC */
    const void *next;
    const optics_gfx_vertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices; /* may be NULL for non-indexed */
    uint32_t index_count;
} optics_gfx_mesh_desc;

#define OPTICS_GFX_MESH_DESC_INIT {.type = OPTICS_GFX_TYPE_MESH_DESC}

/* Optional `flux_mesh_desc.next` payload for GPU skinning. Joint indices are
 * local to the palette supplied at draw time; weights are normalised by the
 * content loader, but the shader defensively handles a zero-sum vertex. */
typedef struct optics_gfx_skin_vertex {
    uint16_t joints[4];
    float weights[4];
} optics_gfx_skin_vertex;

typedef struct optics_gfx_mesh_skin_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_MESH_SKIN_DESC */
    const void *next;
    const optics_gfx_skin_vertex *vertices; /* exactly mesh_desc.vertex_count */
} optics_gfx_mesh_skin_desc;

#define OPTICS_GFX_MESH_SKIN_DESC_INIT {.type = OPTICS_GFX_TYPE_MESH_SKIN_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_mesh_create(
    optics_gfx_device *d, const optics_gfx_mesh_desc *desc, optics_gfx_mesh **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_mesh *optics_gfx_mesh_retain(optics_gfx_mesh *m);
/* Destruction is deferred: a released mesh's vertex/index buffers may
 * still be bound by in-flight frames, so they are destroyed by the
 * device retire queue only after the GPU provably passed every batch
 * that could reference them. */
OPTICS_GFX_API void optics_gfx_mesh_release(optics_gfx_mesh *m);

/* ------------------------------------------------------------------ */
/*  Material (built-in pipelines; bindless-aware)                     */
/* ------------------------------------------------------------------ */

typedef struct optics_gfx_material optics_gfx_material;
typedef struct optics_gfx_sampler optics_gfx_sampler;

typedef enum optics_gfx_material_kind : uint32_t {
    OPTICS_GFX_MATERIAL_UNLIT = 0,
    /* Blinn-Phong with one directional light per draw. Light data is
     * supplied at draw time via flux_scene_draw_mesh_lit; draws made
     * through flux_scene_draw_mesh use FLUX_SCENE_LIGHT_DEFAULT. */
    OPTICS_GFX_MATERIAL_PHONG = 1,
    /* ADR-0117: Perspective-correct analytic vector coverage. Consumes
     * object-space curve segments via flux_material_vector_desc. */
    OPTICS_GFX_MATERIAL_VECTOR = 2,
    /* PBR will be added at the end when it ships. Append-only —
     * never repurpose existing values. */
} optics_gfx_material_kind;

typedef enum optics_gfx_material_alpha_mode : uint32_t {
    OPTICS_GFX_MATERIAL_ALPHA_OPAQUE = 0,
    OPTICS_GFX_MATERIAL_ALPHA_MASK = 1,
    OPTICS_GFX_MATERIAL_ALPHA_BLEND = 2,
} optics_gfx_material_alpha_mode;

/* Optional `flux_material_desc.next` payload for a sampled base-colour
 * texture and glTF-compatible surface state. The material retains image and
 * sampler. A NULL sampler selects Flux's linear clamp-to-edge sampler.
 * UV transform order is scale, counter-clockwise rotation, then offset. */
typedef struct optics_gfx_material_surface_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_MATERIAL_SURFACE_DESC */
    const void *next;
    optics_gfx_image *base_color_image;     /* optional */
    optics_gfx_sampler *base_color_sampler; /* optional; ignored without image */
    optics_gfx_vec2 uv_offset;
    optics_gfx_vec2 uv_scale;
    float uv_rotation;
    optics_gfx_material_alpha_mode alpha_mode;
    float alpha_cutoff; /* MASK only; glTF default is 0.5 */
    bool double_sided;
} optics_gfx_material_surface_desc;

#define OPTICS_GFX_MATERIAL_SURFACE_DESC_INIT                                                      \
    {.type = OPTICS_GFX_TYPE_MATERIAL_SURFACE_DESC, .uv_scale = {1.0f, 1.0f}, .alpha_cutoff = 0.5f}

/* Optional `flux_material_desc.next` payload for FLUX_MATERIAL_VECTOR (ADR-0117).
 * Consumes an object-space line/curve segment storage buffer. */
typedef struct optics_gfx_material_vector_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_MATERIAL_VECTOR_DESC */
    const void *next;
    optics_gfx_buffer
        *segments_buffer; /* Storage buffer containing object-space segments (x0,y0,x1,y1) */
    uint32_t segment_count;
    uint32_t fill_rule; /* 0 = NonZero, 1 = EvenOdd */
} optics_gfx_material_vector_desc;

#define OPTICS_GFX_MATERIAL_VECTOR_DESC_INIT {.type = OPTICS_GFX_TYPE_MATERIAL_VECTOR_DESC}

typedef struct optics_gfx_material_desc {
    optics_gfx_struct_type type; /* FLUX_TYPE_MATERIAL_DESC */
    const void *next;
    optics_gfx_material_kind kind;
    optics_gfx_vec4 base_color;

    /* Render-target formats the pipeline must be compatible with.
     * Translate via flux_format_from_vk(flux_surface_vk_format(...))
     * for the colour target; pass the depth format you'll attach in
     * flux_pass_desc.depth, or FLUX_FORMAT_UNDEFINED for no depth. */
    optics_gfx_format color_format;
    optics_gfx_format depth_format;

    /* FLUX_MATERIAL_PHONG only; ignored by UNLIT.
     * shininess: Blinn-Phong specular exponent. <= 0 selects the
     *            library default (32).
     * specular:  specular strength in [0, 1]. 0 (the zero-init value)
     *            disables the highlight entirely. */
    float shininess;
    float specular;
} optics_gfx_material_desc;

#define OPTICS_GFX_MATERIAL_DESC_INIT {.type = OPTICS_GFX_TYPE_MATERIAL_DESC}

OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_result optics_gfx_material_create(
    optics_gfx_device *d, const optics_gfx_material_desc *desc, optics_gfx_material **out);
OPTICS_GFX_NODISCARD OPTICS_GFX_API optics_gfx_material *
optics_gfx_material_retain(optics_gfx_material *m);
/* Drop one owned reference. GPU objects retire after submitted use completes.
 * Keep resources alive through submission; raw command buffers follow the
 * same rule. No caller-side device-idle wait is required after submission. */
OPTICS_GFX_API void optics_gfx_material_release(optics_gfx_material *m);

OPTICS_GFX_API optics_gfx_material_alpha_mode
optics_gfx_material_get_alpha_mode(const optics_gfx_material *m);

/* ------------------------------------------------------------------ */
/*  Light (value type)                                                */
/* ------------------------------------------------------------------ */

/* One directional light, world space. `direction` is the direction
 * the light travels (sunlight vector, surface-bound); it is
 * normalised internally, and a zero vector falls back to the default
 * light's direction. `ambient` scales `color` for the unlit term. */
typedef struct optics_gfx_scene_light {
    optics_gfx_vec3 direction;
    optics_gfx_vec3 color; /* linear RGB intensity */
    float ambient;         /* ambient = base_color * ambient * color */
} optics_gfx_scene_light;

#define OPTICS_GFX_SCENE_LIGHT_DEFAULT                                                             \
    {.direction = {-0.4f, -0.8f, -0.45f}, .color = {1.0f, 1.0f, 1.0f}, .ambient = 0.08f}

/* ------------------------------------------------------------------ */
/*  Scene draw (records into the current pass)                        */
/* ------------------------------------------------------------------ */

/* Records a single mesh+material draw into the frame's currently
 * active pass. Must be called between flux_frame_begin_pass and
 * flux_frame_end_pass on the same frame, and the active pass's
 * attachments must match the material's color_format and depth_format
 * exactly. No-op if required arguments are NULL. PHONG materials use
 * `light` if non-NULL, or FLUX_SCENE_LIGHT_DEFAULT when NULL; UNLIT
 * materials ignore it. When `joint_matrices` is non-NULL and
 * `joint_count > 0` on a skinned mesh, GPU skinning is enabled. */
typedef struct optics_gfx_scene_draw_mesh_opts {
    const optics_gfx_camera *camera;
    optics_gfx_mat4 world;
    optics_gfx_mesh *mesh;
    optics_gfx_material *material;
    const optics_gfx_scene_light *light;
    const optics_gfx_mat4 *joint_matrices;
    uint32_t joint_count;
} optics_gfx_scene_draw_mesh_opts;

OPTICS_GFX_API void optics_gfx_scene_draw_mesh(optics_gfx_frame *f,
                                               const optics_gfx_scene_draw_mesh_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_SCENE_H */
