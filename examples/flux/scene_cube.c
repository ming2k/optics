/*
 * scene_cube — a spinning, depth-tested cube.
 *
 * Renders one indexed mesh with an unlit material under a perspective
 * camera. The caller owns the depth image and recreates it on resize:
 * flux_pass_desc.depth carries that peer-supplied attachment, matching
 * the "peer-defined attachments" tenet from ADR-0001.
 *
 * Teaches:
 *   - creating a mesh + material and drawing it with flux_scene_draw_mesh
 *   - a perspective camera + per-frame world matrix from a quaternion
 *   - a caller-owned depth attachment via flux_target (peer-defined
 *     attachments; flux owns the image + view + backing memory)
 * Key flux APIs:  flux_mesh_create, flux_material_create,
 *                 flux_scene_draw_mesh, flux_target_create,
 *                 flux_camera_perspective/_look_at
 * Plumbing (raw Vulkan, not flux): GLFW window + VkSurfaceKHR creation,
 *   the per-frame depth layout-transition barrier.
 */
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "pipeline_cache.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT

/* ------------------------------------------------------------------ */
/*  Depth render target, recreated on resize                          */
/*                                                                    */
/*  flux owns the image + backing allocator memory + view; we just    */
/*  recreate it when the swapchain extent changes and hand its view   */
/*  to the pass each frame.                                           */
/* ------------------------------------------------------------------ */

static flux_target *depth_ensure(flux_target *t, flux_device *device, uint32_t w, uint32_t h) {
    if (t && flux_target_width(t) == w && flux_target_height(t) == h)
        return t;
    if (t) {
        vkDeviceWaitIdle(flux_device_vk_device(device));
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

/* ------------------------------------------------------------------ */
/*  Cube mesh                                                         */
/* ------------------------------------------------------------------ */

static flux_mesh *make_cube(flux_device *device) {
    /* 8 corners, indexed for 12 triangles (CCW from outside). */
    flux_vertex verts[8] = {
        {{-1, -1, -1}, {0, 0, -1}, {0, 0}}, {{1, -1, -1}, {0, 0, -1}, {1, 0}},
        {{1, 1, -1}, {0, 0, -1}, {1, 1}},   {{-1, 1, -1}, {0, 0, -1}, {0, 1}},
        {{-1, -1, 1}, {0, 0, 1}, {0, 0}},   {{1, -1, 1}, {0, 0, 1}, {1, 0}},
        {{1, 1, 1}, {0, 0, 1}, {1, 1}},     {{-1, 1, 1}, {0, 0, 1}, {0, 1}},
    };
    /* CCW winding when viewed from outside (front face). */
    uint32_t idx[36] = {
        /* -Z back   */ 0, 2, 1, 0, 3, 2,
        /* +Z front  */ 4, 5, 6, 4, 6, 7,
        /* -X left   */ 0, 4, 7, 0, 7, 3,
        /* +X right  */ 1, 2, 6, 1, 6, 5,
        /* -Y bottom */ 0, 1, 5, 0, 5, 4,
        /* +Y top    */ 3, 7, 6, 3, 6, 2,
    };
    flux_mesh_desc md = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 8,
        .indices = idx,
        .index_count = 36,
    };
    flux_mesh *m = nullptr;
    if (flux_mesh_create(device, &md, &m) != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "mesh create failed: %s\n", ei.message ? ei.message : "?");
    }
    return m;
}

/* ------------------------------------------------------------------ */
/*  Main loop                                                         */
/* ------------------------------------------------------------------ */

