/*
 * Test scene camera math.
 */
#include "test_helpers.h"
#include <flux/flux.h>

#define EPS 1e-4f

int main(void) {
    /* --- perspective + look_at: eye at (0,0,5) maps to origin in view space --- */
    {
        flux_camera cam = {0};
        flux_camera_perspective(&cam, 0.78539816339f, 1.0f, 0.1f, 100.0f);
        flux_camera_look_at(&cam, (flux_vec3){0.0f, 0.0f, 5.0f}, (flux_vec3){0.0f, 0.0f, 0.0f},
                            (flux_vec3){0.0f, 1.0f, 0.0f});

        flux_vec4 ep = flux_mat4_transform_vec4(cam.view, (flux_vec4){0.0f, 0.0f, 5.0f, 1.0f});
        EXPECT_NEAR(ep.x, 0.0f, EPS);
        EXPECT_NEAR(ep.y, 0.0f, EPS);
        EXPECT_NEAR(ep.z, 0.0f, EPS);
    }

    /* --- perspective: near -> NDC z=0, far -> NDC z=1 --- */
    {
        flux_mat4 p = flux_mat4_perspective(0.78539816339f, 1.0f, 0.1f, 100.0f);

        flux_vec4 near = flux_mat4_transform_vec4(p, (flux_vec4){0, 0, -0.1f, 1});
        float ndc_z_near = near.z / near.w;
        EXPECT_NEAR(ndc_z_near, 0.0f, EPS);

        flux_vec4 far = flux_mat4_transform_vec4(p, (flux_vec4){0, 0, -100.0f, 1});
        float ndc_z_far = far.z / far.w;
        EXPECT_NEAR(ndc_z_far, 1.0f, EPS);
    }

    /* --- camera struct: view-projection composition --- */
    {
        flux_camera cam = {0};
        flux_camera_perspective(&cam, 0.78539816339f, 1.0f, 0.1f, 100.0f);
        flux_camera_look_at(&cam, (flux_vec3){0.0f, 0.0f, 5.0f}, (flux_vec3){0.0f, 0.0f, 0.0f},
                            (flux_vec3){0.0f, 1.0f, 0.0f});

        flux_mat4 vp = flux_mat4_multiply(cam.projection, cam.view);

        /* World-space point on the near plane directly in front of camera. */
        flux_vec4 near_world = {0, 0, 4.9f, 1};
        flux_vec4 near_clip = flux_mat4_transform_vec4(vp, near_world);
        float near_ndc_z = near_clip.z / near_clip.w;
        EXPECT_NEAR(near_ndc_z, 0.0f, EPS);

        /* World-space point on the far plane directly in front of camera. */
        flux_vec4 far_world = {0, 0, -95.0f, 1};
        flux_vec4 far_clip = flux_mat4_transform_vec4(vp, far_world);
        float far_ndc_z = far_clip.z / far_clip.w;
        EXPECT_NEAR(far_ndc_z, 1.0f, EPS);

        /* Centre of attention should project to the middle of the screen. */
        flux_vec4 centre_world = {0, 0, 0, 1};
        flux_vec4 centre_clip = flux_mat4_transform_vec4(vp, centre_world);
        float centre_ndc_x = centre_clip.x / centre_clip.w;
        float centre_ndc_y = centre_clip.y / centre_clip.w;
        EXPECT_NEAR(centre_ndc_x, 0.0f, EPS);
        EXPECT_NEAR(centre_ndc_y, 0.0f, EPS);
    }

    /* --- orthographic sanity --- */
    {
        flux_mat4 ortho = flux_mat4_orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);
        flux_vec4 corner = {-1.0f, -1.0f, 0.0f, 1.0f};
        flux_vec4 oc = flux_mat4_transform_vec4(ortho, corner);
        EXPECT_NEAR(oc.x, -1.0f, EPS);
        EXPECT_NEAR(oc.y, 1.0f, EPS); /* Y flipped for Vulkan */
        EXPECT_NEAR(oc.z, 0.0f, EPS);
    }

    TEST_SUMMARY();
}
