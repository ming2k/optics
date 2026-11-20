/*
 * flux_material retire: a material released deferred right after a frame
 * that drew with it must keep its VkPipeline alive until the GPU provably
 * passes that batch. Churns create -> draw -> flux_material_release_deferred
 * every frame to prove no in-flight use-after-free or validation error.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>

#define SURF_W 64u
#define SURF_H 64u
#define CHURN_FRAMES 100u

static optics_gfx_mesh *make_quad(optics_gfx_device *d) {
    optics_gfx_vertex verts[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0}},
        {{0.5f, 0.5f, 0.0f}, {0, 0, 1}, {1, 1}},
        {{-0.5f, 0.5f, 0.0f}, {0, 0, 1}, {0, 1}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    optics_gfx_mesh_desc desc = {
        .type = OPTICS_GFX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    optics_gfx_mesh *out = nullptr;
    return optics_gfx_mesh_create(d, &desc, &out) == OPTICS_GFX_OK ? out : nullptr;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_material_retire: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *surface = nullptr;
    {
        optics_gfx_surface_desc desc = OPTICS_GFX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        OT_EXPECT(optics_gfx_surface_create(d, &desc, &surface) == OPTICS_GFX_OK);
    }

    optics_gfx_target *depth = nullptr;
    {
        optics_gfx_target_desc desc = {
            .type = OPTICS_GFX_TYPE_TARGET_DESC,
            .usage = OPTICS_GFX_TARGET_DEPTH,
            .format = OPTICS_GFX_FORMAT_D32_SFLOAT,
            .width = SURF_W,
            .height = SURF_H,
        };
        OT_EXPECT(optics_gfx_target_create(d, &desc, &depth) == OPTICS_GFX_OK);
    }

    optics_gfx_camera cam;
    optics_gfx_camera_perspective(
        &cam, &(optics_gfx_camera_perspective_opts){.fov_y_rad = 1.0f,
                                                    .aspect = (float)SURF_W / (float)SURF_H,
                                                    .z_near = 0.1f,
                                                    .z_far = 100.0f});
    optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0, 0, 4}, (optics_gfx_vec3){0, 0, 0},
                              (optics_gfx_vec3){0, 1, 0});

    optics_gfx_mesh *mesh = make_quad(d);
    OT_EXPECT(mesh != nullptr);

    /* Churn: each frame creates a fresh material, records a draw, submits,
     * and releases the material immediately with flux_material_release_deferred
     * while the frame is in flight. */
    for (uint32_t i = 0; i < CHURN_FRAMES; ++i) {
        optics_gfx_material *material = nullptr;
        optics_gfx_material_desc mdesc = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {(float)i / (float)CHURN_FRAMES, 0.5f, 1.0f, 1.0f},
            .color_format = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(surface)),
            .depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT,
        };
        OT_EXPECT(optics_gfx_material_create(d, &mdesc, &material) == OPTICS_GFX_OK);

        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
        optics_gfx_frame_prepare_target(frame, depth);
        optics_gfx_pass_attachment color = {
            .load_op = OPTICS_GFX_LOAD_CLEAR,
            .store_op = OPTICS_GFX_STORE_STORE,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        optics_gfx_pass_depth_attachment depth_att = {
            .view = optics_gfx_target_vk_view(depth),
            .format = VK_FORMAT_D32_SFLOAT,
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
        optics_gfx_frame_set_viewport(frame,
                                      &(optics_gfx_frame_set_viewport_opts){.x = 0.0f,
                                                                            .y = 0.0f,
                                                                            .width = (float)SURF_W,
                                                                            .height = (float)SURF_H,
                                                                            .min_depth = 0.0f,
                                                                            .max_depth = 1.0f});
        optics_gfx_frame_set_scissor(frame, &(optics_gfx_frame_set_scissor_opts){
                                                .x = 0, .y = 0, .width = SURF_W, .height = SURF_H});
        optics_gfx_scene_draw_mesh(
            frame, &(optics_gfx_scene_draw_mesh_opts){.camera = &cam,
                                                      .world = optics_gfx_mat4_identity(),
                                                      .mesh = mesh,
                                                      .material = material});
        optics_gfx_frame_end_pass(frame);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

        /* Deferred release while batch is in flight. */
        optics_gfx_material_release(material);
    }

    /* Test null device parameter (uses m->device). */
    {
        optics_gfx_material *material = nullptr;
        optics_gfx_material_desc mdesc = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(surface)),
            .depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT,
        };
        OT_EXPECT(optics_gfx_material_create(d, &mdesc, &material) == OPTICS_GFX_OK);
        optics_gfx_material_release(material);
    }

    optics_gfx_mesh_release(mesh);
    optics_gfx_target_release(depth);
    optics_gfx_surface_release(surface);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
