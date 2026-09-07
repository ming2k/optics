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

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- Version accessors ------------------------------------------------
 * All derived from the FLUX_SG_VERSION_* macros. This file previously
 * hard-coded the string "0.0.29" while the header macros had moved on
 * to 0.0.36 — a stale literal that no test could catch. The string is
 * now built by the macros, and tools/check-version-lockstep.sh keeps
 * every library's macros in lockstep so this class of drift is
 * structurally impossible. */

#define FLUX_SG_STR2(x) #x
#define FLUX_SG_STR(x) FLUX_SG_STR2(x)

void flux_sg_version(int *major, int *minor, int *patch) {
    if (major)
        *major = FLUX_SG_VERSION_MAJOR;
    if (minor)
        *minor = FLUX_SG_VERSION_MINOR;
    if (patch)
        *patch = FLUX_SG_VERSION_PATCH;
}

uint32_t flux_sg_version_number(void) {
    return FLUX_SG_VERSION_NUMBER;
}

bool flux_sg_version_check(int major, int minor, int patch) {
    if (major != FLUX_SG_VERSION_MAJOR)
        return false;
    if (minor > FLUX_SG_VERSION_MINOR)
        return false;
    if (minor == FLUX_SG_VERSION_MINOR && patch > FLUX_SG_VERSION_PATCH)
        return false;
    return true;
}

FLUX_SG_API const char *flux_sg_version_string(void) {
    return FLUX_SG_STR(FLUX_SG_VERSION_MAJOR) "." FLUX_SG_STR(
        FLUX_SG_VERSION_MINOR) "." FLUX_SG_STR(FLUX_SG_VERSION_PATCH);
}

