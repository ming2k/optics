/*
 * .glb (binary glTF 2.0) container parser + glTF JSON walker.
 *
 * Resolves the supported subset (see scene-graph.h) into flux mesh resources
 * and a node tree with world matrices. The JSON is parsed by json.c into a jv
 * tree; this file only walks it. Little-endian is assumed for the binary
 * container and accessor data (the glTF spec mandates both).
 */
#include "internal.h"

#include <flux/math.h>
#include <flux/scene.h>

#include <float.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Little-endian readers                                             */
/* ------------------------------------------------------------------ */

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/*  .glb container                                                    */
/* ------------------------------------------------------------------ */

#define GLB_MAGIC 0x46546C67u /* "glTF" */
#define GLB_VERSION2 2u
#define CHUNK_JSON 0x4E4F534Au /* "JSON" */
#define CHUNK_BIN 0x004E4942u  /* "BIN\0" */

typedef struct {
    const uint8_t *json;
    size_t json_len;
    const uint8_t *bin;
    size_t bin_len;
} glb;

static flux_result glb_parse(const void *bytes, size_t len, glb *out) {
    const uint8_t *p = bytes;
    if (len < 12)
        return FLUX_ERROR_INVALID_ARGUMENT;
    uint32_t magic = rd_u32(p);
    uint32_t version = rd_u32(p + 4);
    uint32_t total = rd_u32(p + 8);
    if (magic != GLB_MAGIC || version != GLB_VERSION2 || total > len)
        return FLUX_ERROR_INVALID_ARGUMENT;

    const uint8_t *q = p + 12;
    const uint8_t *end = p + total;
    memset(out, 0, sizeof(*out));
    while (q + 8 <= end) {
        uint32_t clen = rd_u32(q);
        uint32_t ctype = rd_u32(q + 4);
        if (q + 8 + clen > end)
            return FLUX_ERROR_INVALID_ARGUMENT;
        if (ctype == CHUNK_JSON && !out->json) {
            out->json = q + 8;
            out->json_len = clen;
        } else if (ctype == CHUNK_BIN && !out->bin) {
            out->bin = q + 8;
            out->bin_len = clen;
        }
        q += 8 + clen;
    }
    if (!out->json)
        return FLUX_ERROR_INVALID_ARGUMENT;
    return FLUX_OK;
}

/* ------------------------------------------------------------------ */
/*  Accessor resolution                                               */
/* ------------------------------------------------------------------ */

#define CT_BYTE 5120
#define CT_UBYTE 5121
#define CT_SHORT 5122
#define CT_USHORT 5123
#define CT_UINT 5125
#define CT_FLOAT 5126

/* Resolve accessor `idx` to a raw pointer into `bin`, its element component
 * type, its component count per element (1..4), its element count, and the
 * byte stride between consecutive elements (0 == tight). Returns NULL if the
 * accessor is outside the supported subset. */
static const uint8_t *accessor_data(const jv *root, int idx, const uint8_t *bin, size_t bin_len,
                                    const uint8_t **base, size_t base_len, int *comp, int *comps,
                                    size_t *count, size_t *stride) {
    const jv *accs = jv_obj_get(root, "accessors");
    const jv *bvws = jv_obj_get(root, "bufferViews");
    const jv *acc = jv_arr_at(accs, (size_t)idx);
    if (!acc)
        return NULL;

    int bv_idx = (int)jv_num(jv_obj_get(acc, "bufferView"), -1);
    const jv *bv = jv_arr_at(bvws, (size_t)bv_idx);
    if (!bv)
        return NULL;

    /* buffer must be the embedded BIN chunk (index 0) when present. If a
     * bufferView names another buffer, this loader cannot resolve it. */
    int buf = (int)jv_num(jv_obj_get(bv, "buffer"), 0);
    size_t bv_off = (size_t)jv_num(jv_obj_get(bv, "byteOffset"), 0);
    size_t bv_stride = (size_t)jv_num(jv_obj_get(bv, "byteStride"), 0);
    size_t a_off = (size_t)jv_num(jv_obj_get(acc, "byteOffset"), 0);

    const uint8_t *seg = NULL;
    size_t seglen = 0;
    if (buf == 0 && bin) {
        seg = bin;
        seglen = bin_len;
    } else if (base) {
        seg = *base;
        seglen = base_len;
    }
    if (!seg)
        return NULL;
    if (bv_off + a_off > seglen)
        return NULL;

    *comp = (int)jv_num(jv_obj_get(acc, "componentType"), 0);
    *count = (size_t)jv_num(jv_obj_get(acc, "count"), 0);

    const char *type = NULL;
    const jv *tv = jv_obj_get(acc, "type");
    if (tv && tv->kind == J_STR)
        type = tv->str.data;
    if (type && strcmp(type, "SCALAR") == 0)
        *comps = 1;
    else if (type && strcmp(type, "VEC2") == 0)
        *comps = 2;
    else if (type && strcmp(type, "VEC3") == 0)
        *comps = 3;
    else if (type && strcmp(type, "VEC4") == 0)
        *comps = 4;
    else
        *comps = 0;
    if (*comps == 0 || *count == 0)
        return NULL;

    if (bv_stride == 0) {
        /* Tight packing: stride is the element size. */
        size_t csize = (*comp == CT_FLOAT) ? 4 : (*comp == CT_USHORT ? 2 : 4);
        bv_stride = csize * (size_t)*comps;
    }
    *stride = bv_stride;
    return seg + bv_off + a_off;
}