static void on_resize(GLFWwindow *win, int w, int h) {
    flux_surface *surface = glfwGetWindowUserPointer(win);
    if (surface && w > 0 && h > 0)
        (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
}

int main(int argc, char **argv) {
    /* --phong draws the cube with the Blinn-Phong lit material and an
     * explicit directional light instead of the flat unlit default. */
    bool phong = argc > 1 && strcmp(argv[1], "--phong") == 0;

    if (!glfwInit() || !glfwVulkanSupported())
        return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(960, 540, "flux cube", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
    flux_pipeline_cache_file_set_default_path(&cache, "scene_cube.bin");

    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = req_exts,
        .required_instance_extension_count = ext_count,
        .required_device_extensions = device_exts,
        .required_device_extension_count = sizeof(device_exts) / sizeof(*device_exts),
        .frames_in_flight = 2,
        .pipeline_cache_load = flux_pipeline_cache_file_load,
        .pipeline_cache_save = flux_pipeline_cache_file_save,
        .pipeline_cache_userdata = &cache,
    };
    flux_device *device = nullptr;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(flux_device_vk_instance(device), win, nullptr, &vk_surface) !=
        VK_SUCCESS) {
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(win, &fbw, &fbh);
    flux_surface_desc sdesc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width = (uint32_t)fbw,
        .height = (uint32_t)fbh,
        .vsync = true,
    };
    flux_surface *surface = nullptr;
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return 1;
    }
    glfwSetWindowUserPointer(win, surface);
    glfwSetFramebufferSizeCallback(win, on_resize);

    /* ===== end shared bootstrap (window + device + surface) =====
     * Everything below is what this example is actually about: a mesh,
     * a material, a caller-owned depth image, and the draw loop. */
    flux_mesh *cube = make_cube(device);
    if (!cube)
        goto teardown;

    flux_material_desc mdesc = {
        .type = FLUX_TYPE_MATERIAL_DESC,
        .kind = phong ? FLUX_MATERIAL_PHONG : FLUX_MATERIAL_UNLIT,
        .base_color = {0.8f, 0.6f, 0.3f, 1.0f},
        .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
        .depth_format = FLUX_DEPTH_FORMAT,
        .shininess = 48.0f, /* PHONG only; ignored by UNLIT */
        .specular = 0.6f,
    };
    flux_material *mat = nullptr;
    if (flux_material_create(device, &mdesc, &mat) != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "material create failed: %s\n", ei.message ? ei.message : "?");
        goto teardown_mesh;
    }
    printf("scene_cube ready\n");

    flux_target *depth = nullptr;

    int frame_no = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        flux_frame *frame = nullptr;
        flux_result r = flux_surface_begin_frame(surface, nullptr, &frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
            continue;
        }
        if (r == FLUX_ERROR_INVALID_STATE)
            continue;
        if (r != FLUX_OK)
            break;

        flux_surface_info info;
        flux_surface_get_info(surface, &info);

        depth = depth_ensure(depth, device, info.width, info.height);
        if (!depth)
            break;

        /* Transition depth UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL once
         * per resize (we don't track lifetime cleanly here, so do it
         * every frame — cheap and correct). */
        VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
        {
            VkImageMemoryBarrier2 b = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                .srcAccessMask = 0,
                .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .image = flux_target_vk_image(depth),
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
        }

        flux_pass_attachment color = {
            .view = VK_NULL_HANDLE,
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_STORE,
            .clear_color = {0.05f, 0.05f, 0.08f, 1.0f},
        };
        flux_pass_depth_attachment depth_att = {
            .view = flux_target_vk_view(depth),
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

        /* Viewport / scissor. */
        VkViewport vp = {
            .x = 0.0f,
            .y = 0.0f,
            .width = (float)info.width,
            .height = (float)info.height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D sc = {.offset = {0, 0}, .extent = {info.width, info.height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        /* Camera + spinning world matrix. */
        flux_camera cam;
        flux_camera_perspective(&cam, 1.0f, (float)info.width / (float)info.height, 0.1f, 100.0f);
        flux_camera_look_at(&cam, (flux_vec3){3, 2.5f, 4}, (flux_vec3){0, 0, 0},
                            (flux_vec3){0, 1, 0});

        float t = (float)glfwGetTime();
        flux_quat q =
            flux_quat_axis_angle(flux_vec3_normalize((flux_vec3){0.3f, 1.0f, 0.2f}), t * 0.8f);
        flux_mat4 world = flux_mat4_rotation_quat(q);

        if (phong) {
            flux_scene_light light = FLUX_SCENE_LIGHT_DEFAULT;
            light.direction = (flux_vec3){-0.6f, -1.0f, -0.4f};
            light.ambient = 0.12f;
            flux_scene_draw_mesh_lit(frame, &cam, world, cube, mat, &light);
        } else {
            flux_scene_draw_mesh(frame, &cam, world, cube, mat);
        }

        flux_frame_end_pass(frame);
        r = flux_frame_submit(frame);
        if (r != FLUX_OK)
            break;
        r = flux_frame_present(frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
        } else if (r != FLUX_OK)
            break;

        if (++frame_no == 1)
            printf("first cube frame presented (extent %ux%u)\n", info.width, info.height);
    }

    flux_device_wait_idle(device);
    flux_target_release(depth);
    flux_material_release(mat);
teardown_mesh:
    flux_mesh_release(cube);
teardown:
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
