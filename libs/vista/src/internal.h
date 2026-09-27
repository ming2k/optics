/*
 * Internal types for vista: a minimal JSON value model used to
 * walk the glTF JSON chunk, and the loaded scene representation that owns
 * refcounted flux mesh/material handles.
 */
#ifndef VISTA_INTERNAL_H
#define VISTA_INTERNAL_H

#include <vista/vista.h>

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

/* VRM humanoid bone-slot count (shared by parse and scene). */
#define SG_HUMAN_BONE_COUNT 55

/* ------------------------------------------------------------------ */
/*  Loaded scene                                                      */
/* ------------------------------------------------------------------ */

/* One renderable primitive: a mesh built on the device plus the base colour
 * parsed from the glTF material (linear RGBA, default white). */
typedef struct vista_primitive {
    flux_mesh *mesh;
    flux_vec4 base_color; /* linear; parsed from pbrMetallicRoughness */
    int material_index;   /* -1 selects the installed fallback */
    flux_vec3 aabb_min;   /* local-space AABB of the mesh vertices */
    flux_vec3 aabb_max;
} vista_primitive;

typedef struct vista_skin {
    int *joints;
    flux_mat4 *inverse_bind;
    flux_mat4 *palette;
    uint32_t joint_count;
} vista_skin;

/* A glTF node. `parent` is -1 for roots. */
typedef struct vista_node {
    int parent;
    flux_vec3 rest_translation;
    flux_quat rest_rotation;
    flux_vec3 rest_scale;
    flux_vec3 translation;
    flux_quat rotation;
    flux_vec3 scale;
    flux_quat rest_world_rotation;
    flux_mat4 world; /* cached; recomputed each draw */
    int mesh_first;  /* -1 if this node has no mesh */
    int mesh_prim_count;
    int skin; /* -1 if this node is not skinned */
    char *name;
} vista_node;

typedef enum vista_animation_path {
    SG_ANIM_TRANSLATION,
    SG_ANIM_ROTATION,
    SG_ANIM_SCALE,
} vista_animation_path;

typedef enum vista_interpolation {
    SG_INTERP_LINEAR,
    SG_INTERP_STEP,
    SG_INTERP_CUBIC,
} vista_interpolation;

typedef struct vista_animation_channel {
    int target_node;
    vista_animation_path path;
    vista_interpolation interpolation;
    float *times;
    float *values;
    uint32_t key_count;
    uint8_t components;
} vista_animation_channel;

struct vista_animation {
    int32_t refcount;
    const vista_scene *target;
    vista_animation_channel *channels;
    uint32_t channel_count;
    float duration;
};

struct vista_scene {
    int32_t refcount;
    vista_primitive *prims; /* flat list across all meshes */
    uint32_t prim_count;
    vista_node *nodes;
    uint32_t node_count;
    vista_skin *skins;
    uint32_t skin_count;
    flux_material **materials;
    uint32_t material_count;
    flux_material *fallback_material;
    int *roots; /* node indices */
    uint32_t root_count;
    int human_bones[SG_HUMAN_BONE_COUNT];
    uint32_t *world_order;
};

/* ------------------------------------------------------------------ */
/*  Parsed scene data (CPU-only, no device)                           */
/* ------------------------------------------------------------------ */

typedef struct sg_primitive_data {
    flux_vertex *vertices;
    uint32_t vertex_count;
    uint32_t *indices; /* NULL = non-indexed */
    uint32_t index_count;
    flux_skin_vertex *skin_vertices; /* NULL = static mesh */
    flux_vec3 aabb_min;
    flux_vec3 aabb_max;
    flux_vec4 base_color;
    int material_index;
} sg_primitive_data;

struct vista_scene_data {
    sg_primitive_data *prims;
    uint32_t prim_count;
    vista_node *nodes;
    uint32_t node_count;
    vista_skin *skins;
    uint32_t skin_count;
    int *roots;
    uint32_t root_count;
    int human_bones[SG_HUMAN_BONE_COUNT];
};

typedef struct vista_scene_data sg_scene_data;

void sg_data_free(sg_scene_data *data);
void sg_data_transfer(sg_scene_data *data, vista_scene *sc);
flux_result sg_parse_glb(const void *bytes, size_t len, sg_scene_data *data);
flux_result sg_data_build(flux_device *dev, sg_scene_data *data, vista_scene *sc);

typedef struct sg_glb {
    const uint8_t *json;
    size_t json_len;
    const uint8_t *bin;
    size_t bin_len;
} sg_glb;

#define SG_CT_BYTE 5120
#define SG_CT_UBYTE 5121
#define SG_CT_SHORT 5122
#define SG_CT_USHORT 5123
#define SG_CT_UINT 5125
#define SG_CT_FLOAT 5126

flux_result sg_glb_parse(const void *bytes, size_t len, sg_glb *out);
const uint8_t *sg_accessor_data(const jv *root, int idx, const uint8_t *bin, size_t bin_len,
                                int *comp, int *comps, size_t *count, size_t *stride,
                                bool *normalized);
void sg_read_floats(const uint8_t *ptr, int comp, int comps, bool normalized, float *out);
void sg_read_node(const jv *node_json, vista_node *node);
void sg_update_worlds(vista_scene *scene);
void sg_update_rest_world_rotations(vista_scene *scene);
void sg_update_skin_palettes(vista_scene *scene);
int sg_human_bone_index(const char *name, bool legacy_vrm0);
void sg_read_humanoid(const jv *root, const char *extension_name, bool legacy_vrm0,
                      int out_bones[SG_HUMAN_BONE_COUNT]);

flux_result sg_parse_animation_glb(const vista_scene *target, const void *bytes, size_t len,
                                   vista_animation **out);

#endif /* VISTA_INTERNAL_H */