FLUX_SG_API flux_result flux_sg_load_glb(flux_device *device, const void *glb_bytes,
                                         size_t byte_count, flux_sg_scene **out) {
    if (!device || !glb_bytes || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;

    /* Two-stage pipeline: parse (device-independent, testable, fuzzer-
     * reachable) then build (the only stage touching the device). */
    sg_scene_data data;
    memset(&data, 0, sizeof(data));
    flux_result r = sg_parse_glb(glb_bytes, byte_count, &data);
    if (r != FLUX_OK)
        return r; /* sg_parse_glb releases everything it allocated on failure */

    flux_sg_scene *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        sg_data_free(&data);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    sc->refcount = 1;
    r = sg_data_build(device, &data, sc);
    /* sg_data_build consumed `data` either way; on failure it released
     * whatever it uploaded and left `sc` safe to free. */
    if (r != FLUX_OK) {
        flux_sg_scene_release(sc);
        return r;
    }
    *out = sc;
    return FLUX_OK;
}

/* ---- Parsed-data lifecycle (the parse/build seam, ADR-0016) -------- */

/* ---- Parsed-data lifecycle (the parse/build seam, ADR-0016) -------- */

/* The internal header completes the same struct tag the public header
 * declares (classic C opaque pattern), so the public handle and the
 * internal storage are one type — no downcasts, no layout drift. */

FLUX_SG_API flux_result flux_sg_parse_glb(const void *glb_bytes, size_t byte_count,
                                          flux_sg_scene_data **out) {
    if (!glb_bytes || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    sg_scene_data *data = calloc(1, sizeof(*data));
    if (!data)
        return FLUX_ERROR_OUT_OF_MEMORY;
    flux_result r = sg_parse_glb(glb_bytes, byte_count, data);
    if (r != FLUX_OK) {
        /* sg_parse_glb releases everything it allocated on failure. */
        free(data);
        return r;
    }
    *out = (flux_sg_scene_data *)(void *)data;
    return FLUX_OK;
}

void sg_data_free(sg_scene_data *data) {
    if (!data)
        return;
    for (uint32_t i = 0; i < data->prim_count; ++i) {
        free(data->prims[i].vertices);
        free(data->prims[i].indices);
        free(data->prims[i].skin_vertices);
    }
    free(data->prims);
    if (data->nodes)
        for (uint32_t i = 0; i < data->node_count; ++i)
            free(data->nodes[i].name);
    free(data->nodes);
    if (data->skins)
        for (uint32_t i = 0; i < data->skin_count; ++i) {
            free(data->skins[i].joints);
            free(data->skins[i].inverse_bind);
            free(data->skins[i].palette);
        }
    free(data->skins);
    free(data->roots);
    memset(data, 0, sizeof(*data));
}

FLUX_SG_API void flux_sg_scene_data_free(flux_sg_scene_data *data) {
    sg_data_free(data);
    free(data);
}

FLUX_SG_API uint32_t flux_sg_scene_data_primitive_count(const flux_sg_scene_data *data) {
    return data ? data->prim_count : 0;
}

FLUX_SG_API bool flux_sg_scene_data_bounds(const flux_sg_scene_data *data, flux_vec3 *out_min,
                                           flux_vec3 *out_max) {
    if (!out_min || !out_max || !data)
        return false;
    flux_vec3 wmin = {FLT_MAX, FLT_MAX, FLT_MAX};
    flux_vec3 wmax = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    bool any = false;
    for (uint32_t k = 0; k < data->prim_count; ++k) {
        if (data->prims[k].vertices) {
            wmin.x = fminf(wmin.x, data->prims[k].aabb_min.x);
            wmin.y = fminf(wmin.y, data->prims[k].aabb_min.y);
            wmin.z = fminf(wmin.z, data->prims[k].aabb_min.z);
            wmax.x = fmaxf(wmax.x, data->prims[k].aabb_max.x);
            wmax.y = fmaxf(wmax.y, data->prims[k].aabb_max.y);
            wmax.z = fmaxf(wmax.z, data->prims[k].aabb_max.z);
            any = true;
        }
    }
    if (!any)
        return false;
    *out_min = wmin;
    *out_max = wmax;
    return true;
}

void sg_data_transfer(sg_scene_data *data, flux_sg_scene *sc) {
    /* sc->prims is the build stage's business (it owns the upload);
     * this moves only the parse-owned node/skin/root state. */
    sc->nodes = data->nodes;
    data->nodes = NULL;
    sc->node_count = data->node_count;
    data->node_count = 0;
    sc->skins = data->skins;
    data->skins = NULL;
    sc->skin_count = data->skin_count;
    data->skin_count = 0;
    sc->roots = data->roots;
    data->roots = NULL;
    sc->root_count = data->root_count;
    data->root_count = 0;
    for (int i = 0; i < SG_HUMAN_BONE_COUNT; ++i)
        sc->human_bones[i] = data->human_bones[i];
}

flux_result sg_data_build(flux_device *dev, sg_scene_data *data, flux_sg_scene *sc) {
    if (!dev || !data || !sc)
        return FLUX_ERROR_INVALID_ARGUMENT;

    /* Upload every parsed primitive. On failure the already-uploaded
     * meshes are released here; the node/skin transfers below have not
     * happened yet (they are only committed on full success), so the
     * caller's zeroed `data` and `sc` stay consistent for either path. */
    flux_sg_primitive *prims = data->prim_count ? calloc(data->prim_count, sizeof(*prims)) : NULL;
    if (data->prim_count && !prims) {
        sg_data_free(data);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    for (uint32_t i = 0; i < data->prim_count; ++i) {
        const sg_primitive_data *pd = &data->prims[i];
        flux_mesh_skin_desc sd = {
            .type = FLUX_TYPE_MESH_SKIN_DESC,
            .vertices = pd->skin_vertices,
        };
        flux_mesh_desc md = {
            .type = FLUX_TYPE_MESH_DESC,
            .next = pd->skin_vertices ? &sd : NULL,
            .vertices = pd->vertices,
            .vertex_count = pd->vertex_count,
            .indices = pd->indices,
            .index_count = pd->index_count,
        };
        flux_result r = flux_mesh_create(dev, &md, &prims[i].mesh);
        if (r != FLUX_OK) {
            for (uint32_t k = 0; k < i; ++k)
                flux_mesh_release(prims[k].mesh);
            free(prims);
            sg_data_free(data);
            return r;
        }
        prims[i].base_color = pd->base_color;
        prims[i].material_index = pd->material_index;
        prims[i].aabb_min = pd->aabb_min;
        prims[i].aabb_max = pd->aabb_max;
        /* The mesh owns copies on the GPU; the parsed buffers are
         * consumed here, one primitive at a time (the array free below
         * then sees zeroed slots and frees nothing twice). */
        free(pd->vertices);
        free(pd->indices);
        free(pd->skin_vertices);
        data->prims[i] = (sg_primitive_data){0};
    }

    /* All meshes live: transfer everything into the scene and finish the
     * derived CPU state (world matrices, rest rotations, skin palettes).
     * The parsed primitive array itself is consumed here — its buffers
     * were freed per-primitive above, the copies now live in sc->prims. */
    sc->prims = prims;
    sc->prim_count = data->prim_count;
    free(data->prims);
    data->prims = NULL;
    data->prim_count = 0;
    sg_data_transfer(data, sc);
    sg_data_free(data); /* nodes/skins/roots transferred; nothing left */
    sg_update_worlds(sc);
    sg_update_rest_world_rotations(sc);
    sg_update_skin_palettes(sc);
    return FLUX_OK;
}

FLUX_SG_API flux_result flux_sg_scene_data_build(flux_device *device, flux_sg_scene_data *data,
                                                 flux_sg_scene **out) {
    if (!device || !data || !out)
        return FLUX_ERROR_INVALID_ARGUMENT;
    *out = NULL;
    flux_sg_scene *sc = calloc(1, sizeof(*sc));
    if (!sc) {
        sg_data_free(data);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }
    sc->refcount = 1;
    flux_result r = sg_data_build(device, data, sc);
    /* sg_data_build consumed `data` either way; on failure it released
     * whatever it uploaded and left `sc` safe to free. */
    if (r != FLUX_OK) {
        flux_sg_scene_release(sc);
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
    flux_mat4 t =
        flux_mat4_translate(node->translation.x, node->translation.y, node->translation.z);
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
            flux_scene_draw_mesh_skinned_lit(frame, cam, identity, primitive->mesh, material, light,
                                             skin->palette, skin->joint_count);
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
                bool blend =
                    material && flux_material_get_alpha_mode(material) == FLUX_MATERIAL_ALPHA_BLEND;
                if (!material || blend != (blend_phase != 0))
                    continue;
                const flux_sg_skin *skin = n->skin >= 0 && (uint32_t)n->skin < scene->skin_count
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
                bool blend =
                    material && flux_material_get_alpha_mode(material) == FLUX_MATERIAL_ALPHA_BLEND;
                if (material && blend == (blend_phase != 0))
                    draw_primitive(frame, cam, identity, p, material, opts->light, NULL);
            }
        }
    }
}