static size_t comp_size(int comp) {
    switch (comp) {
    case CT_BYTE:
    case CT_UBYTE:
        return 1;
    case CT_SHORT:
    case CT_USHORT:
        return 2;
    default:
        return 4; /* CT_UINT, CT_FLOAT */
    }
}

/* Read one accessor element as up to 4 floats (normalising integer types to
 * float in [0,1] for normalised colour/UV channels — glTF normalised is not
 * yet honoured; integers are converted directly). */
static void read_floats(const uint8_t *ptr, int comp, int comps, float *out) {
    for (int i = 0; i < comps; ++i) {
        const uint8_t *e = ptr + (size_t)i * comp_size(comp);
        switch (comp) {
        case CT_FLOAT:
            memcpy(&out[i], e, 4);
            break;
        case CT_USHORT:
            out[i] = (float)*(const uint16_t *)e;
            break;
        case CT_UINT:
            out[i] = (float)*(const uint32_t *)e;
            break;
        case CT_UBYTE:
            out[i] = (float)*e;
            break;
        case CT_BYTE:
            out[i] = (float)*(const int8_t *)e;
            break;
        case CT_SHORT:
            out[i] = (float)*(const int16_t *)e;
            break;
        default:
            out[i] = 0.0f;
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Primitive build                                                   */
/* ------------------------------------------------------------------ */

static const jv *attr(const jv *prim, const char *name) {
    return jv_obj_get(jv_obj_get(prim, "attributes"), name);
}

/* Build one primitive's mesh on the device. Returns true on success. */
static bool build_primitive(flux_device *dev, const jv *root, const jv *prim, const uint8_t *bin,
                            size_t bin_len, flux_sg_primitive *out_prim) {
    int pos_acc = (int)jv_num(attr(prim, "POSITION"), -1);
    int nrm_acc = (int)jv_num(attr(prim, "NORMAL"), -1);
    int uv_acc = (int)jv_num(attr(prim, "TEXCOORD_0"), -1);

    int pcomp = 0, pcomps = 0;
    size_t pcount = 0, pstride = 0;
    const uint8_t *pos_ptr = pos_acc >= 0 ? accessor_data(root, pos_acc, bin, bin_len, NULL, 0,
                                                          &pcomp, &pcomps, &pcount, &pstride)
                                          : NULL;
    if (!pos_ptr || pcomp != CT_FLOAT || pcomps != 3 || pcount == 0)
        return false;

    int ncomp = 0, ncomps = 0;
    size_t ncount = 0, nstride = 0;
    const uint8_t *nrm_ptr = (nrm_acc >= 0) ? accessor_data(root, nrm_acc, bin, bin_len, NULL, 0,
                                                            &ncomp, &ncomps, &ncount, &nstride)
                                            : NULL;
    bool have_n = nrm_ptr && ncomp == CT_FLOAT && ncomps == 3 && ncount == pcount;

    int ucomp = 0, ucomps = 0;
    size_t ucount = 0, ustride = 0;
    const uint8_t *uv_ptr = (uv_acc >= 0) ? accessor_data(root, uv_acc, bin, bin_len, NULL, 0,
                                                          &ucomp, &ucomps, &ucount, &ustride)
                                          : NULL;
    bool have_uv = uv_ptr && ucomp == CT_FLOAT && ucomps == 2 && ucount == pcount;

    flux_vertex *verts = malloc(pcount * sizeof(flux_vertex));
    if (!verts)
        return false;
    flux_vec3 aabb_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    flux_vec3 aabb_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t i = 0; i < pcount; ++i) {
        const uint8_t *pp = pos_ptr + i * pstride;
        float p[3] = {0, 0, 0};
        read_floats(pp, CT_FLOAT, 3, p);
        verts[i].position[0] = p[0];
        verts[i].position[1] = p[1];
        verts[i].position[2] = p[2];
        if (p[0] < aabb_min.x)
            aabb_min.x = p[0];
        if (p[1] < aabb_min.y)
            aabb_min.y = p[1];
        if (p[2] < aabb_min.z)
            aabb_min.z = p[2];
        if (p[0] > aabb_max.x)
            aabb_max.x = p[0];
        if (p[1] > aabb_max.y)
            aabb_max.y = p[1];
        if (p[2] > aabb_max.z)
            aabb_max.z = p[2];

        if (have_n) {
            float n[3] = {0, 0, 0};
            read_floats(nrm_ptr + i * nstride, CT_FLOAT, 3, n);
            verts[i].normal[0] = n[0];
            verts[i].normal[1] = n[1];
            verts[i].normal[2] = n[2];
        } else {
            verts[i].normal[0] = verts[i].normal[1] = verts[i].normal[2] = 0.0f;
        }
        if (have_uv) {
            float uv[2] = {0, 0};
            read_floats(uv_ptr + i * ustride, CT_FLOAT, 2, uv);
            verts[i].uv[0] = uv[0];
            verts[i].uv[1] = uv[1];
        } else {
            verts[i].uv[0] = verts[i].uv[1] = 0.0f;
        }
    }

    /* Indices (optional). Generated sequentially when absent. */
    uint32_t *idx = NULL;
    size_t idx_count = 0;
    int idx_acc = (int)jv_num(jv_obj_get(prim, "indices"), -1);
    if (idx_acc >= 0) {
        int icomp = 0, icomps = 0;
        size_t icount = 0, istride = 0;
        const uint8_t *ip =
            accessor_data(root, idx_acc, bin, bin_len, NULL, 0, &icomp, &icomps, &icount, &istride);
        if (!ip || icomps != 1 || icount == 0) {
            free(verts);
            return false;
        }
        idx = malloc(icount * sizeof(uint32_t));
        if (!idx) {
            free(verts);
            return false;
        }
        for (size_t i = 0; i < icount; ++i) {
            float v = 0;
            read_floats(ip + i * istride, icomp, 1, &v);
            idx[i] = (uint32_t)v;
        }
        idx_count = icount;
    } else {
        idx_count = pcount;
        idx = malloc(idx_count * sizeof(uint32_t));
        if (!idx) {
            free(verts);
            return false;
        }
        for (size_t i = 0; i < idx_count; ++i)
            idx[i] = (uint32_t)i;
    }

    flux_mesh_desc md = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = (uint32_t)pcount,
        .indices = idx,
        .index_count = (uint32_t)idx_count,
    };
    flux_mesh *mesh = NULL;
    flux_result r = flux_mesh_create(dev, &md, &mesh);
    free(verts);
    free(idx);
    if (r != FLUX_OK)
        return false;

    out_prim->mesh = mesh;
    out_prim->base_color = flux_vec4_make(1.0f, 1.0f, 1.0f, 1.0f);
    out_prim->aabb_min = aabb_min;
    out_prim->aabb_max = aabb_max;

    /* Map the glTF material's base colour, if any. */
    int mat_idx = (int)jv_num(jv_obj_get(prim, "material"), -1);
    const jv *mats = jv_obj_get(root, "materials");
    const jv *mat = jv_arr_at(mats, (size_t)mat_idx);
    const jv *pbr = mat ? jv_obj_get(mat, "pbrMetallicRoughness") : NULL;
    const jv *bcf = pbr ? jv_obj_get(pbr, "baseColorFactor") : NULL;
    if (bcf && bcf->kind == J_ARR && bcf->arr.count >= 4) {
        out_prim->base_color = flux_vec4_make(
            (float)jv_num(jv_arr_at(bcf, 0), 1.0), (float)jv_num(jv_arr_at(bcf, 1), 1.0),
            (float)jv_num(jv_arr_at(bcf, 2), 1.0), (float)jv_num(jv_arr_at(bcf, 3), 1.0));
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Node tree + world matrices                                        */
/* ------------------------------------------------------------------ */

static void read_node_local(const jv *node, flux_sg_node *n) {
    const jv *m = jv_obj_get(node, "matrix");
    if (m && m->kind == J_ARR && m->arr.count >= 16) {
        /* glTF matrix is column-major; flux_mat4.m is column-major. */
        for (int i = 0; i < 16; ++i)
            n->local.m[i] = (float)jv_num(jv_arr_at(m, (size_t)i), 0.0);
        return;
    }
    /* TRS (defaults: identity translation/rotation, unit scale). */
    flux_vec3 t = {0, 0, 0};
    flux_quat q = flux_quat_identity();
    flux_vec3 s = {1, 1, 1};
    const jv *tv = jv_obj_get(node, "translation");
    if (tv && tv->kind == J_ARR && tv->arr.count >= 3) {
        t = flux_vec3_make((float)jv_num(jv_arr_at(tv, 0), 0), (float)jv_num(jv_arr_at(tv, 1), 0),
                           (float)jv_num(jv_arr_at(tv, 2), 0));
    }
    const jv *qv = jv_obj_get(node, "rotation");
    if (qv && qv->kind == J_ARR && qv->arr.count >= 4) {
        q.x = (float)jv_num(jv_arr_at(qv, 0), 0);
        q.y = (float)jv_num(jv_arr_at(qv, 1), 0);
        q.z = (float)jv_num(jv_arr_at(qv, 2), 0);
        q.w = (float)jv_num(jv_arr_at(qv, 3), 1);
    }
    const jv *sv = jv_obj_get(node, "scale");
    if (sv && sv->kind == J_ARR && sv->arr.count >= 3) {
        s = flux_vec3_make((float)jv_num(jv_arr_at(sv, 0), 1), (float)jv_num(jv_arr_at(sv, 1), 1),
                           (float)jv_num(jv_arr_at(sv, 2), 1));
    }
    flux_mat4 T = flux_mat4_translate(t.x, t.y, t.z);
    flux_mat4 R = flux_mat4_rotation_quat(q);
    flux_mat4 S = flux_mat4_scale(s.x, s.y, s.z);
    n->local = flux_mat4_multiply(T, flux_mat4_multiply(R, S));
}

/* ------------------------------------------------------------------ */
/*  Public: parse .glb into a scene                                   */
/* ------------------------------------------------------------------ */

flux_result sg_parse_glb(flux_device *dev, const void *bytes, size_t len, flux_sg_scene *sc) {
    glb g;
    flux_result r = glb_parse(bytes, len, &g);
    if (r != FLUX_OK)
        return r;

    size_t err = 0;
    jv *root = jv_parse((const char *)g.json, g.json_len, &err);
    if (!root || root->kind != J_OBJ) {
        jv_free(root);
        return FLUX_ERROR_INVALID_ARGUMENT;
    }

    const jv *meshes = jv_obj_get(root, "meshes");
    const jv *nodes = jv_obj_get(root, "nodes");
    uint32_t mesh_count = (meshes && meshes->kind == J_ARR) ? (uint32_t)meshes->arr.count : 0;
    uint32_t node_count = (nodes && nodes->kind == J_ARR) ? (uint32_t)nodes->arr.count : 0;

    /* First pass: build all primitives, record each mesh's primitive span. */
    int *mesh_prim_start = mesh_count ? calloc(mesh_count + 1, sizeof(int)) : NULL;
    int *mesh_prim_count = mesh_count ? calloc(mesh_count, sizeof(int)) : NULL;
    if ((mesh_count && (!mesh_prim_start || !mesh_prim_count))) {
        free(mesh_prim_start);
        free(mesh_prim_count);
        jv_free(root);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    size_t prim_cap = 16, prim_n = 0;
    flux_sg_primitive *prims = malloc(prim_cap * sizeof(*prims));
    if (!prims) {
        free(mesh_prim_start);
        free(mesh_prim_count);
        jv_free(root);
        return FLUX_ERROR_OUT_OF_MEMORY;
    }

    /* Declared here so the `goto oom` path below sees them initialised. */
    int *roots = NULL;
    uint32_t root_n = 0;
    flux_sg_node *narr = NULL;

    for (uint32_t mi = 0; mi < mesh_count; ++mi) {
        const jv *mesh = jv_arr_at(meshes, mi);
        const jv *prims_arr = jv_obj_get(mesh, "primitives");
        uint32_t pc = (prims_arr && prims_arr->kind == J_ARR) ? (uint32_t)prims_arr->arr.count : 0;
        mesh_prim_start[mi] = (int)prim_n;
        mesh_prim_count[mi] = (int)pc;
        for (uint32_t pi = 0; pi < pc; ++pi) {
            if (prim_n == prim_cap) {
                prim_cap *= 2;
                flux_sg_primitive *np = realloc(prims, prim_cap * sizeof(*prims));
                if (!np)
                    goto oom;
                prims = np;
            }
            if (!build_primitive(dev, root, jv_arr_at(prims_arr, pi), g.bin, g.bin_len,
                                 &prims[prim_n])) {
                continue; /* unsupported primitive — skip, not fatal */
            }
            prim_n++;
        }
    }

    if (prim_n == 0) {
        free(prims);
        free(mesh_prim_start);
        free(mesh_prim_count);
        jv_free(root);
        return FLUX_ERROR_UNSUPPORTED;
    }

    /* Nodes. */
    if (node_count) {
        narr = calloc(node_count, sizeof(*narr));
        if (!narr)
            goto oom;
        for (uint32_t ni = 0; ni < node_count; ++ni) {
            flux_sg_node *n = &narr[ni];
            n->parent = -1;
            n->local = flux_mat4_identity();
            n->world = flux_mat4_identity();
            n->mesh_first = -1;
            n->mesh_prim_count = 0;
            const jv *node = jv_arr_at(nodes, ni);
            read_node_local(node, n);
            int mi = (int)jv_num(jv_obj_get(node, "mesh"), -1);
            if (mi >= 0 && (uint32_t)mi < mesh_count) {
                n->mesh_first = mesh_prim_start[mi];
                n->mesh_prim_count = mesh_prim_count[mi];
            }
        }
        /* Resolve parent links from `children`. */
        for (uint32_t ni = 0; ni < node_count; ++ni) {
            const jv *node = jv_arr_at(nodes, ni);
            const jv *ch = jv_obj_get(node, "children");
            if (!ch || ch->kind != J_ARR)
                continue;
            for (size_t c = 0; c < ch->arr.count; ++c) {
                int ci = (int)jv_num(jv_arr_at(ch, c), -1);
                if (ci >= 0 && (uint32_t)ci < node_count)
                    narr[ci].parent = (int)ni;
            }
        }
    }

    /* Roots: the default scene's nodes, else every parentless node. */
    int scene_idx = (int)jv_num(jv_obj_get(root, "scene"), -1);
    const jv *scenes = jv_obj_get(root, "scenes");
    const jv *scene = jv_arr_at(scenes, (size_t)scene_idx);
    const jv *scene_nodes = scene ? jv_obj_get(scene, "nodes") : NULL;
    if (scene_nodes && scene_nodes->kind == J_ARR) {
        root_n = (uint32_t)scene_nodes->arr.count;
        roots = root_n ? malloc(root_n * sizeof(int)) : NULL;
        for (uint32_t i = 0; i < root_n; ++i)
            roots[i] = (int)jv_num(jv_arr_at(scene_nodes, i), -1);
    } else if (node_count) {
        roots = malloc(node_count * sizeof(int));
        for (uint32_t ni = 0; ni < node_count; ++ni)
            if (narr[ni].parent < 0 && root_n < node_count)
                roots[root_n++] = (int)ni;
    }

    /* Resolve world matrices: BFS from roots (parents precede children). */
    for (uint32_t i = 0; i < root_n; ++i) {
        if (roots[i] >= 0 && (uint32_t)roots[i] < node_count)
            narr[roots[i]].world = narr[roots[i]].local;
    }
    /* Propagate to children. node_count is small; iterate to fixpoint. */
    for (uint32_t pass = 0; pass < node_count; ++pass) {
        bool changed = false;
        for (uint32_t ni = 0; ni < node_count; ++ni) {
            if (narr[ni].parent < 0)
                continue;
            flux_sg_node *n = &narr[ni];
            flux_sg_node *p = &narr[ni]; /* placeholder; set below */
            int par = n->parent;
            if (par < 0 || (uint32_t)par >= node_count)
                continue;
            flux_mat4 w = flux_mat4_multiply(narr[par].world, n->local);
            /* Simple convergence: recompute every pass; stop when stable. */
            if (memcmp(&w, &n->world, sizeof(flux_mat4)) != 0) {
                n->world = w;
                changed = true;
            }
            (void)p;
        }
        if (!changed)
            break;
    }

    sc->prims = prims;
    sc->prim_count = (uint32_t)prim_n;
    sc->nodes = narr;
    sc->node_count = node_count;
    sc->roots = roots;
    sc->root_count = root_n;
    free(mesh_prim_start);
    free(mesh_prim_count);
    jv_free(root);
    return FLUX_OK;

oom:
    for (size_t i = 0; i < prim_n; ++i)
        if (prims[i].mesh)
            flux_mesh_release(prims[i].mesh);
    free(prims);
    free(narr);
    free(roots);
    free(mesh_prim_start);
    free(mesh_prim_count);
    jv_free(root);
    return FLUX_ERROR_OUT_OF_MEMORY;
}
