/*
 * Scene-graph animation integration test.
 *
 * Builds a minimal VRM target and VRMC_vrm_animation clip in memory. This
 * exercises the real GLB/accessor parser, humanoid channel binding, rest-pose
 * retargeting, linear quaternion sampling, hips translation, looping, and
 * animation-handle ownership without checking large binary fixtures into git.
 */
#include "test_helpers.h"

#include <flux-scene-graph/scene-graph.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_glb {
    uint8_t *bytes;
    size_t size;
} test_glb;

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
        return (test_glb){0};

    size_t total = 12u + 8u + json_padded + 8u + bin_padded;
    uint8_t *bytes = calloc(1, total);
    if (!bytes)
        return (test_glb){0};

    put_u32_le(bytes, 0x46546c67u);      /* glTF */
    put_u32_le(bytes + 4u, 2u);          /* GLB 2 */
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
        0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 0.70710678f, 0.70710678f,
    };
    return make_glb(json, samples, sizeof(samples));
}

int main(void) {
    flux_device *device = test_helpers_make_headless_device();
    if (!device) {
        fprintf(stderr, "test_scene_graph_animation: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    test_glb target_glb = make_target();
    test_glb animation_glb = make_animation();
    EXPECT(target_glb.bytes != NULL);
    EXPECT(animation_glb.bytes != NULL);
    if (!target_glb.bytes || !animation_glb.bytes) {
        free(target_glb.bytes);
        free(animation_glb.bytes);
        flux_device_release(device);
        TEST_SUMMARY();
    }

    flux_sg_scene *scene = NULL;
    flux_result load_result =
        flux_sg_load_glb(device, target_glb.bytes, target_glb.size, &scene);
    if (load_result != FLUX_OK)
        fprintf(stderr, "target GLB load failed: %s\n", flux_result_string(load_result));
    EXPECT(load_result == FLUX_OK);
    if (scene) {
        flux_material_desc material_desc = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = FLUX_FORMAT_RGBA8_UNORM,
        };
        flux_material *material = NULL;
        EXPECT(flux_material_create(device, &material_desc, &material) == FLUX_OK);
        flux_material *table[1] = {material};
        EXPECT(flux_sg_scene_set_materials(scene, table, 1, material) == FLUX_OK);
        /* The scene retains both table and fallback references. */
        flux_material_release(material);
        EXPECT(flux_sg_scene_set_materials(scene, NULL, 0, NULL) == FLUX_OK);
        EXPECT(flux_sg_scene_set_materials(NULL, NULL, 0, NULL) ==
               FLUX_ERROR_INVALID_ARGUMENT);
    }
    flux_sg_animation *animation = NULL;
    if (scene)
        EXPECT(flux_sg_load_animation_glb(scene, animation_glb.bytes, animation_glb.size,
                                          &animation) == FLUX_OK);

    if (scene && animation) {
        EXPECT_NEAR(flux_sg_animation_duration(animation), 1.0f, 1e-6);
        EXPECT(flux_sg_animation_channel_count(animation) == 2u);

        flux_vec3 eye = {0};
        EXPECT(flux_sg_scene_humanoid_bone_position(scene, "leftEye", &eye));
        EXPECT_NEAR(eye.x, 1.0f, 1e-5);
        EXPECT_NEAR(eye.y, 2.0f, 1e-5);

        /* Matrix-authored nodes must preserve their column-major rotation
         * during TRS decomposition: +X under +90-degree Z becomes +Y. */
        flux_vec3 jaw = {0};
        EXPECT(flux_sg_scene_humanoid_bone_position(scene, "jaw", &jaw));
        EXPECT_NEAR(jaw.x, 0.0f, 1e-5);
        EXPECT_NEAR(jaw.y, 3.0f, 1e-5);

        EXPECT(flux_sg_scene_apply_animation(scene, animation, 0.5f, false) == FLUX_OK);
        EXPECT(flux_sg_scene_humanoid_bone_position(scene, "leftEye", &eye));
        EXPECT_NEAR(eye.x, 0.70710678f, 1e-4);
        EXPECT_NEAR(eye.y, 3.20710678f, 1e-4);

        EXPECT(flux_sg_scene_apply_animation(scene, animation, 1.0f, false) == FLUX_OK);
        EXPECT(flux_sg_scene_humanoid_bone_position(scene, "leftEye", &eye));
        EXPECT_NEAR(eye.x, 0.0f, 1e-4);
        EXPECT_NEAR(eye.y, 4.0f, 1e-4);

        EXPECT(flux_sg_scene_apply_animation(scene, animation, 1.0f, true) == FLUX_OK);
        EXPECT(flux_sg_scene_humanoid_bone_position(scene, "leftEye", &eye));
        EXPECT_NEAR(eye.x, 1.0f, 1e-5);
        EXPECT_NEAR(eye.y, 2.0f, 1e-5);

        flux_sg_animation *retained = flux_sg_animation_retain(animation);
        flux_sg_animation_release(animation);
        animation = retained;
        EXPECT(flux_sg_scene_apply_animation(scene, animation, -1.0f, false) == FLUX_OK);
    }

    flux_sg_animation_release(animation);
    flux_sg_scene_release(scene);
    free(target_glb.bytes);
    free(animation_glb.bytes);
    flux_device_release(device);
    TEST_SUMMARY();
}
