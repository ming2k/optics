/*
 * scene_cube — canonical 3D scene rendering starter for Flux (ADR-0097).
 *
 * Demonstrates:
 *   - 3D mesh creation (flux_mesh) and material binding (flux_material);
 *   - Camera projection (perspective + look_at) and world transform;
 *   - Depth buffering (flux_target) and unlit/lit Phong shading;
 *   - Native Iris windowing (zero GLFW, system cursor, fractional HiDPI).
 */

#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

typedef struct scene_cube_app {
    flux_mesh *cube;
    flux_material *mat;
    flux_target *depth;
    bool phong;
    float time;
    uint32_t width;
    uint32_t height;
} scene_cube_app;

static flux_target *recreate_depth(flux_device *device, flux_target *t, uint32_t w, uint32_t h) {
    if (t) {
        if (flux_target_width(t) == w && flux_target_height(t) == h)
            return t;
        flux_target_release(t);
    }
    flux_target_desc ddesc = {
        .type = FLUX_TYPE_TARGET_DESC,
        .usage = FLUX_TARGET_DEPTH,
        .format = FLUX_DEPTH_FORMAT,
        .width = w,
        .height = h,
    };
    flux_target *nt = nullptr;
    if (flux_target_create(device, &ddesc, &nt) != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_target_create (depth) failed: %s\n", ei.message ? ei.message : "?");
        return nullptr;
    }
    return nt;
}

static flux_mesh *make_cube(flux_device *device) {
    flux_vertex verts[8] = {
        {{-1, -1, -1}, {0, 0, -1}, {0, 0}}, {{1, -1, -1}, {0, 0, -1}, {1, 0}},
        {{1, 1, -1}, {0, 0, -1}, {1, 1}},   {{-1, 1, -1}, {0, 0, -1}, {0, 1}},
        {{-1, -1, 1}, {0, 0, 1}, {0, 0}},   {{1, -1, 1}, {0, 0, 1}, {1, 0}},
        {{1, 1, 1}, {0, 0, 1}, {1, 1}},     {{-1, 1, 1}, {0, 0, 1}, {0, 1}},
    };
    uint32_t idx[36] = {
        0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 4, 7, 0, 7, 3,
        1, 2, 6, 1, 6, 5, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2,
    };
    flux_mesh_desc md = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 8,
        .indices = idx,
        .index_count = 36,
    };
    flux_mesh *m = nullptr;
    (void)flux_mesh_create(device, &md, &m);
    return m;
}

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    scene_cube_app *app = user;

    app->cube = make_cube(device);
    if (!app->cube)
        return false;

    flux_material_desc mat_desc = {
        .type = FLUX_TYPE_MATERIAL_DESC,
        .kind = FLUX_MATERIAL_PHONG,
        .base_color = {0.85f, 0.45f, 0.25f, 1.0f},
        .color_format = FLUX_FORMAT_BGRA8_UNORM,
        .depth_format = FLUX_DEPTH_FORMAT,
        .shininess = 32.0f,
        .specular = 0.5f,
    };
    if (flux_material_create(device, &mat_desc, &app->mat) != FLUX_OK) {
        flux_mesh_release(app->cube);
        return false;
    }

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    scene_cube_app *app = user;
    if (app->depth)
        flux_target_release(app->depth);
    if (app->mat)
        flux_material_release(app->mat);
    if (app->cube)
        flux_mesh_release(app->cube);
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    scene_cube_app *app = user;

    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == LENS_KEY_ESCAPE || key == 'q' || key == 'Q') {
            iris_window_close();
            return;
        } else if (key == ' ') {
            app->phong = !app->phong;
        }
    }
    iris_request_animation_frame();

    /* Lens UI overlay banner */
    lens_column_begin(ui, &(lens_layout_opts){.pad = 16, .gap = 8, .cross = LENS_START});
    lens_row_begin(ui, &(lens_layout_opts){.pad = 8, .gap = 12, .cross = LENS_CENTER});

    if (lens_button(ui,
                    &(lens_button_opts){
                        .label = app->phong ? "Mode: Lit (Phong)" : "Mode: Unlit",
                        .variant = app->phong ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
                    })
            .clicked) {
        app->phong = !app->phong;
    }
    lens_label(ui,
               &(lens_label_opts){.text = "[Space] Toggle Shading | [Esc] Exit", .size = 13.0f});
    lens_close(ui);
    lens_close(ui);
}

static void on_prepare(flux_frame *frame, flux_canvas *canvas, flux_device *device, float scale,
                       void *user) {
    (void)canvas;
    scene_cube_app *app = user;
    app->time += 0.016f;

    int32_t log_w = 960, log_h = 540;
    iris_window_get_geometry(&log_w, &log_h);
    uint32_t pw = (uint32_t)lroundf((float)log_w * scale);
    uint32_t ph = (uint32_t)lroundf((float)log_h * scale);
    if (pw == 0 || ph == 0)
        return;

    app->width = pw;
    app->height = ph;
    app->depth = recreate_depth(device, app->depth, pw, ph);
    if (!app->depth)
        return;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);

    /* Transition depth buffer layout */
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .srcAccessMask = 0,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .image = flux_target_vk_image(app->depth),
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .levelCount = 1,
                .layerCount = 1,
            },
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);

    flux_pass_attachment color = {
        .view = VK_NULL_HANDLE,
        .load_op = FLUX_LOAD_CLEAR,
        .store_op = FLUX_STORE_STORE,
        .clear_color = {0.05f, 0.05f, 0.08f, 1.0f},
    };
    flux_pass_depth_attachment depth_att = {
        .view = flux_target_vk_view(app->depth),
        .format = DEPTH_FORMAT,
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

    VkViewport vp = {
        .x = 0.0f,
        .y = 0.0f,
        .width = (float)pw,
        .height = (float)ph,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {.offset = {0, 0}, .extent = {pw, ph}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    flux_camera cam;
    flux_camera_perspective(&cam, 1.0f, (float)pw / (float)ph, 0.1f, 100.0f);
    flux_camera_look_at(&cam, (flux_vec3){3, 2.5f, 4}, (flux_vec3){0, 0, 0}, (flux_vec3){0, 1, 0});

    float t = app->time;
    flux_quat q =
        flux_quat_axis_angle(flux_vec3_normalize((flux_vec3){0.3f, 1.0f, 0.2f}), t * 0.8f);
    flux_mat4 world = flux_mat4_rotation_quat(q);

    if (app->phong) {
        flux_scene_light light = FLUX_SCENE_LIGHT_DEFAULT;
        light.direction = (flux_vec3){-0.6f, -1.0f, -0.4f};
        light.ambient = 0.12f;
        flux_scene_draw_mesh_lit(frame, &cam, world, app->cube, app->mat, &light);
    } else {
        flux_scene_draw_mesh(frame, &cam, world, app->cube, app->mat);
    }

    flux_frame_end_pass(frame);
}

int main(void) {
    scene_cube_app app = {
        .phong = true,
    };
    printf("flux scene_cube — native Iris 3D window (Esc to quit)\n");
    return iris_app_run(&(iris_app_opts){
        .title = "flux scene — cube",
        .app_id = "org.optics.flux.scene-cube",
        .width = 960,
        .height = 540,
        .dark = true,
        .no_clear = true,
        .start = on_start,
        .stop = on_stop,
        .build = on_build,
        .prepare = on_prepare,
        .user = &app,
    });
}
