/*
 * particles_terrain — an animated heightfield rendered as a point cloud.
 *
 * A flat grid of particles is generated entirely inside the vertex
 * shader (no vertex buffer): each point's x/z is fixed on a square and
 * its y is a sum of travelling sinusoids, so the plane develops moving
 * peaks and valleys. Points are sized and coloured by height, drawn with
 * additive blending — ridges accumulate into a glowing terrain with no
 * triangle mesh at all.
 *
 * This is the first example that combines the public graphics-pipeline
 * API (from hello_triangle) with a depth-tested pass (from scene_cube),
 * using a POINT_LIST topology the built-in scene/material pipelines
 * don't expose, and a flux_target for the depth attachment.
 *
 * Teaches:
 *   - a POINT_LIST pipeline via flux_graphics_pipeline (additive particles)
 *   - a caller-owned depth attachment via flux_target (peer-defined
 *     attachments; flux owns the image + view + backing memory)
 *   - a perspective camera + orbiting world matrix from flux math
 *   - push constants feeding time + camera into a procedural vertex shader
 * Key flux APIs:  flux_graphics_pipeline_create/_bind, flux_target_create,
 *                 flux_camera_perspective/_look_at, flux_mat4_multiply,
 *                 flux_surface_begin_frame, flux_frame_begin_pass
 * Plumbing (raw Vulkan, not flux): GLFW window + VkSurfaceKHR creation,
 *   the per-frame depth layout-transition barrier.
 */
#include <flux/flux.h>
#include <flux/math.h>
#include <flux/scene.h> /* flux_camera + perspective/look_at */
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include "pipeline_cache.h"

#include <math.h>
#include <stdalign.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V — uint32_t-aligned so Vulkan can read it as code words. */
alignas(uint32_t) static const unsigned char terrain_vert_spv[] = {
#embed "particles_terrain.vert.spv"
};
alignas(uint32_t) static const unsigned char terrain_frag_spv[] = {
#embed "particles_terrain.frag.spv"
};

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT

/* Must match the GRID constant in particles_terrain.vert. */
#define GRID 256
#define POINT_COUNT (GRID * GRID)

/* Push constants: MVP + time + base point size. Matches the PC block
 * in the vertex shader. */
typedef struct terrain_push {
    float mvp[16];
    float time;
    float point_size;
} terrain_push;

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
/*  Main loop                                                         */
/* ------------------------------------------------------------------ */

