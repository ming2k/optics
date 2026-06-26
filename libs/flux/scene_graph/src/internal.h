/*
 * Internal types for flux-scene-graph: a minimal JSON value model used to
 * walk the glTF JSON chunk, and the loaded scene representation that owns
 * refcounted flux mesh/material handles.
 */
#ifndef FLUX_SG_INTERNAL_H
#define FLUX_SG_INTERNAL_H

#include <flux-scene-graph/scene-graph.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  JSON value tree                                                   */
/* ------------------------------------------------------------------ */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jkind;

typedef struct jv {
    jkind kind;
    union {
        bool b;
        double num;
        struct {
            char *data;
            size_t len;
        } str; /* len excludes the NUL */
        struct {
            struct jv **items;
            size_t count;
        } arr;
        struct {
            char **keys;
            struct jv **vals;
            size_t count;
        } obj;
    };
} jv;

/* Parse `len` bytes of UTF-8 JSON into a tree. Returns NULL on syntax
 * error (the byte offset of the failure is written to *err_off when
 * non-NULL). The tree is heap-owned; free with jv_free. */
jv *jv_parse(const char *json, size_t len, size_t *err_off);

/* Recursively free a tree returned by jv_parse. NULL-safe. */
void jv_free(jv *v);

/* Accessors. Object lookup is linear (glTF objects are small). Return NULL
 * when absent or when `v` is the wrong kind. */
const jv *jv_obj_get(const jv *v, const char *key);
const jv *jv_arr_at(const jv *v, size_t i);
double jv_num(const jv *v, double fallback);
bool jv_bool(const jv *v, bool fallback);

/* ------------------------------------------------------------------ */
/*  Loaded scene                                                      */
/* ------------------------------------------------------------------ */

/* One renderable primitive: a mesh built on the device plus the base colour
 * parsed from the glTF material (linear RGBA, default white). The scene does
 * not own a flux_material — the application supplies one at draw time, because
 * material creation needs the render-target formats, which only the host
 * knows (see flux_material_desc.color_format / depth_format). `world` is
 * resolved at draw time from the owning node's chain. */
typedef struct flux_sg_primitive {
    flux_mesh *mesh;
    flux_vec4 base_color; /* linear; parsed from pbrMetallicRoughness */
    flux_vec3 aabb_min;   /* local-space AABB of the mesh vertices,  */
    flux_vec3 aabb_max;   /* used by flux_sg_scene_bounds for framing */
} flux_sg_primitive;

/* A glTF node. `parent` is -1 for roots. `mesh_first` / `mesh_prim_count`
 * index into the scene's flat primitive list (a glTF mesh may hold several
 * primitives, all flattened together). */
typedef struct flux_sg_node {
    int parent;
    flux_mat4 local;
    flux_mat4 world; /* cached; recomputed each draw */
    int mesh_first;  /* -1 if this node has no mesh */
    int mesh_prim_count;
} flux_sg_node;

struct flux_sg_scene {
    int32_t refcount;
    flux_sg_primitive *prims; /* flat list across all meshes */
    uint32_t prim_count;
    flux_sg_node *nodes;
    uint32_t node_count;
    int *roots; /* node indices */
    uint32_t root_count;
};

/* Parse the .glb container + glTF JSON and populate `*sc` (which must be
 * pre-allocated and zeroed). Returns FLUX_OK if at least one primitive was
 * built; otherwise a flux_result code. */
flux_result sg_parse_glb(flux_device *dev, const void *bytes, size_t len, flux_sg_scene *sc);

#endif /* FLUX_SG_INTERNAL_H */
