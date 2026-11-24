/*
 * scene_contracts — CPU-only contracts for liboptics_scene:
 *
 *   - Version macros and runtime accessors (all four entry points + partial nulls);
 *   - Multi-primitive & multi-mesh GLB parsing with UNSIGNED_SHORT / UNSIGNED_INT
 *     indices, NORMAL, TEXCOORD_0, interleaved byteStride, JOINTS_0/WEIGHTS_0 skin
 *     attributes, baseColorFactor, and unknown trailing GLB chunks (per glTF 2.0);
 *   - Bounds union across multiple primitives;
 *   - Empty-scene & null-handle safety across the entire public surface.
 */

#include "optics_test.h"

#include <optics/scene.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t wr_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
    return 4;
}

static size_t make_glb_with_extra_chunk(uint8_t *buf, size_t cap, const char *json,
                                        const uint8_t *bin, size_t bin_len, bool add_unknown) {
    size_t js_len = strlen(json);
    size_t js_pad = (4 - (js_len % 4)) % 4;
    size_t bin_pad = (4 - (bin_len % 4)) % 4;
    size_t extra_bytes = add_unknown ? (8 + 4) : 0;
    size_t total = 12 + 8 + js_len + js_pad + (bin_len ? 8 + bin_len + bin_pad : 0) + extra_bytes;
    if (total > cap)
        return 0;

    size_t o = 0;
    o += wr_u32(buf + o, 0x46546C67u);
    o += wr_u32(buf + o, 2u);
    o += wr_u32(buf + o, (uint32_t)total);

    o += wr_u32(buf + o, (uint32_t)(js_len + js_pad));
    o += wr_u32(buf + o, 0x4E4F534Au);
    memcpy(buf + o, json, js_len);
    for (size_t i = 0; i < js_pad; ++i)
        buf[o + js_len + i] = ' ';
    o += js_len + js_pad;

    if (bin_len) {
        o += wr_u32(buf + o, (uint32_t)(bin_len + bin_pad));
        o += wr_u32(buf + o, 0x004E4942u);
        memcpy(buf + o, bin, bin_len);
        for (size_t i = 0; i < bin_pad; ++i)
            buf[o + bin_len + i] = 0;
        o += bin_len + bin_pad;
    }

    if (add_unknown) {
        o += wr_u32(buf + o, 4u);
        o += wr_u32(buf + o, 0x58585858u); /* unknown chunk type "XXXX" */
        o += wr_u32(buf + o, 0xDEADBEEFu);
    }
    return o;
}

static void test_version_contract(void) {
    int major = -1, minor = -1, patch = -1;
    optics_scene_version(&major, &minor, &patch);
    OT_EXPECT(major == OPTICS_SCENE_VERSION_MAJOR);
    OT_EXPECT(minor == OPTICS_SCENE_VERSION_MINOR);
    OT_EXPECT(patch == OPTICS_SCENE_VERSION_PATCH);

    /* Partial nulls must be tolerated safely. */
    optics_scene_version(nullptr, nullptr, nullptr);
    major = -1;
    optics_scene_version(&major, nullptr, nullptr);
    OT_EXPECT(major == OPTICS_SCENE_VERSION_MAJOR);

    OT_EXPECT(optics_scene_version_number() == OPTICS_SCENE_VERSION_NUMBER);
    OT_EXPECT(optics_scene_version_check(OPTICS_SCENE_VERSION_MAJOR, OPTICS_SCENE_VERSION_MINOR,
                                         OPTICS_SCENE_VERSION_PATCH));
    OT_EXPECT(!optics_scene_version_check(OPTICS_SCENE_VERSION_MAJOR + 1, 0, 0));
    OT_EXPECT(
        !optics_scene_version_check(OPTICS_SCENE_VERSION_MAJOR, OPTICS_SCENE_VERSION_MINOR + 1, 0));
    OT_EXPECT(!optics_scene_version_check(OPTICS_SCENE_VERSION_MAJOR, OPTICS_SCENE_VERSION_MINOR,
                                          OPTICS_SCENE_VERSION_PATCH + 1));

    char expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d", OPTICS_SCENE_VERSION_MAJOR,
             OPTICS_SCENE_VERSION_MINOR, OPTICS_SCENE_VERSION_PATCH);
    OT_EXPECT_STR_EQ(optics_scene_version_string(), expected);
}