static void on_resize(GLFWwindow *win, int w, int h) {
    flux_surface *surface = glfwGetWindowUserPointer(win);
    if (surface && w > 0 && h > 0)
        (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
}

int main(void) {
    int major, minor, patch;
    flux_version(&major, &minor, &patch);
    printf("flux %d.%d.%d (%s)\n", major, minor, patch, flux_version_string());

    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 1;
    }
    if (!glfwVulkanSupported()) {
        fprintf(stderr, "no Vulkan via GLFW\n");
        glfwTerminate();
        return 1;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(960, 540, "flux particles terrain", nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    flux_pipeline_cache_file cache = FLUX_PIPELINE_CACHE_FILE_INIT;
    flux_pipeline_cache_file_set_default_path(&cache, "particles_terrain.bin");

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
    flux_result r = flux_device_create(&ddesc, &device);
    if (r != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_device_create -> %s\n  %s\n", flux_result_string(r),
                ei.message ? ei.message : "(no info)");
        glfwDestroyWindow(win);
        glfwTerminate();
        return (int)r;
    }

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(flux_device_vk_instance(device), win, nullptr, &vk_surface) !=
        VK_SUCCESS) {
        fprintf(stderr, "glfwCreateWindowSurface failed\n");
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
    r = flux_surface_create(device, &sdesc, &surface);
    if (r != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_surface_create -> %s\n  %s\n", flux_result_string(r),
                ei.message ? ei.message : "(no info)");
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        return (int)r;
    }
    glfwSetWindowUserPointer(win, surface);
    glfwSetFramebufferSizeCallback(win, on_resize);

    /* ===== end shared bootstrap (window + device + surface) =====
     * Everything below is what this example is actually about: a
     * point-list particle pipeline, a caller-owned depth image, and
     * a draw loop that feeds a time-driven heightfield shader. */
    flux_graphics_pipeline_desc pdesc = FLUX_GRAPHICS_PIPELINE_DESC_INIT;
    pdesc.vertex_spirv = (const uint32_t *)terrain_vert_spv;
    pdesc.vertex_spirv_word_count = sizeof(terrain_vert_spv) / sizeof(uint32_t);
    pdesc.fragment_spirv = (const uint32_t *)terrain_frag_spv;
    pdesc.fragment_spirv_word_count = sizeof(terrain_frag_spv) / sizeof(uint32_t);
    pdesc.topology = FLUX_TOPOLOGY_POINT_LIST; /* particles, not triangles */
    pdesc.cull = FLUX_CULL_NONE;               /* points have no face      */
    pdesc.blend = FLUX_BLEND_PRESET_ADDITIVE;  /* glowing accumulation     */
    pdesc.depth = FLUX_DEPTH_TEST_AND_WRITE;   /* sort ridges/valleys      */
    pdesc.color_format = flux_format_from_vk(flux_surface_vk_format(surface));
    pdesc.depth_format = FLUX_DEPTH_FORMAT;
    pdesc.push_constant_bytes = sizeof(terrain_push);

    flux_graphics_pipeline *pipe = nullptr;
    if (flux_graphics_pipeline_create(device, &pdesc, &pipe) != FLUX_OK) {
        flux_error_info ei;
        flux_get_last_error(&ei);
        fprintf(stderr, "flux_graphics_pipeline_create failed: %s\n",
                ei.message ? ei.message : "?");
        goto teardown_surface;
    }
    printf("particle pipeline ready (%d×%d = %d points)\n", GRID, GRID, POINT_COUNT);

    flux_target *depth = nullptr;

    int frame_no = 0;
    while (!glfwWindowShouldClose(win)) {
        glfwPollEvents();

        flux_frame *frame = nullptr;
        r = flux_surface_begin_frame(surface, nullptr, &frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
            continue;
        }
        if (r == FLUX_ERROR_INVALID_STATE)
            continue;
        if (r != FLUX_OK) {
            fprintf(stderr, "begin_frame -> %s\n", flux_result_string(r));
            break;
        }

        flux_surface_info info;
        flux_surface_get_info(surface, &info);

        depth = depth_ensure(depth, device, info.width, info.height);
        if (!depth)
            break;

        /* Transition depth UNDEFINED -> DEPTH_ATTACHMENT_OPTIMAL. Done
         * every frame — cheap and correct, mirroring scene_cube. */
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
            .clear_color = {0.02f, 0.02f, 0.04f, 1.0f},
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

        /* Camera: perspective, orbiting around the terrain centre. */
        float t = (float)glfwGetTime();
        float orbit_r = 16.0f;
        flux_camera cam;
        flux_camera_perspective(&cam, 1.0f, (float)info.width / (float)info.height, 0.1f, 100.0f);
        flux_camera_look_at(&cam,
                            (flux_vec3){cosf(t * 0.15f) * orbit_r, 8.0f, sinf(t * 0.15f) * orbit_r},
                            (flux_vec3){0, 0, 0}, (flux_vec3){0, 1, 0});

        /* MVP = projection * view * world (identity world here). */
        flux_mat4 view_proj = flux_mat4_multiply(cam.projection, cam.view);
        flux_mat4 mvp = flux_mat4_multiply(view_proj, flux_mat4_identity());

        terrain_push pc;
        memcpy(pc.mvp, mvp.m, sizeof(pc.mvp));
        pc.time = t;
        /* A zero base is a shader-side sentinel for the mandatory 1px
         * fallback when Vulkan's optional largePoints feature is absent. */
        pc.point_size = flux_device_supports_large_points(device) ? 3.0f : 0.0f;
        flux_graphics_pipeline_bind(frame, pipe, &pc, sizeof(pc));

        vkCmdDraw(cmd, POINT_COUNT, 1, 0, 0);

        flux_frame_end_pass(frame);
        r = flux_frame_submit(frame);
        if (r != FLUX_OK) {
            fprintf(stderr, "submit -> %s\n", flux_result_string(r));
            break;
        }
        r = flux_frame_present(frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            int w, h;
            glfwGetFramebufferSize(win, &w, &h);
            if (w > 0 && h > 0)
                (void)flux_surface_resize(surface, (uint32_t)w, (uint32_t)h);
        } else if (r != FLUX_OK) {
            fprintf(stderr, "present -> %s\n", flux_result_string(r));
            break;
        }

        if (++frame_no == 1)
            printf("first particle frame presented (extent %ux%u)\n", info.width, info.height);
    }

    flux_device_wait_idle(device);
    flux_target_release(depth);
    flux_graphics_pipeline_release(pipe);
teardown_surface:
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    return 0;
}
