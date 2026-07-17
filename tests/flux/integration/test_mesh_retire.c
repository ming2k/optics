/*
 * flux_mesh retire queue: a mesh released right after a frame that
 * drew it must keep its vertex/index buffers alive until the GPU
 * provably passes that batch. The pre-fix behaviour destroyed both
 * buffers and freed their memory inline — the same hazard that
 * motivated the image retire queue (i915: GPU hang -> context reset ->
 * VK_ERROR_DEVICE_LOST). This test churns create -> draw -> release
 * every frame so any inline destruction trips an error on real
 * hardware. Skips if no Vulkan device.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#define SURF_W 64u
#define SURF_H 64u
#define CHURN_FRAMES 400u

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
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_mesh_retire: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *surface = nullptr;
    {
        flux_surface_desc desc = FLUX_SURFACE_DESC_INIT;
        desc.width = SURF_W;
        desc.height = SURF_H;
        EXPECT(flux_surface_create(d, &desc, &surface) == FLUX_OK);
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
        EXPECT(flux_target_create(d, &desc, &depth) == FLUX_OK);
    }

    flux_camera cam;
    flux_camera_perspective(&cam, 1.0f, (float)SURF_W / (float)SURF_H, 0.1f, 100.0f);
    flux_camera_look_at(&cam, (flux_vec3){0, 0, 4}, (flux_vec3){0, 0, 0}, (flux_vec3){0, 1, 0});

    flux_material *material = nullptr;
    {
        flux_material_desc desc = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 0, 0, 1},
            .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
            .depth_format = FLUX_FORMAT_D32_SFLOAT,
        };
        EXPECT(flux_material_create(d, &desc, &material) == FLUX_OK);
    }

    /* Churn: each frame draws a fresh indexed mesh, then releases it
     * immediately after present — while the batch that bound its
     * vertex/index buffers may still be in flight. Deferred destruction
     * must keep those buffers valid. */
    for (uint32_t i = 0; i < CHURN_FRAMES; ++i) {
        flux_mesh *mesh = make_quad(d);
        EXPECT(mesh != nullptr);

        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
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
        flux_frame_set_viewport(frame, 0.0f, 0.0f, (float)SURF_W, (float)SURF_H, 0.0f, 1.0f);
        flux_frame_set_scissor(frame, 0, 0, SURF_W, SURF_H);
        flux_scene_draw_mesh(frame, &cam, flux_mat4_identity(), mesh, material);
        flux_frame_end_pass(frame);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);

        flux_mesh_release(mesh);
    }

    /* Release-without-frame: zombies parked after the last submission
     * must be destroyed by device teardown (drain path). */
    flux_mesh *stray = make_quad(d);
    EXPECT(stray != nullptr);
    flux_mesh_release(stray);

    flux_material_release(material);
    flux_target_release(depth);
    flux_surface_release(surface);
    flux_device_release(d);
    TEST_SUMMARY();
}
