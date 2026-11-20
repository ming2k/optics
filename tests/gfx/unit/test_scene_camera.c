/*
 * Test scene camera math.
 */
#include "optics_test.h"
#include <optics/gfx.h>

#define EPS 1e-4f

int main(void) {
    /* --- perspective + look_at: eye at (0,0,5) maps to origin in view space --- */
    {
        optics_gfx_camera cam = {};
        optics_gfx_camera_perspective(
            &cam,
            &(optics_gfx_camera_perspective_opts){
                .fov_y_rad = 0.78539816339f, .aspect = 1.0f, .z_near = 0.1f, .z_far = 100.0f});
        optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0.0f, 0.0f, 5.0f},
                                  (optics_gfx_vec3){0.0f, 0.0f, 0.0f},
                                  (optics_gfx_vec3){0.0f, 1.0f, 0.0f});

        optics_gfx_vec4 ep =
            optics_gfx_mat4_transform_vec4(cam.view, (optics_gfx_vec4){0.0f, 0.0f, 5.0f, 1.0f});
        OT_EXPECT_NEAR(ep.x, 0.0f, EPS);
        OT_EXPECT_NEAR(ep.y, 0.0f, EPS);
        OT_EXPECT_NEAR(ep.z, 0.0f, EPS);
    }

    /* --- perspective: near -> NDC z=0, far -> NDC z=1 --- */
    {
        optics_gfx_mat4 p = optics_gfx_mat4_perspective(0.78539816339f, 1.0f, 0.1f, 100.0f);

        optics_gfx_vec4 near = optics_gfx_mat4_transform_vec4(p, (optics_gfx_vec4){0, 0, -0.1f, 1});
        float ndc_z_near = near.z / near.w;
        OT_EXPECT_NEAR(ndc_z_near, 0.0f, EPS);

        optics_gfx_vec4 far =
            optics_gfx_mat4_transform_vec4(p, (optics_gfx_vec4){0, 0, -100.0f, 1});
        float ndc_z_far = far.z / far.w;
        OT_EXPECT_NEAR(ndc_z_far, 1.0f, EPS);
    }

    /* --- camera struct: view-projection composition --- */
    {
        optics_gfx_camera cam = {};
        optics_gfx_camera_perspective(
            &cam,
            &(optics_gfx_camera_perspective_opts){
                .fov_y_rad = 0.78539816339f, .aspect = 1.0f, .z_near = 0.1f, .z_far = 100.0f});
        optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0.0f, 0.0f, 5.0f},
                                  (optics_gfx_vec3){0.0f, 0.0f, 0.0f},
                                  (optics_gfx_vec3){0.0f, 1.0f, 0.0f});

        optics_gfx_mat4 vp = optics_gfx_mat4_multiply(cam.projection, cam.view);

        /* World-space point on the near plane directly in front of camera. */
        optics_gfx_vec4 near_world = {0, 0, 4.9f, 1};
        optics_gfx_vec4 near_clip = optics_gfx_mat4_transform_vec4(vp, near_world);
        float near_ndc_z = near_clip.z / near_clip.w;
        OT_EXPECT_NEAR(near_ndc_z, 0.0f, EPS);

        /* World-space point on the far plane directly in front of camera. */
        optics_gfx_vec4 far_world = {0, 0, -95.0f, 1};
        optics_gfx_vec4 far_clip = optics_gfx_mat4_transform_vec4(vp, far_world);
        float far_ndc_z = far_clip.z / far_clip.w;
        OT_EXPECT_NEAR(far_ndc_z, 1.0f, EPS);

        /* Centre of attention should project to the middle of the screen. */
        optics_gfx_vec4 centre_world = {0, 0, 0, 1};
        optics_gfx_vec4 centre_clip = optics_gfx_mat4_transform_vec4(vp, centre_world);
        float centre_ndc_x = centre_clip.x / centre_clip.w;
        float centre_ndc_y = centre_clip.y / centre_clip.w;
        OT_EXPECT_NEAR(centre_ndc_x, 0.0f, EPS);
        OT_EXPECT_NEAR(centre_ndc_y, 0.0f, EPS);
    }

    /* --- orthographic sanity --- */
    {
        optics_gfx_mat4 ortho = optics_gfx_mat4_orthographic(-1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f);
        optics_gfx_vec4 corner = {-1.0f, -1.0f, 0.0f, 1.0f};
        optics_gfx_vec4 oc = optics_gfx_mat4_transform_vec4(ortho, corner);
        OT_EXPECT_NEAR(oc.x, -1.0f, EPS);
        OT_EXPECT_NEAR(oc.y, 1.0f, EPS); /* Y flipped for Vulkan */
        OT_EXPECT_NEAR(oc.z, 0.0f, EPS);
    }

    return OT_REPORT();
}
