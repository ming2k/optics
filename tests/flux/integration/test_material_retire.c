/*
 * flux_material retire: a material released deferred right after a frame
 * that drew with it must keep its VkPipeline alive until the GPU provably
 * passes that batch. Churns create -> draw -> flux_material_release_deferred
 * every frame to prove no in-flight use-after-free or validation error.
 */
#include "flux_test.h"
#include "optics_test.h"
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#define SURF_W 64u
#define SURF_H 64u
#define CHURN_FRAMES 100u

static flux_mesh *make_quad(flux_device *d) {
    flux_vertex verts[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0, 0, 1}, {0, 0}},
        {{0.5f, -0.5f, 0.0f}, {0, 0, 1}, {1, 0}},
        {{0.5f, 0.5f, 0.0f}, {0, 0, 1}, {1, 1}},
        {{-0.5f, 0.5f, 0.0f}, {0, 0, 1}, {0, 1}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    flux_mesh_desc desc = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    flux_mesh *out = nullptr;
    return flux_mesh_create(d, &desc, &out) == FLUX_OK ? out : nullptr;
}

int main(void) {
    flux_device *d = flux_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_material_retire: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    flux_surface *surface = nullptr;
    {
        flux_surface_desc desc = FLUX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        OT_EXPECT(flux_surface_create(d, &desc, &surface) == FLUX_OK);
    }

    flux_target *depth = nullptr;
    {
        flux_target_desc desc = {
            .type = FLUX_TYPE_TARGET_DESC,
            .usage = FLUX_TARGET_DEPTH,
            .format = FLUX_FORMAT_D32_SFLOAT,
            .width = SURF_W,
            .height = SURF_H,
        };
        OT_EXPECT(flux_target_create(d, &desc, &depth) == FLUX_OK);
    }

    flux_camera cam;
    flux_camera_perspective(&cam,
                            &(flux_camera_perspective_opts){.fov_y_rad = 1.0f,
                                                            .aspect = (float)SURF_W / (float)SURF_H,
                                                            .z_near = 0.1f,
                                                            .z_far = 100.0f});
    flux_camera_look_at(&cam, (flux_vec3){0, 0, 4}, (flux_vec3){0, 0, 0}, (flux_vec3){0, 1, 0});

    flux_mesh *mesh = make_quad(d);
    OT_EXPECT(mesh != nullptr);

    /* Churn: each frame creates a fresh material, records a draw, submits,
     * and releases the material immediately with flux_material_release_deferred
     * while the frame is in flight. */
    for (uint32_t i = 0; i < CHURN_FRAMES; ++i) {
        flux_material *material = nullptr;
        flux_material_desc mdesc = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {(float)i / (float)CHURN_FRAMES, 0.5f, 1.0f, 1.0f},
            .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
            .depth_format = FLUX_FORMAT_D32_SFLOAT,
        };
        OT_EXPECT(flux_material_create(d, &mdesc, &material) == FLUX_OK);

        flux_frame *frame = nullptr;
        OT_EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
        flux_frame_prepare_target(frame, depth);
        flux_pass_attachment color = {
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_STORE,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        flux_pass_depth_attachment depth_att = {
            .view = flux_target_vk_view(depth),
            .format = VK_FORMAT_D32_SFLOAT,
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_DONT_CARE,
            .clear_depth = 1.0f,
        };
        flux_pass_desc pass = {
            .type = FLUX_TYPE_PASS_DESC,
            .color_attachment_count = 1,
            .color_attachments = &color,
            .depth = &depth_att,
        };
        flux_frame_begin_pass(frame, &pass);
        flux_frame_set_viewport(frame, &(flux_frame_set_viewport_opts){.x = 0.0f,
                                                                       .y = 0.0f,
                                                                       .width = (float)SURF_W,
                                                                       .height = (float)SURF_H,
                                                                       .min_depth = 0.0f,
                                                                       .max_depth = 1.0f});
        flux_frame_set_scissor(frame, &(flux_frame_set_scissor_opts){
                                          .x = 0, .y = 0, .width = SURF_W, .height = SURF_H});
        flux_scene_draw_mesh(frame, &(flux_scene_draw_mesh_opts){.camera = &cam,
                                                                 .world = flux_mat4_identity(),
                                                                 .mesh = mesh,
                                                                 .material = material});
        flux_frame_end_pass(frame);
        OT_EXPECT(flux_frame_submit(frame) == FLUX_OK);
        OT_EXPECT(flux_frame_present(frame) == FLUX_OK);

        /* Deferred release while batch is in flight. */
        flux_material_release(material);
    }

    /* Test null device parameter (uses m->device). */
    {
        flux_material *material = nullptr;
        flux_material_desc mdesc = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
            .depth_format = FLUX_FORMAT_D32_SFLOAT,
        };
        OT_EXPECT(flux_material_create(d, &mdesc, &material) == FLUX_OK);
        flux_material_release(material);
    }

    flux_mesh_release(mesh);
    flux_target_release(depth);
    flux_surface_release(surface);
    flux_device_release(d);
    return OT_REPORT();
}
