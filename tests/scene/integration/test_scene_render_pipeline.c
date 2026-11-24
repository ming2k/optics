/*
 * scene_render_pipeline — full GPU rendering integration test for liboptics_scene.
 *
 * Covers the paths that test_scene_graph_animation alone does not exercise:
 *   1. Two-stage parse/build seam: optics_scene_parse_glb -> optics_scene_data_build;
 *   2. Real GPU rendering via optics_scene_draw into an offscreen surface + depth
 *      buffer with pixel readback;
 *   3. Per-primitive material table (optics_scene_set_materials) vs draw-time
 *      material override (opts.material);
 *   4. STEP, LINEAR, and CUBICSPLINE animation sampling + scale/translation
 *      channels visibly move rendered pixels on screen;
 *   5. optics_scene_reset_pose restores rest-pose bounds AND rendered pixels;
 *   6. GPU-skinned GLB mesh (skins + inverseBindMatrices + JOINTS_0/WEIGHTS_0)
 *      renders and moves under joint animation;
 *   7. optics_scene_retain / optics_scene_release refcount safety.
 */

#include "gfx_test.h"
#include "optics_test.h"

#include <optics/gfx.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>
#include <optics/scene.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define DEPTH_VK VK_FORMAT_D32_SFLOAT
#define DEPTH_FMT OPTICS_GFX_FORMAT_D32_SFLOAT

typedef struct owned_glb {
    uint8_t *bytes;
    size_t size;
} owned_glb;

static void put_u32_le(uint8_t *dst, uint32_t v) {
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8);
    dst[2] = (uint8_t)(v >> 16);
    dst[3] = (uint8_t)(v >> 24);
}

static owned_glb make_glb(const char *json, const void *bin, size_t bin_size) {
    size_t js = strlen(json);
    size_t js_pad = (js + 3u) & ~(size_t)3u;
    size_t bin_pad = (bin_size + 3u) & ~(size_t)3u;
    size_t total = 12u + 8u + js_pad + 8u + bin_pad;
    uint8_t *bytes = calloc(1, total);
    if (!bytes)
        return (owned_glb){};
    put_u32_le(bytes + 0u, 0x46546c67u);
    put_u32_le(bytes + 4u, 2u);
    put_u32_le(bytes + 8u, (uint32_t)total);
    put_u32_le(bytes + 12u, (uint32_t)js_pad);
    put_u32_le(bytes + 16u, 0x4e4f534au);
    memcpy(bytes + 20u, json, js);
    memset(bytes + 20u + js, ' ', js_pad - js);
    size_t bh = 20u + js_pad;
    put_u32_le(bytes + bh, (uint32_t)bin_pad);
    put_u32_le(bytes + bh + 4u, 0x004e4942u);
    memcpy(bytes + bh + 8u, bin, bin_size);
    return (owned_glb){.bytes = bytes, .size = total};
}

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + ((size_t)y * W + (size_t)x) * 4u;
}

