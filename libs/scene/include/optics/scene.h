/*
 * scene/scene.h — glTF 2.0 3D scene hierarchy, TRS transforms, and asset runtime (ADR-0016,
 * ADR-0106).
 *
 * Sibling library to liboptics_gfx: it parses glTF, builds gfx mesh resources, and
 * records 3D scene draws. It feeds the optics_gfx_scene_draw_mesh primitive.
 *
 * Supported subset:
 *   - Binary glTF (.glb) with embedded buffer (the BIN chunk).
 *   - Indexed primitives with POSITION, NORMAL, optional TEXCOORD_0, and
 *     JOINTS_0/WEIGHTS_0 GPU skinning.
 *   - Single scene/node tree; mutable TRS poses compose into world matrices.
 *   - glTF animation sampling (STEP/LINEAR/CUBICSPLINE) and external VRM
 *     Animation 1.0 clips retargeted onto VRM 0.x or VRM 1.0 humanoid rigs.
 *   - Per-primitive materials may be installed by the host; callers may
 *     still override every primitive with one material at draw time.
 */

#ifndef OPTICS_SCENE_H
#define OPTICS_SCENE_H

#include <optics/gfx/core.h>  /* optics_gfx_device, optics_gfx_frame, optics_gfx_result        */
#include <optics/gfx/math.h>  /* optics_gfx_mat4, optics_gfx_vec3                         */
#include <optics/gfx/scene.h> /* optics_gfx_camera, optics_gfx_material, optics_gfx_scene_light*/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(OPTICS_SCENE_STATIC)
#ifdef OPTICS_SCENE_BUILDING
#define OPTICS_SCENE_API __declspec(dllexport)
#else
#define OPTICS_SCENE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define OPTICS_SCENE_API __attribute__((visibility("default")))
#else
#define OPTICS_SCENE_API
#endif

#define OPTICS_SCENE_VERSION_MAJOR 0
#define OPTICS_SCENE_VERSION_MINOR 0
#define OPTICS_SCENE_VERSION_PATCH 84

/* Packed integer version, monotonic — identical layout to
 * OPTICS_GFX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7). */
#define OPTICS_SCENE_VERSION_NUMBER                                                                \
    (((uint32_t)OPTICS_SCENE_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_SCENE_VERSION_MINOR << 8) |  \
     (uint32_t)OPTICS_SCENE_VERSION_PATCH)

OPTICS_SCENE_API void optics_scene_version(int *major, int *minor, int *patch);
OPTICS_SCENE_API uint32_t optics_scene_version_number(void);
OPTICS_SCENE_API bool optics_scene_version_check(int major, int minor, int patch);
OPTICS_SCENE_API const char *optics_scene_version_string(void);

/* ================================================================== */
/*  Scene                                                             */
/* ================================================================== */

/* Opaque loaded scene: a tree of nodes, each carrying an optional mesh
 * (one or more primitives), a local transform, and a cached world matrix. */
typedef struct optics_scene optics_scene;
typedef struct optics_scene_animation optics_scene_animation;

typedef struct optics_scene_load_glb_opts {
    const void *bytes;
    size_t byte_count;
    optics_scene **out;
} optics_scene_load_glb_opts;

/* Parse a .glb (binary glTF 2.0) and build GPU resources on `device`. */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result
optics_scene_load_glb(optics_gfx_device *device, const optics_scene_load_glb_opts *opts);

/* Opaque parsed scene: everything a .glb contains, minus the device. */
typedef struct optics_scene_data optics_scene_data;

typedef struct optics_scene_parse_glb_opts {
    const void *bytes;
    size_t byte_count;
    optics_scene_data **out;
} optics_scene_parse_glb_opts;

/* Parse a .glb into device-independent scene data (CPU parse only). */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result
optics_scene_parse_glb(const optics_scene_parse_glb_opts *opts);

typedef struct optics_scene_data_build_opts {
    optics_scene_data *data;
    optics_scene **out;
} optics_scene_data_build_opts;

