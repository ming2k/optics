/*
 * flux-scene-graph — public API implementation.
 *
 * The scene owns refcounted flux_mesh handles and a node tree with cached
 * world matrices. Draw records one flux_scene_draw_mesh(_lit) per primitive,
 * composed with the owning node's world matrix, into the caller's active
 * pass. The scene never touches the canvas or scene module internals — it is
 * a pure consumer of the flux_scene_draw_mesh primitive (ADR-0016).
 */
#include "internal.h"

#include <flux/math.h>
#include <flux/scene.h>

#include <stdlib.h>

#define FLUX_SG_VERSION_STRING "0.0.18"

FLUX_SG_API const char *flux_sg_version_string(void) {
    return FLUX_SG_VERSION_STRING;
}

FLUX_SG_API flux_result flux_sg_load_glb(flux_device *device, const void *glb_bytes,
                                         size_t byte_count, flux_sg_scene **out) {
    if (!device || !glb_bytes || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    flux_sg_scene *sc = calloc(1, sizeof(*sc));
    if (!sc)
        return FLUX_ERROR_OUT_OF_MEMORY;
    sc->refcount = 1;
    flux_result r = sg_parse_glb(device, glb_bytes, byte_count, sc);
    if (r != FLUX_OK) {
        /* sg_parse_glb frees anything it allocated on failure. */
        free(sc);
        return r;
    }
    *out = sc;
    return FLUX_OK;
}

FLUX_SG_API flux_sg_scene *flux_sg_scene_retain(flux_sg_scene *scene) {
    if (scene)
        scene->refcount++;
    return scene;
}

FLUX_SG_API void flux_sg_scene_release(flux_sg_scene *scene) {
    if (!scene)
        return;
    if (--scene->refcount > 0)
        return;
    for (uint32_t i = 0; i < scene->prim_count; ++i)
        if (scene->prims[i].mesh)
            flux_mesh_release(scene->prims[i].mesh);
    for (uint32_t i = 0; i < scene->node_count; ++i)
        free(scene->nodes[i].name);
    for (uint32_t i = 0; i < scene->skin_count; ++i) {
        free(scene->skins[i].joints);
        free(scene->skins[i].inverse_bind);
        free(scene->skins[i].palette);
    }
    for (uint32_t i = 0; i < scene->material_count; ++i)
        flux_material_release(scene->materials[i]);
    flux_material_release(scene->fallback_material);
    free(scene->materials);
    free(scene->prims);
    free(scene->nodes);
    free(scene->skins);
    free(scene->roots);
    free(scene);
}

FLUX_SG_API flux_result flux_sg_scene_set_materials(flux_sg_scene *scene,
                                                    flux_material *const *materials,
                                                    uint32_t material_count,
                                                    flux_material *fallback) {
    if (!scene || (material_count > 0 && !materials))
        return FLUX_ERROR_INVALID_ARGUMENT;
    flux_material **replacement = NULL;
    if (material_count > 0) {
        replacement = calloc(material_count, sizeof(*replacement));
        if (!replacement)
            return FLUX_ERROR_OUT_OF_MEMORY;
        for (uint32_t i = 0; i < material_count; ++i)
            replacement[i] = flux_material_retain(materials[i]);
    }
    flux_material *replacement_fallback = flux_material_retain(fallback);

    flux_material **old = scene->materials;
    uint32_t old_count = scene->material_count;
    flux_material *old_fallback = scene->fallback_material;
    scene->materials = replacement;
    scene->material_count = material_count;
    scene->fallback_material = replacement_fallback;

    for (uint32_t i = 0; i < old_count; ++i)
        flux_material_release(old[i]);
    flux_material_release(old_fallback);
    free(old);
    return FLUX_OK;
}

FLUX_SG_API uint32_t flux_sg_scene_primitive_count(const flux_sg_scene *scene) {
    return scene ? scene->prim_count : 0;
}

FLUX_SG_API bool flux_sg_scene_humanoid_bone_position(const flux_sg_scene *scene,
                                                       const char *bone_name,
                                                       flux_vec3 *out_position) {
    if (!scene || !bone_name || !out_position)
        return false;
    int bone = sg_human_bone_index(bone_name, false);
    if (bone < 0)
        return false;
    int node = scene->human_bones[bone];
    if (node < 0 || (uint32_t)node >= scene->node_count)
        return false;
    *out_position = flux_vec3_make(scene->nodes[node].world.m[12], scene->nodes[node].world.m[13],
                                   scene->nodes[node].world.m[14]);
    return true;
}

static flux_mat4 node_local(const flux_sg_node *node) {
    flux_mat4 t = flux_mat4_translate(node->translation.x, node->translation.y, node->translation.z);
    flux_mat4 r = flux_mat4_rotation_quat(node->rotation);
    flux_mat4 s = flux_mat4_scale(node->scale.x, node->scale.y, node->scale.z);
    return flux_mat4_multiply(t, flux_mat4_multiply(r, s));
}

void sg_update_worlds(flux_sg_scene *scene) {
    if (!scene)
        return;
    for (uint32_t i = 0; i < scene->node_count; ++i) {
        flux_sg_node *node = &scene->nodes[i];
        node->local = node_local(node);
        node->world = node->parent < 0 ? node->local : flux_mat4_identity();
    }
    /* glTF node arrays are not required to be topologically sorted. Repeated
     * propagation converges within max tree depth without recursion or a
     * temporary allocation. */
    for (uint32_t pass = 0; pass < scene->node_count; ++pass)
        for (uint32_t i = 0; i < scene->node_count; ++i) {
            flux_sg_node *node = &scene->nodes[i];
            if (node->parent >= 0 && (uint32_t)node->parent < scene->node_count)
                node->world = flux_mat4_multiply(scene->nodes[node->parent].world, node->local);
        }
}

void sg_update_rest_world_rotations(flux_sg_scene *scene) {
    if (!scene)
        return;
    for (uint32_t i = 0; i < scene->node_count; ++i)
        scene->nodes[i].rest_world_rotation = scene->nodes[i].rest_rotation;
    for (uint32_t pass = 0; pass < scene->node_count; ++pass)
        for (uint32_t i = 0; i < scene->node_count; ++i) {
            flux_sg_node *node = &scene->nodes[i];
            if (node->parent >= 0 && (uint32_t)node->parent < scene->node_count)
                node->rest_world_rotation = flux_quat_normalize(flux_quat_multiply(
                    scene->nodes[node->parent].rest_world_rotation, node->rest_rotation));
        }
}

void sg_update_skin_palettes(flux_sg_scene *scene) {
    if (!scene)
        return;
    for (uint32_t si = 0; si < scene->skin_count; ++si) {
        flux_sg_skin *skin = &scene->skins[si];
        for (uint32_t ji = 0; ji < skin->joint_count; ++ji) {
            int node = skin->joints[ji];
            skin->palette[ji] =
                node >= 0 && (uint32_t)node < scene->node_count
                    ? flux_mat4_multiply(scene->nodes[node].world, skin->inverse_bind[ji])
                    : flux_mat4_identity();
        }
    }
}

/* Expand [*wmin, *wmax] to contain the 8 corners of [bmin, bmax] after
 * transformation by world matrix m. */
static void expand_world_aabb(flux_mat4 m, flux_vec3 bmin, flux_vec3 bmax, flux_vec3 *wmin,
                              flux_vec3 *wmax) {
    const float cx[2] = {bmin.x, bmax.x};
    const float cy[2] = {bmin.y, bmax.y};
    const float cz[2] = {bmin.z, bmax.z};
    for (int i = 0; i < 2; ++i)
        for (int j = 0; j < 2; ++j)
            for (int k = 0; k < 2; ++k) {
                flux_vec4 wp =
                    flux_mat4_transform_vec4(m, flux_vec4_make(cx[i], cy[j], cz[k], 1.0f));
                if (wp.x < wmin->x)
                    wmin->x = wp.x;
                if (wp.y < wmin->y)
                    wmin->y = wp.y;
                if (wp.z < wmin->z)
                    wmin->z = wp.z;
                if (wp.x > wmax->x)
                    wmax->x = wp.x;
                if (wp.y > wmax->y)
                    wmax->y = wp.y;
                if (wp.z > wmax->z)
                    wmax->z = wp.z;
            }
}

FLUX_SG_API bool flux_sg_scene_bounds(const flux_sg_scene *scene, flux_vec3 *out_min,
                                      flux_vec3 *out_max) {
    if (!scene || !out_min || !out_max)
        return false;
    flux_vec3 wmin = {1e30f, 1e30f, 1e30f};
    flux_vec3 wmax = {-1e30f, -1e30f, -1e30f};
    bool any = false;

    /* Match flux_sg_draw's visibility rule: when the scene has a node
     * tree, primitives are drawn via their owning node's world matrix;
     * a node-less scene draws all primitives with an identity world. */
    if (scene->node_count > 0) {
        for (uint32_t ni = 0; ni < scene->node_count; ++ni) {
            const flux_sg_node *n = &scene->nodes[ni];
            if (n->mesh_first < 0 || n->mesh_prim_count <= 0)
                continue;
            for (int k = n->mesh_first; k < n->mesh_first + n->mesh_prim_count; ++k) {
                if ((uint32_t)k >= scene->prim_count)
                    break;
                const flux_sg_primitive *p = &scene->prims[k];
                expand_world_aabb(n->world, p->aabb_min, p->aabb_max, &wmin, &wmax);
                any = true;
            }
        }
    } else {
        flux_mat4 id = flux_mat4_identity();
        for (uint32_t k = 0; k < scene->prim_count; ++k) {
            expand_world_aabb(id, scene->prims[k].aabb_min, scene->prims[k].aabb_max, &wmin, &wmax);
            any = true;
        }
    }
    if (!any)
        return false;
    *out_min = wmin;
    *out_max = wmax;
    return true;
}

static flux_material *primitive_material(const flux_sg_scene *scene,
                                         const flux_sg_primitive *primitive,
                                         flux_material *override) {
    if (override)
        return override;
    int index = primitive->material_index;
    if (index >= 0 && (uint32_t)index < scene->material_count && scene->materials[index])
        return scene->materials[index];
    return scene->fallback_material;
}

static void draw_primitive(flux_frame *frame, const flux_camera *cam, flux_mat4 world,
                           const flux_sg_primitive *primitive, flux_material *material,
                           const flux_scene_light *light, const flux_sg_skin *skin) {
    if (!primitive->mesh || !material)
        return;
    if (skin && skin->joint_count > 0) {
        /* Skin palettes already produce model/world-space positions, so the
         * draw world is identity (world * inverse(world) form). */
        flux_mat4 identity = flux_mat4_identity();
        if (light)
            flux_scene_draw_mesh_skinned_lit(frame, cam, identity, primitive->mesh, material,
                                              light, skin->palette, skin->joint_count);
        else
            flux_scene_draw_mesh_skinned(frame, cam, identity, primitive->mesh, material,
                                          skin->palette, skin->joint_count);
    } else if (light) {
        flux_scene_draw_mesh_lit(frame, cam, world, primitive->mesh, material, light);
    } else {
        flux_scene_draw_mesh(frame, cam, world, primitive->mesh, material);
    }
}

FLUX_SG_API void flux_sg_draw(flux_frame *frame, const flux_camera *cam, const flux_sg_scene *scene,
                              const flux_sg_draw_opts *opts) {
    if (!frame || !cam || !scene || !opts)
        return;

    /* glTF alpha semantics require all depth-writing OPAQUE/MASK draws before
     * depth-reading, non-writing BLEND draws. Source order remains stable
     * within each phase for layered avatar materials. */
    for (int blend_phase = 0; blend_phase < 2; ++blend_phase) {
        for (uint32_t ni = 0; ni < scene->node_count; ++ni) {
            const flux_sg_node *n = &scene->nodes[ni];
            if (n->mesh_first < 0 || n->mesh_prim_count <= 0)
                continue;
            for (int k = n->mesh_first; k < n->mesh_first + n->mesh_prim_count; ++k) {
                if ((uint32_t)k >= scene->prim_count)
                    break;
                const flux_sg_primitive *p = &scene->prims[k];
                flux_material *material = primitive_material(scene, p, opts->material);
                bool blend = material && flux_material_get_alpha_mode(material) ==
                                             FLUX_MATERIAL_ALPHA_BLEND;
                if (!material || blend != (blend_phase != 0))
                    continue;
                const flux_sg_skin *skin =
                    n->skin >= 0 && (uint32_t)n->skin < scene->skin_count
                        ? &scene->skins[n->skin]
                        : NULL;
                draw_primitive(frame, cam, n->world, p, material, opts->light, skin);
            }
        }

        /* A scene with no nodes (mesh-only file) still has primitives. */
        if (scene->node_count == 0) {
            flux_mat4 identity = flux_mat4_identity();
            for (uint32_t k = 0; k < scene->prim_count; ++k) {
                const flux_sg_primitive *p = &scene->prims[k];
                flux_material *material = primitive_material(scene, p, opts->material);
                bool blend = material && flux_material_get_alpha_mode(material) ==
                                             FLUX_MATERIAL_ALPHA_BLEND;
                if (material && blend == (blend_phase != 0))
                    draw_primitive(frame, cam, identity, p, material, opts->light, NULL);
            }
        }
    }
}
