/*
 * Scene-graph animation integration test.
 *
 * Builds a minimal VRM target and VRMC_vrm_animation clip in memory. This
 * exercises the real GLB/accessor parser, humanoid channel binding, rest-pose
 * retargeting, linear quaternion sampling, hips translation, looping, and
 * animation-handle ownership without checking large binary fixtures into git.
 */
#include "gfx_test.h"
#include "optics_test.h"

#include <optics/scene.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_glb {
    uint8_t *bytes;
    size_t size;
} test_glb;

static optics_gfx_result load_glb(optics_gfx_device *device, const void *bytes, size_t size,
                                  optics_scene **out) {
    return optics_scene_load_glb(
        device, &(optics_scene_load_glb_opts){.bytes = bytes, .byte_count = size, .out = out});
}

static optics_gfx_result set_materials(optics_scene *scene, optics_gfx_material *const *materials,
                                       uint32_t count, optics_gfx_material *fallback) {
    return optics_scene_set_materials(
        scene, &(optics_scene_set_materials_opts){
                   .materials = materials, .material_count = count, .fallback = fallback});
}

static optics_gfx_result load_animation_glb(const optics_scene *target, const void *bytes,
                                            size_t size, optics_scene_animation **out) {
    return optics_scene_load_animation_glb(
        target,
        &(optics_scene_load_animation_glb_opts){.bytes = bytes, .byte_count = size, .out = out});
}

static bool bone_position(const optics_scene *scene, const char *name, optics_gfx_vec3 *out) {
    return optics_scene_humanoid_bone_position(
        scene, &(optics_scene_humanoid_bone_position_opts){.bone_name = name, .out_position = out});
}

static optics_gfx_result apply_animation(optics_scene *scene,
                                         const optics_scene_animation *animation, float time,
                                         bool loop) {
    return optics_scene_apply_animation(
        scene, &(optics_scene_apply_animation_opts){
                   .animation = animation, .time_seconds = time, .loop = loop});
}

static bool scene_bounds(const optics_scene *scene, optics_gfx_vec3 *out_min,
                         optics_gfx_vec3 *out_max) {
    return optics_scene_bounds(scene,
                               &(optics_scene_bounds_opts){.out_min = out_min, .out_max = out_max});
}

