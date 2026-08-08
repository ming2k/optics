/*
 * gltf_viewer — load a .glb with flux-scene-graph and render it.
 *
 * Modes:
 *   (no args)        build a minimal valid .glb in memory (a flat-shaded
 *                    cube) so the example is self-contained, and view it
 *                    from a fixed camera.
 *   --file path.glb  load and display that file instead.
 *   --orbit          auto-frame the model from its world-space bounding box
 *                    (flux_sg_scene_bounds) and orbit a turntable camera
 *                    around it (yaw sweeps with time, pitch held at a
 *                    slight elevation). Without --file, loads the bundled
 *                    Khronos Duck (FLUX_DUCK_ASSET, baked in by meson).
 *
 * The example owns the window, device, surface, depth target, and material
 * (it knows its render-target formats); flux-scene-graph owns the parsed
 * mesh + node tree. This split is the ADR-0016 boundary: the scene-graph
 * sibling feeds flux_scene_draw_mesh; it never touches windowing or the
 * render target. Depth is a caller-owned flux_target (peer-defined
 * attachments, ADR-0001): flux owns the image, memory, and view.
 */
#include <flux-scene-graph/scene-graph.h>
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Orbit parameters (--orbit). */
#define ORBIT_FOV_Y 1.0f   /* vertical field of view, radians (~57°)   */
#define ORBIT_SPEED 0.40f  /* yaw rate, radians/sec (~16 s per turn)   */
#define ORBIT_PITCH 0.30f  /* fixed elevation (~17° above horizon)     */
#define FRAME_MARGIN 1.25f /* model fills ~80% of the smaller axis      */

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
/*  Minimal in-memory .glb: a flat-shaded cube                        */
/* ------------------------------------------------------------------ */

/* 6 faces, 4 verts each, flat normals (outward). */
static const float CUBE_P[24 * 3] = {
    /* +Z front */ -1, -1, 1,  1,  -1, 1,  1,  1,  1,  -1, 1,  1,
    /* -Z back */ 1,   -1, -1, -1, -1, -1, -1, 1,  -1, 1,  1,  -1,
    /* +X right */ 1,  -1, 1,  1,  -1, -1, 1,  1,  -1, 1,  1,  1,
    /* -X left  */ -1, -1, -1, -1, -1, 1,  -1, 1,  1,  -1, 1,  -1,
    /* +Y top   */ -1, 1,  1,  1,  1,  1,  1,  1,  -1, -1, 1,  -1,
    /* -Y bottom*/ -1, -1, -1, 1,  -1, -1, 1,  -1, 1,  -1, -1, 1,
};
static const float CUBE_N[24 * 3] = {
    0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0,  0,  -1, 0,  0,  -1, 0,  0,  -1, 0,  0,  -1,
    1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, -1, 0,  0,  -1, 0,  0,  -1, 0,  0,  -1, 0,  0,
    0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0,  -1, 0,  0,  -1, 0,  0,  -1, 0,  0,  -1, 0,
};
static const uint16_t CUBE_I[36] = {
    0,  1,  2,  0,  2,  3,  /* +Z */
    4,  5,  6,  4,  6,  7,  /* -Z */
    8,  9,  10, 8,  10, 11, /* +X */
    12, 13, 14, 12, 14, 15, /* -X */
    16, 17, 18, 16, 18, 19, /* +Y */
    20, 21, 22, 20, 22, 23, /* -Y */
};

