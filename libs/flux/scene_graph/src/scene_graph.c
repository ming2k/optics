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

#define FLUX_SG_VERSION_STRING "0.0.3"

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
    free(scene->prims);
    free(scene->nodes);
    free(scene->roots);
    free(scene);
}

FLUX_SG_API uint32_t flux_sg_scene_primitive_count(const flux_sg_scene *scene) {
    return scene ? scene->prim_count : 0;
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

FLUX_SG_API void flux_sg_draw(flux_frame *frame, const flux_camera *cam, const flux_sg_scene *scene,
                              const flux_sg_draw_opts *opts) {
    if (!frame || !cam || !scene || !opts || !opts->material)
        return;

    for (uint32_t ni = 0; ni < scene->node_count; ++ni) {
        const flux_sg_node *n = &scene->nodes[ni];
        if (n->mesh_first < 0 || n->mesh_prim_count <= 0)
            continue;
        for (int k = n->mesh_first; k < n->mesh_first + n->mesh_prim_count; ++k) {
            const flux_sg_primitive *p = &scene->prims[k];
            if (!p->mesh)
                continue;
            if (opts->light)
                flux_scene_draw_mesh_lit(frame, cam, n->world, p->mesh, opts->material,
                                         opts->light);
            else
                flux_scene_draw_mesh(frame, cam, n->world, p->mesh, opts->material);
        }
    }
    /* A scene with no nodes (mesh-only file) still has primitives; draw them
     * with an identity world so a flat .glb is visible. */
    if (scene->node_count == 0) {
        flux_mat4 id = flux_mat4_identity();
        for (uint32_t k = 0; k < scene->prim_count; ++k) {
            if (!scene->prims[k].mesh)
                continue;
            if (opts->light)
                flux_scene_draw_mesh_lit(frame, cam, id, scene->prims[k].mesh, opts->material,
                                         opts->light);
            else
                flux_scene_draw_mesh(frame, cam, id, scene->prims[k].mesh, opts->material);
        }
    }
}