static void render_scene(optics_gfx_surface *surface, optics_gfx_target *depth,
                         const optics_gfx_camera *cam, const optics_scene *scene,
                         optics_gfx_material *override_mat, uint8_t *out_px) {
    optics_gfx_frame *frame = nullptr;
    OT_REQUIRE(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
    optics_gfx_frame_prepare_target(frame, depth);

    optics_gfx_pass_attachment color = {
        .load_op = OPTICS_GFX_LOAD_CLEAR,
        .store_op = OPTICS_GFX_STORE_STORE,
        .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    optics_gfx_pass_depth_attachment depth_att = {
        .view = optics_gfx_target_vk_view(depth),
        .format = DEPTH_VK,
        .load_op = OPTICS_GFX_LOAD_CLEAR,
        .store_op = OPTICS_GFX_STORE_DONT_CARE,
        .clear_depth = 1.0f,
    };
    optics_gfx_pass_desc pass = {
        .type = OPTICS_GFX_TYPE_PASS_DESC,
        .color_attachment_count = 1,
        .color_attachments = &color,
        .depth = &depth_att,
    };
    optics_gfx_frame_begin_pass(frame, &pass);
    optics_gfx_frame_set_viewport(frame, &(optics_gfx_frame_set_viewport_opts){.x = 0.0f,
                                                                               .y = 0.0f,
                                                                               .width = (float)W,
                                                                               .height = (float)H,
                                                                               .min_depth = 0.0f,
                                                                               .max_depth = 1.0f});
    optics_gfx_frame_set_scissor(
        frame, &(optics_gfx_frame_set_scissor_opts){.x = 0, .y = 0, .width = W, .height = H});

    optics_gfx_scene_light light = OPTICS_GFX_SCENE_LIGHT_DEFAULT;
    optics_scene_draw(frame, &(optics_scene_draw_opts){
                                 .camera = cam,
                                 .scene = scene,
                                 .material = override_mat,
                                 .light = &light,
                             });

    optics_gfx_frame_end_pass(frame);
    OT_REQUIRE(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_REQUIRE(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    memset(out_px, 0xCD, BYTES);
    OT_REQUIRE(optics_gfx_surface_read_pixels(surface, out_px, BYTES) == OPTICS_GFX_OK);
}

/* Front-facing CCW quad at z=0 centered at origin, half-extent 0.45. */
static owned_glb make_quad_scene_glb(void) {
    static const char JSON[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":120}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
        "\"indices\":2,\"material\":0}]}],"
        "\"nodes\":[{\"name\":\"quad\",\"mesh\":0}],"
        "\"scenes\":[{\"nodes\":[0]}],\"scene\":0"
        "}";
    uint8_t bin[120] = {};
    const float pos[12] = {
        -0.45f, -0.45f, 0.0f, 0.45f, -0.45f, 0.0f, 0.45f, 0.45f, 0.0f, -0.45f, 0.45f, 0.0f,
    };
    const float norm[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    memcpy(bin + 0, pos, 48);
    memcpy(bin + 48, norm, 48);
    memcpy(bin + 96, idx, 24);
    return make_glb(JSON, bin, sizeof(bin));
}

/* Animation targeting node "quad" by name:
 *   - STEP translation from (0,0,0) at t=0 to (+1.1,0,0) at t=1;
 *   - CUBICSPLINE scale from (1,1,1) at t=0 to (2,2,1) at t=1 (zero tangents).
 */
static owned_glb make_step_and_cubic_anim_glb(void) {
    static const char JSON[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":104}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":32,\"byteLength\":72}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"}"
        "],"
        "\"nodes\":[{\"name\":\"quad\"}],"
        "\"animations\":[{"
        "\"samplers\":["
        "{\"input\":0,\"output\":1,\"interpolation\":\"STEP\"},"
        "{\"input\":0,\"output\":2,\"interpolation\":\"CUBICSPLINE\"}"
        "],"
        "\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":0,\"path\":\"scale\"}}"
        "]"
        "}]"
        "}";
    /* times(2), step_trans(2*3), cubic_scale(2*3*3 = inTangent, value, outTangent) */
    const float samples[26] = {
        0.0f, 1.0f,                                           /* times */
        0.0f, 0.0f, 0.0f, 1.1f, 0.0f, 0.0f,                   /* STEP translation */
        0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, /* key 0: scale=1 */
        0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 1.0f, 0.0f, 0.0f, 0.0f, /* key 1: scale=2 */
    };
    return make_glb(JSON, samples, sizeof(samples));
}

/* Skinned quad bound 100% to joint 0 ("bone0"). */
static owned_glb make_skinned_scene_glb(void) {
    static const char JSON[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":264}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":48},"
        "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":16},"
        "{\"buffer\":0,\"byteOffset\":112,\"byteLength\":64},"
        "{\"buffer\":0,\"byteOffset\":176,\"byteLength\":24},"
        "{\"buffer\":0,\"byteOffset\":200,\"byteLength\":64}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":4,\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5121,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":4,\"type\":\"VEC4\"},"
        "{\"bufferView\":4,\"componentType\":5125,\"count\":6,\"type\":\"SCALAR\"},"
        "{\"bufferView\":5,\"componentType\":5126,\"count\":1,\"type\":\"MAT4\"}"
        "],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":0,\"NORMAL\":1,\"JOINTS_0\":2,\"WEIGHTS_0\":3},\"indices\":4}]}],"
        "\"skins\":[{\"joints\":[0],\"inverseBindMatrices\":5}],"
        "\"nodes\":["
        "{\"name\":\"bone0\",\"translation\":[0,0,0]},"
        "{\"name\":\"skinned_mesh\",\"mesh\":0,\"skin\":0}"
        "],"
        "\"scenes\":[{\"nodes\":[0,1]}],\"scene\":0"
        "}";
    uint8_t bin[264] = {};
    const float pos[12] = {
        -0.4f, -0.4f, 0.0f, 0.4f, -0.4f, 0.0f, 0.4f, 0.4f, 0.0f, -0.4f, 0.4f, 0.0f,
    };
    const float norm[12] = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const uint8_t joints[16] = {};
    const float weights[16] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const float ibm[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    memcpy(bin + 0, pos, 48);
    memcpy(bin + 48, norm, 48);
    memcpy(bin + 96, joints, 16);
    memcpy(bin + 112, weights, 64);
    memcpy(bin + 176, idx, 24);
    memcpy(bin + 200, ibm, 64);
    return make_glb(JSON, bin, sizeof(bin));
}

static owned_glb make_bone_shift_anim_glb(void) {
    static const char JSON[] =
        "{"
        "\"asset\":{\"version\":\"2.0\"},"
        "\"buffers\":[{\"byteLength\":32}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}"
        "],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"}"
        "],"
        "\"nodes\":[{\"name\":\"bone0\"}],"
        "\"animations\":[{"
        "\"samplers\":[{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]"
        "}]"
        "}";
    const float samples[8] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.1f, 0.0f, 0.0f};
    return make_glb(JSON, samples, sizeof(samples));
}

int main(void) {
    optics_gfx_device *device = optics_gfx_test_make_headless_device();
    if (!device)
        OT_SKIP("no Vulkan device");

    optics_gfx_surface *surface = nullptr;
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    OT_REQUIRE(optics_gfx_surface_create(device, &sd, &surface) == OPTICS_GFX_OK);

    optics_gfx_target *depth = nullptr;
    OT_REQUIRE(optics_gfx_target_create(device,
                                        &(optics_gfx_target_desc){
                                            .type = OPTICS_GFX_TYPE_TARGET_DESC,
                                            .usage = OPTICS_GFX_TARGET_DEPTH,
                                            .format = DEPTH_FMT,
                                            .width = W,
                                            .height = H,
                                        },
                                        &depth) == OPTICS_GFX_OK);

    optics_gfx_camera cam;
    optics_gfx_camera_perspective(
        &cam, &(optics_gfx_camera_perspective_opts){
                  .fov_y_rad = 1.0f, .aspect = 1.0f, .z_near = 0.1f, .z_far = 50.0f});
    optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0, 0, 4}, (optics_gfx_vec3){0, 0, 0},
                              (optics_gfx_vec3){0, 1, 0});

    optics_gfx_format color_fmt = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(surface));
    optics_gfx_material *red = nullptr;
    optics_gfx_material *blue = nullptr;
    optics_gfx_material_desc md = {
        .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
        .kind = OPTICS_GFX_MATERIAL_UNLIT,
        .base_color = {1.0f, 0.0f, 0.0f, 1.0f},
        .color_format = color_fmt,
        .depth_format = DEPTH_FMT,
    };
    OT_REQUIRE(optics_gfx_material_create(device, &md, &red) == OPTICS_GFX_OK);
    md.base_color = (optics_gfx_vec4){0.0f, 0.0f, 1.0f, 1.0f};
    OT_REQUIRE(optics_gfx_material_create(device, &md, &blue) == OPTICS_GFX_OK);

    static uint8_t px[BYTES];

    /* 1. Two-stage parse -> build seam, material table vs override, STEP/CUBICSPLINE, reset_pose.
     */
    {
        owned_glb quad_glb = make_quad_scene_glb();
        owned_glb anim_glb = make_step_and_cubic_anim_glb();
        OT_REQUIRE(quad_glb.bytes != nullptr && anim_glb.bytes != nullptr);

        optics_scene_data *parsed = nullptr;
        OT_REQUIRE(optics_scene_parse_glb(&(optics_scene_parse_glb_opts){
                       .bytes = quad_glb.bytes, .byte_count = quad_glb.size, .out = &parsed}) ==
                   OPTICS_GFX_OK);
        OT_EXPECT(optics_scene_data_primitive_count(parsed) == 1u);

        optics_scene *scene = nullptr;
        OT_REQUIRE(optics_scene_data_build(
                       device, &(optics_scene_data_build_opts){.data = parsed, .out = &scene}) ==
                   OPTICS_GFX_OK);
        optics_scene_data_free(parsed);
        OT_REQUIRE(scene != nullptr);
        OT_EXPECT(optics_scene_primitive_count(scene) == 1u);

        /* Retain/release contract. */
        OT_EXPECT(optics_scene_retain(scene) == scene);
        optics_scene_release(scene);

        /* Per-primitive material table (red) renders red at center when override is nullptr. */
        optics_gfx_material *table[1] = {red};
        OT_REQUIRE(optics_scene_set_materials(scene, &(optics_scene_set_materials_opts){
                                                         .materials = table,
                                                         .material_count = 1,
                                                         .fallback = red,
                                                     }) == OPTICS_GFX_OK);

        render_scene(surface, depth, &cam, scene, nullptr, px);
        const uint8_t *center = px_at(px, W / 2, H / 2);
        OT_EXPECT(center[0] > 200 && center[2] < 30);

        /* Draw-time material override (blue) supersedes the installed table. */
        render_scene(surface, depth, &cam, scene, blue, px);
        center = px_at(px, W / 2, H / 2);
        OT_EXPECT(center[2] > 200 && center[0] < 30);

        /* Load STEP + CUBICSPLINE animation and verify t=0.5 holds STEP translation at 0
         * while CUBICSPLINE scale grows smoothly to ~1.5 (widening the quad). */
        optics_scene_animation *anim = nullptr;
        OT_REQUIRE(optics_scene_load_animation_glb(scene, &(optics_scene_load_animation_glb_opts){
                                                              .bytes = anim_glb.bytes,
                                                              .byte_count = anim_glb.size,
                                                              .out = &anim,
                                                          }) == OPTICS_GFX_OK);
        OT_EXPECT(optics_scene_animation_channel_count(anim) == 2u);
        OT_EXPECT_NEAR(optics_scene_animation_duration(anim), 1.0f, 1e-5);

        OT_REQUIRE(optics_scene_apply_animation(scene, &(optics_scene_apply_animation_opts){
                                                           .animation = anim,
                                                           .time_seconds = 0.5f,
                                                           .loop = false,
                                                       }) == OPTICS_GFX_OK);
        optics_gfx_vec3 mn = {}, mx = {};
        OT_EXPECT(optics_scene_bounds(scene,
                                      &(optics_scene_bounds_opts){.out_min = &mn, .out_max = &mx}));
        /* STEP translation still 0; CUBICSPLINE scale at t=0.5 with zero tangents is 1.5 ->
         * 0.45 * 1.5 = 0.675. */
        OT_EXPECT_NEAR(mn.x, -0.675f, 1e-3);
        OT_EXPECT_NEAR(mx.x, 0.675f, 1e-3);

        /* At t=1.0, STEP jumps translation to +1.1 and scale reaches 2.0 (quad shifts right). */
        OT_REQUIRE(optics_scene_apply_animation(scene, &(optics_scene_apply_animation_opts){
                                                           .animation = anim,
                                                           .time_seconds = 1.0f,
                                                           .loop = false,
                                                       }) == OPTICS_GFX_OK);
        render_scene(surface, depth, &cam, scene, nullptr, px);
        const uint8_t *right_px = px_at(px, W / 2 + 16, H / 2);
        const uint8_t *left_px = px_at(px, W / 2 - 16, H / 2);
        OT_EXPECT(right_px[0] > 200);
        OT_EXPECT(left_px[0] < 20);

        /* Reset pose restores rest bounds and center pixels cleanly. */
        optics_scene_reset_pose(scene);
        OT_EXPECT(optics_scene_bounds(scene,
                                      &(optics_scene_bounds_opts){.out_min = &mn, .out_max = &mx}));
        OT_EXPECT_NEAR(mn.x, -0.45f, 1e-4);
        OT_EXPECT_NEAR(mx.x, 0.45f, 1e-4);

        render_scene(surface, depth, &cam, scene, nullptr, px);
        OT_EXPECT(px_at(px, W / 2, H / 2)[0] > 200);
        OT_EXPECT(px_at(px, W / 2 + 16, H / 2)[0] < 20);

        optics_gfx_device_wait_idle(device);
        optics_scene_animation_release(anim);
        optics_scene_release(scene);
        free(quad_glb.bytes);
        free(anim_glb.bytes);
    }

    /* 2. Skinned GLB mesh: animating joint "bone0" shifts the rendered pixels left. */
    {
        owned_glb skin_glb = make_skinned_scene_glb();
        owned_glb anim_glb = make_bone_shift_anim_glb();
        OT_REQUIRE(skin_glb.bytes != nullptr && anim_glb.bytes != nullptr);

        optics_scene *skinned = nullptr;
        OT_REQUIRE(optics_scene_load_glb(device, &(optics_scene_load_glb_opts){
                                                     .bytes = skin_glb.bytes,
                                                     .byte_count = skin_glb.size,
                                                     .out = &skinned,
                                                 }) == OPTICS_GFX_OK);

        optics_scene_animation *anim = nullptr;
        OT_REQUIRE(optics_scene_load_animation_glb(skinned, &(optics_scene_load_animation_glb_opts){
                                                                .bytes = anim_glb.bytes,
                                                                .byte_count = anim_glb.size,
                                                                .out = &anim,
                                                            }) == OPTICS_GFX_OK);

        /* Rest pose: centered blue quad. */
        render_scene(surface, depth, &cam, skinned, blue, px);
        OT_EXPECT(px_at(px, W / 2, H / 2)[2] > 200);
        OT_EXPECT(px_at(px, W / 2 - 16, H / 2)[2] < 20);

        /* Animated pose at t=1.0: bone0 translates to -1.1 X, pulling skinned quad left. */
        OT_REQUIRE(optics_scene_apply_animation(skinned, &(optics_scene_apply_animation_opts){
                                                             .animation = anim,
                                                             .time_seconds = 1.0f,
                                                             .loop = false,
                                                         }) == OPTICS_GFX_OK);
        render_scene(surface, depth, &cam, skinned, blue, px);
        OT_EXPECT(px_at(px, W / 2, H / 2)[2] < 20);
        OT_EXPECT(px_at(px, W / 2 - 16, H / 2)[2] > 200);

        optics_gfx_device_wait_idle(device);
        optics_scene_animation_release(anim);
        optics_scene_release(skinned);
        free(skin_glb.bytes);
        free(anim_glb.bytes);
    }

    optics_gfx_device_wait_idle(device);
    optics_gfx_material_release(blue);
    optics_gfx_material_release(red);
    optics_gfx_target_release(depth);
    optics_gfx_surface_release(surface);
    optics_gfx_device_release(device);
    return OT_REPORT();
}