/* Upload a parsed scene onto `device`: one optics_gfx_mesh per primitive. */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result
optics_scene_data_build(optics_gfx_device *device, const optics_scene_data_build_opts *opts);

/* Release a parsed scene data. */
OPTICS_SCENE_API void optics_scene_data_free(optics_scene_data *data);

/* Number of mesh primitives in the parsed scene. */
OPTICS_SCENE_API uint32_t optics_scene_data_primitive_count(const optics_scene_data *data);

typedef struct optics_scene_data_bounds_opts {
    optics_gfx_vec3 *out_min;
    optics_gfx_vec3 *out_max;
} optics_scene_data_bounds_opts;

/* World-space axis-aligned bounding box of parsed primitives. */
OPTICS_SCENE_API bool optics_scene_data_bounds(const optics_scene_data *data,
                                               const optics_scene_data_bounds_opts *opts);

OPTICS_SCENE_API optics_scene *optics_scene_retain(optics_scene *scene);
OPTICS_SCENE_API void optics_scene_release(optics_scene *scene);

typedef struct optics_scene_set_materials_opts {
    optics_gfx_material *const *materials;
    uint32_t material_count;
    optics_gfx_material *fallback;
} optics_scene_set_materials_opts;

/* Transactionally replace the scene-owned per-index material table. */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result
optics_scene_set_materials(optics_scene *scene, const optics_scene_set_materials_opts *opts);

/* Number of mesh primitives in the scene. */
OPTICS_SCENE_API uint32_t optics_scene_primitive_count(const optics_scene *scene);

typedef struct optics_scene_bounds_opts {
    optics_gfx_vec3 *out_min;
    optics_gfx_vec3 *out_max;
} optics_scene_bounds_opts;

/* World-space axis-aligned bounding box of every primitive drawn by the scene. */
OPTICS_SCENE_API bool optics_scene_bounds(const optics_scene *scene,
                                          const optics_scene_bounds_opts *opts);

typedef struct optics_scene_humanoid_bone_position_opts {
    const char *bone_name;
    optics_gfx_vec3 *out_position;
} optics_scene_humanoid_bone_position_opts;

/* Current model-space position of a VRM humanoid bone (e.g. "head" or "hips"). */
OPTICS_SCENE_API bool
optics_scene_humanoid_bone_position(const optics_scene *scene,
                                    const optics_scene_humanoid_bone_position_opts *opts);

typedef struct optics_scene_load_animation_glb_opts {
    const void *bytes;
    size_t byte_count;
    optics_scene_animation **out;
} optics_scene_load_animation_glb_opts;

/* Load animation from a binary glTF/VRMA file and bind its channels to `target`. */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result optics_scene_load_animation_glb(
    const optics_scene *target, const optics_scene_load_animation_glb_opts *opts);
OPTICS_SCENE_API optics_scene_animation *
optics_scene_animation_retain(optics_scene_animation *animation);
OPTICS_SCENE_API void optics_scene_animation_release(optics_scene_animation *animation);
OPTICS_SCENE_API float optics_scene_animation_duration(const optics_scene_animation *animation);
OPTICS_SCENE_API uint32_t
optics_scene_animation_channel_count(const optics_scene_animation *animation);

typedef struct optics_scene_apply_animation_opts {
    const optics_scene_animation *animation;
    float time_seconds;
    bool loop;
} optics_scene_apply_animation_opts;

/* Reset to the model rest pose, then sample and apply the clip. */
OPTICS_GFX_NODISCARD OPTICS_SCENE_API optics_gfx_result
optics_scene_apply_animation(optics_scene *scene, const optics_scene_apply_animation_opts *opts);
OPTICS_SCENE_API void optics_scene_reset_pose(optics_scene *scene);

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

typedef struct optics_scene_draw_opts {
    const optics_gfx_camera *camera;
    const optics_scene *scene;
    optics_gfx_material *material;
    const optics_gfx_scene_light *light;
} optics_scene_draw_opts;

/* Record mesh draw calls composed with each node's world matrix. */
OPTICS_SCENE_API void optics_scene_draw(optics_gfx_frame *frame,
                                        const optics_scene_draw_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_SCENE_H */
