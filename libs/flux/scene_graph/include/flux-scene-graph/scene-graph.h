/*
 * flux-scene-graph/scene-graph.h — glTF 2.0 content layer over flux.
 *
 * A sibling library to libflux (ADR-0016): it parses glTF, builds flux
 * mesh resources, and records scene draws. It feeds the flux_scene_draw_mesh
 * primitive — it never touches the canvas or scene module internals.
 *
 * Supported subset (v0.1):
 *   - Binary glTF (.glb) with one embedded buffer (the BIN chunk).
 *   - Indexed primitives with POSITION, NORMAL, optional TEXCOORD_0, and
 *     JOINTS_0/WEIGHTS_0 GPU skinning.
 *   - A single scene/node tree; mutable TRS poses compose into world matrices.
 *   - glTF animation sampling (STEP/LINEAR/CUBICSPLINE) and external VRM
 *     Animation 1.0 clips retargeted onto VRM 0.x or VRM 1.0 humanoid rigs.
 *   - Per-primitive materials may be installed by the host; callers may
 *     still override every primitive with one material at draw time.
 *
 * Out of subset (skipped, not fatal): external buffers/URIs, image decoding,
 * morph targets, and multiple scenes. Image decoding and glTF material
 * construction live in the safe Rust content layer. These remain
 * future work; the loader reports FLUX_ERROR_UNSUPPORTED only when a mesh
 * cannot be built at all.
 */
#ifndef FLUX_SCENE_GRAPH_H
#define FLUX_SCENE_GRAPH_H

#include <flux/core.h>  /* flux_device, flux_frame, flux_result        */
#include <flux/math.h>  /* flux_mat4                                    */
#include <flux/scene.h> /* flux_camera, flux_material, flux_scene_light*/

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(FLUX_SG_STATIC)
#ifdef FLUX_SG_BUILDING
#define FLUX_SG_API __declspec(dllexport)
#else
#define FLUX_SG_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define FLUX_SG_API __attribute__((visibility("default")))
#else
#define FLUX_SG_API
#endif

#define FLUX_SG_VERSION_MAJOR 0
#define FLUX_SG_VERSION_MINOR 0
#define FLUX_SG_VERSION_PATCH 13

FLUX_SG_API const char *flux_sg_version_string(void);

/* ================================================================== */
/*  Scene                                                             */
/* ================================================================== */

/* Opaque loaded scene: a tree of nodes, each carrying an optional mesh
 * (one or more primitives), a local transform, and a cached world matrix.
 * Mesh primitives own refcounted flux_mesh handles and the base colour
 * parsed from the glTF material; the application supplies the flux_material
 * at draw time (it owns the render-target formats). */
typedef struct flux_sg_scene flux_sg_scene;
typedef struct flux_sg_animation flux_sg_animation;

/* Parse a .glb (binary glTF 2.0) and build GPU resources on `device`.
 * On success `*out` is a scene with refcount 1. Returns:
 *   FLUX_OK                      — at least one mesh primitive loaded.
 *   FLUX_ERROR_UNSUPPORTED       — parsed, but no loadable primitive found.
 *   FLUX_ERROR_INVALID_ARGUMENT  — not a .glb, or malformed container/JSON.
 *   FLUX_ERROR_OUT_OF_MEMORY.                                    */
FLUX_SG_API flux_result flux_sg_load_glb(flux_device *device, const void *glb_bytes,
                                         size_t byte_count, flux_sg_scene **out);

FLUX_SG_API flux_sg_scene *flux_sg_scene_retain(flux_sg_scene *scene);
FLUX_SG_API void flux_sg_scene_release(flux_sg_scene *scene);

/* Transactionally replace the scene-owned per-index material table and
 * fallback material. Every non-NULL material is retained. Primitive material
 * indices come directly from glTF; missing/out-of-range entries use fallback.
 * Passing an empty table and NULL fallback clears installed materials. */
FLUX_SG_API flux_result flux_sg_scene_set_materials(flux_sg_scene *scene,
                                                    flux_material *const *materials,
                                                    uint32_t material_count,
                                                    flux_material *fallback);

/* Number of mesh primitives in the scene (diagnostic). */
FLUX_SG_API uint32_t flux_sg_scene_primitive_count(const flux_sg_scene *scene);

/* World-space axis-aligned bounding box of every primitive the scene
 * draws (each primitive's local AABB is transformed by its owning
 * node's world matrix). Returns false if the scene has no primitives
 * with finite bounds. Use this to frame a camera around any loaded
 * model without hardcoding per-asset constants. */
FLUX_SG_API bool flux_sg_scene_bounds(const flux_sg_scene *scene, flux_vec3 *out_min,
                                      flux_vec3 *out_max);

/* Current model-space position of a VRM humanoid bone (for example "head" or
 * "hips"). Useful for cameras and attachments that must follow animation. */
FLUX_SG_API bool flux_sg_scene_humanoid_bone_position(const flux_sg_scene *scene,
                                                       const char *bone_name,
                                                       flux_vec3 *out_position);

/* Load the first animation from a binary glTF/VRMA file and bind its channels
 * to `target`. VRMC_vrm_animation humanoid channels are retargeted by bone
 * identity, including VRM 0.x thumb-name compatibility and rest-pose rotation
 * conversion. Ordinary glTF clips fall back to node-name matching. The clip
 * retains `target` and remains permanently bound to that exact scene. */
FLUX_SG_API flux_result flux_sg_load_animation_glb(const flux_sg_scene *target,
                                                   const void *glb_bytes, size_t byte_count,
                                                   flux_sg_animation **out);
FLUX_SG_API flux_sg_animation *flux_sg_animation_retain(flux_sg_animation *animation);
FLUX_SG_API void flux_sg_animation_release(flux_sg_animation *animation);
FLUX_SG_API float flux_sg_animation_duration(const flux_sg_animation *animation);
FLUX_SG_API uint32_t flux_sg_animation_channel_count(const flux_sg_animation *animation);

/* Reset to the model rest pose, then sample and apply the clip. When `loop` is
 * true, time wraps by duration; otherwise it clamps to the last key. */
FLUX_SG_API flux_result flux_sg_scene_apply_animation(flux_sg_scene *scene,
                                                      const flux_sg_animation *animation,
                                                      float time_seconds, bool loop);
FLUX_SG_API void flux_sg_scene_reset_pose(flux_sg_scene *scene);

/* ================================================================== */
/*  Draw                                                              */
/* ================================================================== */

typedef struct flux_sg_draw_opts {
    /* Optional whole-scene override. NULL selects the scene's installed
     * per-primitive materials and fallback. */
    flux_material *material;
    /* NULL = FLUX_SCENE_LIGHT_DEFAULT. Ignored by UNLIT materials. */
    const flux_scene_light *light;
} flux_sg_draw_opts;

/* Record one flux_scene_draw_mesh(_lit) per primitive, composed with each
 * node's world matrix. Must be called inside flux_frame_begin_pass /
 * flux_frame_end_pass on a pass whose attachments match `opts->material`'s
 * color/depth formats. OPAQUE/MASK primitives are recorded before BLEND
 * primitives. No-op if frame/cam/scene/opts is NULL. */
FLUX_SG_API void flux_sg_draw(flux_frame *frame, const flux_camera *cam, const flux_sg_scene *scene,
                              const flux_sg_draw_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* FLUX_SCENE_GRAPH_H */
