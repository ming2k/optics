/*
 * vista/vista.h — glTF 2.0 3D scene hierarchy, TRS transforms, and asset runtime (ADR-0016, ADR-0106).
 *
 * Sibling library to libflux: it parses glTF, builds flux mesh resources, and
 * records 3D scene draws. It feeds the flux_scene_draw_mesh primitive.
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

#ifndef VISTA_H
#define VISTA_H

#include <flux/core.h>  /* flux_device, flux_frame, flux_result        */
#include <flux/math.h>  /* flux_mat4, flux_vec3                         */
#include <flux/scene.h> /* flux_camera, flux_material, flux_scene_light*/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(VISTA_STATIC)
#ifdef VISTA_BUILDING
#define VISTA_API __declspec(dllexport)
#else
#define VISTA_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define VISTA_API __attribute__((visibility("default")))
#else
#define VISTA_API
#endif

#define VISTA_VERSION_MAJOR 0
#define VISTA_VERSION_MINOR 0
#define VISTA_VERSION_PATCH 54

/* Packed integer version, monotonic — identical layout to
 * FLUX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7). */
#define VISTA_VERSION_NUMBER                                                                       \
    (((uint32_t)VISTA_VERSION_MAJOR << 16) | ((uint32_t)VISTA_VERSION_MINOR << 8) |                \
     (uint32_t)VISTA_VERSION_PATCH)

VISTA_API void vista_version(int *major, int *minor, int *patch);
VISTA_API uint32_t vista_version_number(void);
VISTA_API bool vista_version_check(int major, int minor, int patch);
VISTA_API const char *vista_version_string(void);

/* ================================================================== */
/*  Scene                                                             */
/* ================================================================== */

/* Opaque loaded scene: a tree of nodes, each carrying an optional mesh
 * (one or more primitives), a local transform, and a cached world matrix. */
typedef struct vista_scene vista_scene;
typedef struct vista_animation vista_animation;

/* Parse a .glb (binary glTF 2.0) and build GPU resources on `device`. */
FLUX_NODISCARD VISTA_API flux_result vista_load_glb(flux_device *device, const void *glb_bytes,
                                                    size_t byte_count, vista_scene **out);

/* Opaque parsed scene: everything a .glb contains, minus the device. */
typedef struct vista_scene_data vista_scene_data;

/* Parse a .glb into device-independent scene data (CPU parse only). */
FLUX_NODISCARD VISTA_API flux_result vista_parse_glb(const void *glb_bytes, size_t byte_count,
                                                     vista_scene_data **out);

/* Upload a parsed scene onto `device`: one flux_mesh per primitive. */
FLUX_NODISCARD VISTA_API flux_result vista_scene_data_build(flux_device *device,
                                                            vista_scene_data *data,
                                                            vista_scene **out);

/* Release a parsed scene data. */
VISTA_API void vista_scene_data_free(vista_scene_data *data);

/* Number of mesh primitives in the parsed scene. */
VISTA_API uint32_t vista_scene_data_primitive_count(const vista_scene_data *data);

/* World-space axis-aligned bounding box of parsed primitives. */
VISTA_API bool vista_scene_data_bounds(const vista_scene_data *data, flux_vec3 *out_min,
                                       flux_vec3 *out_max);

VISTA_API vista_scene *vista_scene_retain(vista_scene *scene);
VISTA_API void vista_scene_release(vista_scene *scene);

/* Transactionally replace the scene-owned per-index material table. */
FLUX_NODISCARD VISTA_API flux_result vista_scene_set_materials(vista_scene *scene,
                                                               flux_material *const *materials,
                                                               uint32_t material_count,
                                                               flux_material *fallback);

/* Number of mesh primitives in the scene. */
VISTA_API uint32_t vista_scene_primitive_count(const vista_scene *scene);

/* World-space axis-aligned bounding box of every primitive drawn by the scene. */
VISTA_API bool vista_scene_bounds(const vista_scene *scene, flux_vec3 *out_min,
                                  flux_vec3 *out_max);

/* Current model-space position of a VRM humanoid bone (e.g. "head" or "hips"). */
VISTA_API bool vista_scene_humanoid_bone_position(const vista_scene *scene,
                                                  const char *bone_name,
                                                  flux_vec3 *out_position);

/* Load animation from a binary glTF/VRMA file and bind its channels to `target`. */
FLUX_NODISCARD VISTA_API flux_result vista_load_animation_glb(const vista_scene *target,
                                                              const void *glb_bytes,
                                                              size_t byte_count,
                                                              vista_animation **out);
VISTA_API vista_animation *vista_animation_retain(vista_animation *animation);
VISTA_API void vista_animation_release(vista_animation *animation);
VISTA_API float vista_animation_duration(const vista_animation *animation);
VISTA_API uint32_t vista_animation_channel_count(const vista_animation *animation);

/* Reset to the model rest pose, then sample and apply the clip. */
FLUX_NODISCARD VISTA_API flux_result vista_scene_apply_animation(
    vista_scene *scene, const vista_animation *animation, float time_seconds, bool loop);
VISTA_API void vista_scene_reset_pose(vista_scene *scene);

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

typedef struct vista_draw_opts {
    flux_material *material;
    const flux_scene_light *light;
} vista_draw_opts;

/* Record mesh draw calls composed with each node's world matrix. */
VISTA_API void vista_draw(flux_frame *frame, const flux_camera *cam, const vista_scene *scene,
                          const vista_draw_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* VISTA_H */