static uint8_t *build_cube_glb(size_t *out_len) {
    const size_t pos_bytes = sizeof(CUBE_P);                  /* 288 */
    const size_t nrm_bytes = sizeof(CUBE_N);                  /* 288 */
    const size_t idx_bytes = sizeof(CUBE_I);                  /* 72  */
    const size_t bin_len = pos_bytes + nrm_bytes + idx_bytes; /* 648, 4-aligned */

    char json[1024];
    int jn =
        snprintf(json, sizeof(json),
                 "{"
                 "\"asset\":{\"version\":\"2.0\"},"
                 "\"buffers\":[{\"byteLength\":%zu}],"
                 "\"bufferViews\":["
                 "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":%zu,\"target\":34962},"
                 "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34962},"
                 "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%zu,\"target\":34963}"
                 "],"
                 "\"accessors\":["
                 "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
                 "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
                 "{\"bufferView\":2,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}"
                 "],"
                 "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.8,0.6,0.3,1]}}],"
                 "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
                 "\"indices\":2,\"material\":0}]}],"
                 "\"nodes\":[{\"mesh\":0}],"
                 "\"scenes\":[{\"nodes\":[0]}],"
                 "\"scene\":0"
                 "}",
                 bin_len, pos_bytes, pos_bytes, nrm_bytes, pos_bytes + nrm_bytes, idx_bytes);
    if (jn <= 0 || (size_t)jn >= sizeof(json))
        return NULL;
    size_t json_len = (size_t)jn;
    /* Pad JSON chunk to 4 bytes with 0x20. */
    size_t json_padded = (json_len + 3u) & ~(size_t)3u;
    /* BIN is already 4-aligned; keep a padded length regardless. */
    size_t bin_padded = (bin_len + 3u) & ~(size_t)3u;

    size_t total = 12u + 8u + json_padded + 8u + bin_padded;
    uint8_t *buf = calloc(1, total);
    if (!buf)
        return NULL;

    uint8_t *w = buf;
    /* Header. */
    uint32_t magic = 0x46546C67u, version = 2, length = (uint32_t)total;
    memcpy(w + 0, &magic, 4);
    memcpy(w + 4, &version, 4);
    memcpy(w + 8, &length, 4);
    w += 12;
    /* JSON chunk. */
    uint32_t json_chunk_len = (uint32_t)json_padded, json_type = 0x4E4F534Au;
    memcpy(w + 0, &json_chunk_len, 4);
    memcpy(w + 4, &json_type, 4);
    memcpy(w + 8, json, json_len);
    memset(w + 8 + json_len, 0x20, json_padded - json_len);
    w += 8 + json_padded;
    /* BIN chunk. */
    uint32_t bin_chunk_len = (uint32_t)bin_padded, bin_type = 0x004E4942u;
    memcpy(w + 0, &bin_chunk_len, 4);
    memcpy(w + 4, &bin_type, 4);
    uint8_t *bin = w + 8;
    memcpy(bin + 0, CUBE_P, pos_bytes);
    memcpy(bin + pos_bytes, CUBE_N, nrm_bytes);
    memcpy(bin + pos_bytes + nrm_bytes, CUBE_I, idx_bytes);

    *out_len = total;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Slurp a whole file (--file, or the bundled Duck for --orbit)      */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Auto-framing from the scene's world-space AABB (--orbit)          */
/* ------------------------------------------------------------------ */

typedef struct {
    flux_vec3 center;
    float half_diag; /* half the bounding-box diagonal            */
} framing;

/* Camera distance so the bounding sphere fills the smaller screen axis.
 * Recomputed each frame so a resize stays correctly framed. */
static float orbit_distance(const framing *fr, float aspect) {
    float fov_y = ORBIT_FOV_Y;
    float fov_x = 2.0f * atanf(tanf(fov_y * 0.5f) * aspect);
    float fov_min = (aspect >= 1.0f) ? fov_y : fov_x;
    return (fr->half_diag / sinf(fov_min * 0.5f)) * FRAME_MARGIN;
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
    uint8_t *mem_glb = NULL;
    size_t mem_len = 0;
    const char *file_path = NULL;
    bool orbit = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file_path = argv[++i];
        else if (strcmp(argv[i], "--orbit") == 0)
            orbit = true;
    }

    const void *glb = NULL;
    size_t glb_len = 0;
    uint8_t *file_buf = NULL;
    if (file_path) {
        file_buf = read_file(file_path, &glb_len);
        if (!file_buf) {
            fprintf(stderr, "cannot open %s\n", file_path);
            return 1;
        }
        glb = file_buf;
    } else if (orbit) {
        /* --orbit without --file: the bundled Khronos Duck. Fall back to
         * the synthesised cube if the asset is missing, so the example
         * always runs. */
        file_buf = read_file(FLUX_DUCK_ASSET, &glb_len);
        if (file_buf)
            glb = file_buf;
        else
            fprintf(stderr, "cannot open %s, using in-memory cube\n", FLUX_DUCK_ASSET);
    }
    if (!glb) {
        mem_glb = build_cube_glb(&mem_len);
        if (!mem_glb) {
            fprintf(stderr, "failed to build in-memory cube\n");
            free(file_buf);
            return 1;
        }
        glb = mem_glb;
        glb_len = mem_len;
    }

    if (!glfwInit() || !glfwVulkanSupported()) {
        free(mem_glb);
        free(file_buf);
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    GLFWwindow *win = glfwCreateWindow(960, 540, orbit ? "flux gltf viewer — orbit"
                                                       : "flux gltf viewer",
                                       nullptr, nullptr);
    if (!win) {
        glfwTerminate();
        free(mem_glb);
        free(file_buf);
        return 1;
    }

    uint32_t ext_count = 0;
    const char **req_exts = glfwGetRequiredInstanceExtensions(&ext_count);
    const char *device_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = req_exts,
        .required_instance_extension_count = ext_count,
        .required_device_extensions = device_exts,
        .required_device_extension_count = sizeof(device_exts) / sizeof(*device_exts),
        .frames_in_flight = 2,
    };
    flux_device *device = nullptr;
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        glfwDestroyWindow(win);
        glfwTerminate();
        free(mem_glb);
        free(file_buf);
        return 1;
    }

    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(flux_device_vk_instance(device), win, nullptr, &vk_surface) !=
        VK_SUCCESS) {
        flux_device_release(device);
        glfwDestroyWindow(win);
        glfwTerminate();
        free(mem_glb);
        free(file_buf);
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
        free(mem_glb);
        free(file_buf);
        return 1;
    }
    glfwSetWindowUserPointer(win, surface);
    glfwSetFramebufferSizeCallback(win, on_resize);

    /* ===== end shared bootstrap; this example's actual work: ===== */
    flux_sg_scene *scene = nullptr;
    flux_result r = flux_sg_load_glb(device, glb, glb_len, &scene);
    if (r != FLUX_OK) {
        fprintf(stderr, "flux_sg_load_glb failed: %d\n", (int)r);
        goto teardown;
    }
    printf("gltf_viewer: loaded %u primitive(s)\n", flux_sg_scene_primitive_count(scene));

    /* --orbit: auto-frame from the world-space AABB of whatever loaded. */
    framing fr = {0};
    if (orbit) {
        flux_vec3 bmin, bmax;
        if (!flux_sg_scene_bounds(scene, &bmin, &bmax)) {
            fprintf(stderr, "scene has no measurable bounds\n");
            flux_sg_scene_release(scene);
            goto teardown;
        }
        fr.center = flux_vec3_scale(flux_vec3_add(bmin, bmax), 0.5f);
        fr.half_diag = flux_vec3_length(flux_vec3_sub(bmax, bmin)) * 0.5f;
        printf("orbit: centre=(%.2f,%.2f,%.2f) half-diagonal=%.2f\n", fr.center.x, fr.center.y,
               fr.center.z, fr.half_diag);
    }

    flux_material_desc mdesc = {
        .type = FLUX_TYPE_MATERIAL_DESC,
        .kind = FLUX_MATERIAL_PHONG,
        .base_color = {0.8f, 0.6f, 0.3f, 1.0f},
        .color_format = flux_format_from_vk(flux_surface_vk_format(surface)),
        .depth_format = FLUX_DEPTH_FORMAT,
        .shininess = 48.0f,
        .specular = 0.6f,
    };
    if (orbit) {
        /* Neutral white with a softer highlight for arbitrary models. */
        mdesc.base_color = (flux_vec4){1.0f, 1.0f, 1.0f, 1.0f};
        mdesc.shininess = 32.0f;
        mdesc.specular = 0.35f;
    }
    flux_material *mat = nullptr;
    if (flux_material_create(device, &mdesc, &mat) != FLUX_OK) {
        fprintf(stderr, "material create failed\n");
        flux_sg_scene_release(scene);
        goto teardown;
    }

    flux_target *depth = nullptr;
    flux_sg_draw_opts draw_opts = {.material = mat};

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
                .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                .image = flux_target_vk_image(depth),
                .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                                     .levelCount = 1,
                                     .layerCount = 1},
            };
            VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                   .imageMemoryBarrierCount = 1,
                                   .pImageMemoryBarriers = &b};
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

        VkViewport vp = {
            .width = (float)info.width, .height = (float)info.height, .maxDepth = 1.0f};
        VkRect2D sc = {.extent = {info.width, info.height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        float aspect = (float)info.width / (float)info.height;
        flux_camera cam;
        if (orbit) {
            /* Perspective + spherical eye from bounds + time. */
            float dist = orbit_distance(&fr, aspect);
            float diag = fr.half_diag * 2.0f;
            float z_near = (fr.half_diag > 0.0f) ? fr.half_diag * 0.05f : 0.1f;
            float z_far = (dist + diag) * 5.0f + 1.0f;
            flux_camera_perspective(&cam, ORBIT_FOV_Y, aspect, z_near, z_far);

            float yaw = (float)glfwGetTime() * ORBIT_SPEED;
            float cp = cosf(ORBIT_PITCH);
            flux_vec3 eye = flux_vec3_make(fr.center.x + dist * cp * sinf(yaw),
                                           fr.center.y + dist * sinf(ORBIT_PITCH),
                                           fr.center.z + dist * cp * cosf(yaw));
            flux_camera_look_at(&cam, eye, fr.center, (flux_vec3){0, 1, 0});

            char title[128];
            float deg = fmodf(yaw * 180.0f / (float)M_PI, 360.0f);
            if (deg < 0.0f)
                deg += 360.0f;
            snprintf(title, sizeof(title), "flux gltf viewer — orbit r=%.2f yaw=%.0f°", dist,
                     deg);
            glfwSetWindowTitle(win, title);
        } else {
            /* Fixed camera framing the unit cube at the origin. */
            flux_camera_perspective(&cam, 1.0f, aspect, 0.1f, 100.0f);
            flux_camera_look_at(&cam, (flux_vec3){3, 2.5f, 4}, (flux_vec3){0, 0, 0},
                                (flux_vec3){0, 1, 0});
        }

        flux_scene_light light = FLUX_SCENE_LIGHT_DEFAULT;
        light.direction = orbit ? (flux_vec3){-0.5f, -0.9f, -0.35f}
                                : (flux_vec3){-0.6f, -1.0f, -0.4f};
        light.ambient = orbit ? 0.22f : 0.12f;
        draw_opts.light = &light;
        flux_sg_draw(frame, &cam, scene, &draw_opts);

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
    }

    flux_device_wait_idle(device);
    flux_target_release(depth);
    flux_material_release(mat);
    flux_sg_scene_release(scene);

teardown:
    flux_surface_release(surface);
    vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, nullptr);
    flux_device_release(device);
    glfwDestroyWindow(win);
    glfwTerminate();
    free(mem_glb);
    free(file_buf);
    return 0;
}