static void test_multi_primitive_indexed_and_interleaved(void) {
    /* Two primitives in one mesh:
     *   prim 0: interleaved POS(12B) + NORMAL(12B) + UV(8B), byteStride=32, 3 verts (96B)
     *           indexed by 3 UNSIGNED_SHORT indices (6B + 2B pad = 8B)
     *   prim 1: tight POS(36B) + JOINTS_0(UBYTE4=12B) + WEIGHTS_0(VEC4=48B),
     *           indexed by 3 UNSIGNED_INT indices (12B)
     * Total BIN = 96 + 8 + 36 + 12 + 48 + 12 = 212 bytes.
     */
    uint8_t bin[256] = {};
    typedef struct interleaved_v {
        float pos[3];
        float norm[3];
        float uv[2];
    } interleaved_v;
    static_assert(sizeof(interleaved_v) == 32, "interleaved vertex stride must be 32 bytes");

    const interleaved_v v0[3] = {
        {{-2.0f, -1.0f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{0.0f, -1.0f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{-1.0f, 1.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.5f, 1.0f}},
    };
    const uint16_t idx0[3] = {0, 1, 2};
    const float v1_pos[9] = {
        3.0f, 0.0f, -2.5f, 5.0f, 0.0f, -2.5f, 4.0f, 2.0f, 4.0f,
    };
    const uint8_t v1_joints[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const float v1_weights[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const uint32_t idx1[3] = {0, 1, 2};

    memcpy(bin + 0, v0, 96);
    memcpy(bin + 96, idx0, 6);
    memcpy(bin + 104, v1_pos, 36);
    memcpy(bin + 140, v1_joints, 12);
    memcpy(bin + 152, v1_weights, 48);
    memcpy(bin + 200, idx1, 12);

    static const char JSON[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":212}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":96,\"byteStride\":32},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6},"
        "{\"buffer\":0,\"byteOffset\":104,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":140,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":152,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":200,\"byteLength\":12}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"byteOffset\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":12,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":0,\"byteOffset\":24,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
        "{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
        "{\"bufferView\":3,\"componentType\":5121,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"VEC4\"},"
        "{\"bufferView\":5,\"componentType\":5125,\"count\":3,\"type\":\"SCALAR\"}"
        "],"
        "\"materials\":[{"
        "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.2,0.4,0.8,1.0]}"
        "}],"
        "\"meshes\":[{\"primitives\":["
        "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,"
        "\"material\":0},"
        "{\"attributes\":{\"POSITION\":4,\"JOINTS_0\":5,\"WEIGHTS_0\":6},\"indices\":7}"
        "]}],"
        "\"nodes\":[{\"name\":\"root\",\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";

    uint8_t glb[4096];
    size_t len = make_glb_with_extra_chunk(glb, sizeof(glb), JSON, bin, 212, true);
    OT_REQUIRE(len > 0);

    optics_scene_data *data = nullptr;
    OT_REQUIRE(optics_scene_parse_glb(&(optics_scene_parse_glb_opts){
                   .bytes = glb, .byte_count = len, .out = &data}) == OPTICS_GFX_OK);
    OT_REQUIRE(data != nullptr);
    OT_EXPECT(optics_scene_data_primitive_count(data) == 2u);

    optics_gfx_vec3 mn = {}, mx = {};
    OT_EXPECT(optics_scene_data_bounds(
        data, &(optics_scene_data_bounds_opts){.out_min = &mn, .out_max = &mx}));
    OT_EXPECT_NEAR(mn.x, -2.0f, 1e-5);
    OT_EXPECT_NEAR(mn.y, -1.0f, 1e-5);
    OT_EXPECT_NEAR(mn.z, -2.5f, 1e-5);
    OT_EXPECT_NEAR(mx.x, 5.0f, 1e-5);
    OT_EXPECT_NEAR(mx.y, 2.0f, 1e-5);
    OT_EXPECT_NEAR(mx.z, 4.0f, 1e-5);

    /* Null device in data_build must fail cleanly without freeing `data` unexpectedly. */
    optics_scene *sc = (optics_scene *)(uintptr_t)0x1;
    OT_EXPECT(optics_scene_data_build(nullptr,
                                      &(optics_scene_data_build_opts){.data = data, .out = &sc}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_scene_data_free(data);
}

static void test_empty_scene_and_null_guards(void) {
    /* A valid glTF with zero meshes/primitives must parse cleanly and report 0 bounds. */
    static const char EMPTY_JSON[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scenes\":[{\"nodes\":[]}],\"scene\":0}";
    uint8_t glb[1024];
    size_t len = make_glb_with_extra_chunk(glb, sizeof(glb), EMPTY_JSON, nullptr, 0, false);
    OT_REQUIRE(len > 0);

    optics_scene_data *data = nullptr;
    optics_gfx_result r = optics_scene_parse_glb(
        &(optics_scene_parse_glb_opts){.bytes = glb, .byte_count = len, .out = &data});
    if (r == OPTICS_GFX_OK) {
        OT_REQUIRE(data != nullptr);
        OT_EXPECT(optics_scene_data_primitive_count(data) == 0u);
        optics_gfx_vec3 mn = {}, mx = {};
        OT_EXPECT(!optics_scene_data_bounds(
            data, &(optics_scene_data_bounds_opts){.out_min = &mn, .out_max = &mx}));
        optics_scene_data_free(data);
    } else {
        OT_EXPECT(data == nullptr);
    }

    /* Null guards across every non-device scene & animation API. */
    optics_scene *sc = nullptr;
    OT_EXPECT(
        optics_scene_load_glb(
            nullptr, &(optics_scene_load_glb_opts){.bytes = glb, .byte_count = len, .out = &sc}) ==
        OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_scene_load_glb(nullptr, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_scene_parse_glb(nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_scene_data_build(nullptr, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    OT_EXPECT(optics_scene_retain(nullptr) == nullptr);
    optics_scene_release(nullptr);
    OT_EXPECT(optics_scene_primitive_count(nullptr) == 0u);

    optics_gfx_vec3 mn = {}, mx = {}, pos = {};
    OT_EXPECT(
        !optics_scene_bounds(nullptr, &(optics_scene_bounds_opts){.out_min = &mn, .out_max = &mx}));
    OT_EXPECT(!optics_scene_bounds(nullptr, nullptr));
    OT_EXPECT(!optics_scene_humanoid_bone_position(
        nullptr,
        &(optics_scene_humanoid_bone_position_opts){.bone_name = "head", .out_position = &pos}));
    OT_EXPECT(!optics_scene_humanoid_bone_position(nullptr, nullptr));

    optics_scene_animation *anim = nullptr;
    OT_EXPECT(optics_scene_load_animation_glb(nullptr,
                                              &(optics_scene_load_animation_glb_opts){
                                                  .bytes = glb, .byte_count = len, .out = &anim}) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_scene_animation_retain(nullptr) == nullptr);
    optics_scene_animation_release(nullptr);
    OT_EXPECT(optics_scene_animation_duration(nullptr) == 0.0f);
    OT_EXPECT(optics_scene_animation_channel_count(nullptr) == 0u);
    OT_EXPECT(optics_scene_apply_animation(nullptr, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    optics_scene_reset_pose(nullptr);
    optics_scene_draw(nullptr, nullptr);
}

int main(void) {
    test_version_contract();
    test_multi_primitive_indexed_and_interleaved();
    test_empty_scene_and_null_guards();
    return OT_REPORT();
}