static void put_u32_le(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static test_glb make_glb(const char *json, const void *bin, size_t bin_size) {
    size_t json_size = strlen(json);
    size_t json_padded = (json_size + 3u) & ~(size_t)3u;
    size_t bin_padded = (bin_size + 3u) & ~(size_t)3u;
    if (json_padded > UINT32_MAX || bin_padded > UINT32_MAX ||
        json_padded > SIZE_MAX - bin_padded - 28u || json_padded + bin_padded + 28u > UINT32_MAX)
        return (test_glb){};

    size_t total = 12u + 8u + json_padded + 8u + bin_padded;
    uint8_t *bytes = calloc(1, total);
    if (!bytes)
        return (test_glb){};

    put_u32_le(bytes, 0x46546c67u); /* glTF */
    put_u32_le(bytes + 4u, 2u);     /* GLB 2 */
    put_u32_le(bytes + 8u, (uint32_t)total);
    put_u32_le(bytes + 12u, (uint32_t)json_padded);
    put_u32_le(bytes + 16u, 0x4e4f534au); /* JSON */
    memcpy(bytes + 20u, json, json_size);
    memset(bytes + 20u + json_size, ' ', json_padded - json_size);

    size_t bin_header = 20u + json_padded;
    put_u32_le(bytes + bin_header, (uint32_t)bin_padded);
    put_u32_le(bytes + bin_header + 4u, 0x004e4942u); /* BIN\0 */
    memcpy(bytes + bin_header + 8u, bin, bin_size);
    return (test_glb){.bytes = bytes, .size = total};
}

static test_glb make_target(void) {
    static const char json[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"extensions\":{\"VRMC_vrm\":{\"humanoid\":{\"humanBones\":{"
        "\"hips\":{\"node\":0},\"head\":{\"node\":1},\"leftEye\":{\"node\":2},"
        "\"neck\":{\"node\":3},\"jaw\":{\"node\":4}"
        "}}}},"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":["
        "{\"name\":\"hips\",\"translation\":[0,1,0],\"children\":[1]},"
        "{\"name\":\"head\",\"translation\":[0,1,0],\"children\":[2,3],\"mesh\":0},"
        "{\"name\":\"leftEye\",\"translation\":[1,0,0]},"
        "{\"name\":\"neck\",\"matrix\":[0,1,0,0,-1,0,0,0,0,0,1,0,0,0,0,1],"
        "\"children\":[4]},"
        "{\"name\":\"jaw\",\"translation\":[1,0,0]}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    static const float positions[9] = {
        -0.1f, -0.1f, 0.0f, 0.1f, -0.1f, 0.0f, 0.0f, 0.1f, 0.0f,
    };
    return make_glb(json, positions, sizeof(positions));
}

/* A parent and its child listed in reverse order, with the child preceding
 * the parent in the node array (glTF does not require topological order).
 * Two such pairs: chain A (0 -> 1) and chain B (2 -> 3), each with a pure
 * translation so the composed world translation is exact. */
static test_glb make_reversed_chain(void) {
    static const char json[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":36}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0}}]}],"
        "\"nodes\":["
        "{\"name\":\"childA\",\"translation\":[0,2,0],\"mesh\":0},"
        "{\"name\":\"parentA\",\"translation\":[0,1,0],\"children\":[0]},"
        "{\"name\":\"childB\",\"translation\":[0,0,3],\"mesh\":0},"
        "{\"name\":\"parentB\",\"translation\":[4,0,0],\"children\":[2]}],"
        "\"scenes\":[{\"nodes\":[1,3]}],\"scene\":0}";
    static const float positions[9] = {
        -0.1f, -0.1f, 0.0f, 0.1f, -0.1f, 0.0f, 0.0f, 0.1f, 0.0f,
    };
    return make_glb(json, positions, sizeof(positions));
}

static test_glb make_animation(void) {
    static const char json[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"extensions\":{\"VRMC_vrm_animation\":{\"humanoid\":{\"humanBones\":{"
        "\"hips\":{\"node\":0},\"head\":{\"node\":1}"
        "}}}},"
        "\"buffers\":[{\"byteLength\":64}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":32,\"byteLength\":32}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,\"type\":\"VEC4\"}],"
        "\"nodes\":["
        "{\"name\":\"hips\",\"translation\":[0,1,0],\"children\":[1]},"
        "{\"name\":\"head\",\"translation\":[0,1,0]}],"
        "\"animations\":[{"
        "\"samplers\":["
        "{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"},"
        "{\"input\":0,\"output\":2,\"interpolation\":\"LINEAR\"}],"
        "\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":1,\"path\":\"rotation\"}}]"
        "}]}";
    /* times; hips translations; head quaternions (identity -> +90 deg Z). */
    static const float samples[16] = {
        0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f, 2.0f,        0.0f,
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f,
    };
    return make_glb(json, samples, sizeof(samples));
}

int main(void) {
    optics_gfx_device *device = optics_gfx_test_make_headless_device();
    if (!device) {
        fprintf(stderr, "test_scene_graph_animation: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    test_glb target_glb = make_target();
    test_glb animation_glb = make_animation();
    OT_EXPECT(target_glb.bytes != nullptr);
    OT_EXPECT(animation_glb.bytes != nullptr);
    if (!target_glb.bytes || !animation_glb.bytes) {
        free(target_glb.bytes);
        free(animation_glb.bytes);
        optics_gfx_device_release(device);
        return OT_REPORT();
    }

    optics_scene *scene = nullptr;
    optics_gfx_result load_result = load_glb(device, target_glb.bytes, target_glb.size, &scene);
    if (load_result != OPTICS_GFX_OK)
        fprintf(stderr, "target GLB load failed: %s\n", optics_gfx_result_string(load_result));
    OT_EXPECT(load_result == OPTICS_GFX_OK);
    if (scene) {
        optics_gfx_material_desc material_desc = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        };
        optics_gfx_material *material = nullptr;
        OT_EXPECT(optics_gfx_material_create(device, &material_desc, &material) == OPTICS_GFX_OK);
        optics_gfx_material *table[1] = {material};
        OT_EXPECT(set_materials(scene, table, 1, material) == OPTICS_GFX_OK);
        /* The scene retains both table and fallback references. */
        optics_gfx_material_release(material);
        OT_EXPECT(set_materials(scene, nullptr, 0, nullptr) == OPTICS_GFX_OK);
        OT_EXPECT(set_materials(nullptr, nullptr, 0, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }
    optics_scene_animation *animation = nullptr;
    if (scene)
        OT_EXPECT(load_animation_glb(scene, animation_glb.bytes, animation_glb.size, &animation) ==
                  OPTICS_GFX_OK);

    if (scene && animation) {
        OT_EXPECT_NEAR(optics_scene_animation_duration(animation), 1.0f, 1e-6);
        OT_EXPECT(optics_scene_animation_channel_count(animation) == 2u);

        optics_gfx_vec3 eye = {};
        OT_EXPECT(bone_position(scene, "leftEye", &eye));
        OT_EXPECT_NEAR(eye.x, 1.0f, 1e-5);
        OT_EXPECT_NEAR(eye.y, 2.0f, 1e-5);

        /* Matrix-authored nodes must preserve their column-major rotation
         * during TRS decomposition: +X under +90-degree Z becomes +Y. */
        optics_gfx_vec3 jaw = {};
        OT_EXPECT(bone_position(scene, "jaw", &jaw));
        OT_EXPECT_NEAR(jaw.x, 0.0f, 1e-5);
        OT_EXPECT_NEAR(jaw.y, 3.0f, 1e-5);

        OT_EXPECT(apply_animation(scene, animation, 0.5f, false) == OPTICS_GFX_OK);
        OT_EXPECT(bone_position(scene, "leftEye", &eye));
        OT_EXPECT_NEAR(eye.x, 0.70710678f, 1e-4);
        OT_EXPECT_NEAR(eye.y, 3.20710678f, 1e-4);

        OT_EXPECT(apply_animation(scene, animation, 1.0f, false) == OPTICS_GFX_OK);
        OT_EXPECT(bone_position(scene, "leftEye", &eye));
        OT_EXPECT_NEAR(eye.x, 0.0f, 1e-4);
        OT_EXPECT_NEAR(eye.y, 4.0f, 1e-4);

        OT_EXPECT(apply_animation(scene, animation, 1.0f, true) == OPTICS_GFX_OK);
        OT_EXPECT(bone_position(scene, "leftEye", &eye));
        OT_EXPECT_NEAR(eye.x, 1.0f, 1e-5);
        OT_EXPECT_NEAR(eye.y, 2.0f, 1e-5);

        optics_scene_animation *retained = optics_scene_animation_retain(animation);
        optics_scene_animation_release(animation);
        animation = retained;
        OT_EXPECT(apply_animation(scene, animation, -1.0f, false) == OPTICS_GFX_OK);
    }

    /* A node array where children precede their parents must still fold the
     * parent world matrix into each child. The previous world-matrix update
     * relied on array-order convergence; this locks the correct composed
     * transforms regardless of ordering. */
    test_glb reversed_glb = make_reversed_chain();
    OT_EXPECT(reversed_glb.bytes != nullptr);
    if (reversed_glb.bytes) {
        optics_scene *reversed = nullptr;
        optics_gfx_result reversed_result =
            load_glb(device, reversed_glb.bytes, reversed_glb.size, &reversed);
        OT_EXPECT(reversed_result == OPTICS_GFX_OK);
        if (reversed) {
            /* Two children each carry the mesh. Their translated AABBs
             * (positions plus the child's world translation) must fold in
             * both parents, proving the parent world matrix reached each
             * child even though the children precede the parents in the
             * node array. */
            optics_gfx_vec3 bounds_min = {};
            optics_gfx_vec3 bounds_max = {};
            OT_EXPECT(scene_bounds(reversed, &bounds_min, &bounds_max));
            /* Positions span x in [-0.1, 0.1], y in [-0.1, 0.1], z = 0.
             * childA's world is (0, 3, 0); childB's is (4, 0, 3). */
            OT_EXPECT_NEAR(bounds_min.x, -0.1f, 1e-4);
            OT_EXPECT_NEAR(bounds_min.y, -0.1f, 1e-4);
            OT_EXPECT_NEAR(bounds_min.z, 0.0f, 1e-4);
            OT_EXPECT_NEAR(bounds_max.x, 4.1f, 1e-4);
            OT_EXPECT_NEAR(bounds_max.y, 3.1f, 1e-4);
            OT_EXPECT_NEAR(bounds_max.z, 3.0f, 1e-4);
        }
        optics_gfx_device_wait_idle(device);
        optics_scene_release(reversed);
        free(reversed_glb.bytes);
    }

    optics_scene_animation_release(animation);
    /* Drain before releasing the scene: mesh buffers park on the device
     * retire queue and the release contract requires all referencing GPU
     * work to have completed. */
    optics_gfx_device_wait_idle(device);
    optics_scene_release(scene);
    free(target_glb.bytes);
    free(animation_glb.bytes);
    optics_gfx_device_release(device);
    return OT_REPORT();
}
