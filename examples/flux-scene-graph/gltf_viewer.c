/*
 * gltf_viewer — glTF 2.0 asset loader and 3D scene visualizer (ADR-0097).
 *
 * Demonstrates:
 *   - glTF 2.0 binary (.glb) loading and scene graph traversal;
 *   - Automatic scene framing and 3D orbit camera;
 *   - Depth buffering, lit Phong rendering, and dynamic resizing;
 *   - Native Iris windowing (zero GLFW, system cursor, fractional HiDPI).
 */

#include <flux-scene-graph/scene-graph.h>
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>
#include <iris/app.h>
#include <iris/iris.h>
#include <iris/window.h>
#include <lens/lens.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

enum {
    ORBIT_SPEED_DEG = 45,
};

static const float ORBIT_SPEED = 0.6f;
static const float ORBIT_PITCH = 0.35f;
static const float ORBIT_FOV_Y = 1.0f;
static const float FRAME_MARGIN = 1.25f;

typedef struct framing {
    flux_vec3 center;
    float half_diag;
} framing;

typedef struct gltf_viewer_app {
    const void *glb;
    size_t glb_len;
    bool orbit;
    float time;

    flux_sg_scene *scene;
    flux_material *mat;
    flux_target *depth;
    framing fr;
} gltf_viewer_app;

static float orbit_distance(const framing *fr, float aspect) {
    float fov_y = ORBIT_FOV_Y;
    float fov_x = 2.0f * atanf(tanf(fov_y * 0.5f) * aspect);
    float fov_min = (aspect >= 1.0f) ? fov_y : fov_x;
    return (fr->half_diag / sinf(fov_min * 0.5f)) * FRAME_MARGIN;
}

static flux_target *depth_ensure(flux_target *t, flux_device *device, uint32_t w, uint32_t h) {
    if (t) {
        if (flux_target_width(t) == w && flux_target_height(t) == h)
            return t;
        flux_target_release(t);
    }
    flux_target_desc desc = {
        .type = FLUX_TYPE_TARGET_DESC,
        .usage = FLUX_TARGET_DEPTH,
        .format = FLUX_DEPTH_FORMAT,
        .width = w,
        .height = h,
    };
    flux_target *nt = nullptr;
    (void)flux_target_create(device, &desc, &nt);
    return nt;
}

static uint8_t *build_cube_glb(size_t *out_len) {
    static const float pos[72] = {
        -1, -1, 1,  1,  -1, 1,  1,  1,  1,  -1, 1,  1,  1,  -1, 1,  1,  -1, -1,
        1,  1,  -1, 1,  1,  1,  1,  -1, -1, -1, -1, -1, -1, 1,  -1, 1,  1,  -1,
        -1, -1, -1, -1, -1, 1,  -1, 1,  1,  -1, 1,  -1, -1, 1,  1,  1,  1,  1,
        1,  1,  -1, -1, 1,  -1, -1, -1, -1, 1,  -1, -1, 1,  -1, 1,  -1, -1, 1,
    };
    static const float norm[72] = {
        0, 0, 1,  0, 0, 1,  0, 0, 1,  0, 0, 1,  1,  0,  0, 1,  0,  0, 1,  0,  0, 1,  0,  0,
        0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, -1, 0,  0, -1, 0,  0, -1, 0,  0, -1, 0,  0,
        0, 1, 0,  0, 1, 0,  0, 1, 0,  0, 1, 0,  0,  -1, 0, 0,  -1, 0, 0,  -1, 0, 0,  -1, 0,
    };
    static const uint16_t idx[36] = {
        0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
    };

    uint8_t bin[sizeof(pos) + sizeof(norm) + sizeof(idx)];
    memcpy(bin, pos, sizeof(pos));
    memcpy(bin + sizeof(pos), norm, sizeof(norm));
    memcpy(bin + sizeof(pos) + sizeof(norm), idx, sizeof(idx));
    size_t bin_len = sizeof(bin);

    const char *json = "{\"asset\":{\"version\":\"2.0\"},"
                       "\"buffers\":[{\"byteLength\":648}],"
                       "\"bufferViews\":["
                       "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":288},"
                       "{\"buffer\":0,\"byteOffset\":288,\"byteLength\":288},"
                       "{\"buffer\":0,\"byteOffset\":576,\"byteLength\":72}"
                       "],"
                       "\"accessors\":["
                       "{\"bufferView\":0,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\","
                       "\"min\":[-1,-1,-1],\"max\":[1,1,1]},"
                       "{\"bufferView\":1,\"componentType\":5126,\"count\":24,\"type\":\"VEC3\"},"
                       "{\"bufferView\":2,\"componentType\":5123,\"count\":36,\"type\":\"SCALAR\"}"
                       "],"
                       "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1},"
                       "\"indices\":2}]}],"
                       "\"nodes\":[{\"mesh\":0}],"
                       "\"scenes\":[{\"nodes\":[0]}],"
                       "\"scene\":0}";

    size_t json_len = strlen(json);
    size_t json_pad = (4 - (json_len % 4)) % 4;
    size_t json_chunk = json_len + json_pad;
    size_t bin_pad = (4 - (bin_len % 4)) % 4;
    size_t bin_chunk = bin_len + bin_pad;
    size_t total = 12 + 8 + json_chunk + 8 + bin_chunk;

    uint8_t *glb = calloc(1, total);
    if (!glb)
        return NULL;

    uint32_t *hdr = (uint32_t *)glb;
    hdr[0] = 0x46546C67;
    hdr[1] = 2;
    hdr[2] = (uint32_t)total;

    uint8_t *p = glb + 12;
    uint32_t *jc = (uint32_t *)p;
    jc[0] = (uint32_t)json_chunk;
    jc[1] = 0x4E4F534A;
    memcpy(p + 8, json, json_len);
    memset(p + 8 + json_len, ' ', json_pad);
    p += 8 + json_chunk;

    uint32_t *bc = (uint32_t *)p;
    bc[0] = (uint32_t)bin_chunk;
    bc[1] = 0x004E4942;
    memcpy(p + 8, bin, bin_len);

    *out_len = total;
    return glb;
}

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_bytes != (size_t)len) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)len;
    return buf;
}

static bool on_start(lens *ui, flux_device *device, void *user) {
    (void)ui;
    gltf_viewer_app *app = user;

    flux_result r = flux_sg_load_glb(device, app->glb, app->glb_len, &app->scene);
    if (r != FLUX_OK) {
        fprintf(stderr, "flux_sg_load_glb failed: %d\n", (int)r);
        return false;
    }

    if (app->orbit) {
        flux_vec3 bmin, bmax;
        if (flux_sg_scene_bounds(app->scene, &bmin, &bmax)) {
            app->fr.center = flux_vec3_scale(flux_vec3_add(bmin, bmax), 0.5f);
            app->fr.half_diag = flux_vec3_length(flux_vec3_sub(bmax, bmin)) * 0.5f;
        }
    }

    flux_material_desc mdesc = {
        .type = FLUX_TYPE_MATERIAL_DESC,
        .kind = FLUX_MATERIAL_PHONG,
        .base_color =
            app->orbit ? (flux_vec4){1.0f, 1.0f, 1.0f, 1.0f} : (flux_vec4){0.8f, 0.6f, 0.3f, 1.0f},
        .color_format = FLUX_FORMAT_BGRA8_UNORM,
        .depth_format = FLUX_DEPTH_FORMAT,
        .shininess = app->orbit ? 32.0f : 48.0f,
        .specular = app->orbit ? 0.35f : 0.6f,
    };
    if (flux_material_create(device, &mdesc, &app->mat) != FLUX_OK) {
        flux_sg_scene_release(app->scene);
        return false;
    }

    return true;
}

static void on_stop(lens *ui, flux_device *device, void *user) {
    (void)ui;
    (void)device;
    gltf_viewer_app *app = user;
    if (app->depth)
        flux_target_release(app->depth);
    if (app->mat)
        flux_material_release(app->mat);
    if (app->scene)
        flux_sg_scene_release(app->scene);
}

static void on_build(lens *ui, const lens_input *in, void *user) {
    gltf_viewer_app *app = user;

    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == LENS_KEY_ESCAPE || key == 'q' || key == 'Q') {
            iris_window_close();
            return;
        } else if (key == ' ') {
            app->orbit = !app->orbit;
        }
    }
    iris_request_animation_frame();

    /* Lens UI overlay */
    lens_column_begin(ui, &(lens_layout_opts){.pad = 16, .gap = 6, .cross = LENS_START});
    lens_row_begin(ui, &(lens_layout_opts){.pad = 6, .gap = 10, .cross = LENS_CENTER});
    if (lens_button(ui,
                    &(lens_button_opts){
                        .label = app->orbit ? "Camera: Orbit" : "Camera: Fixed",
                        .variant = app->orbit ? LENS_BUTTON_PRIMARY : LENS_BUTTON_DEFAULT,
                    })
            .clicked) {
        app->orbit = !app->orbit;
    }
    lens_label(ui, &(lens_label_opts){.text = "[Space] Toggle Orbit | [Esc] Exit", .size = 13.0f});
    lens_close(ui);
    lens_close(ui);
}

static void on_prepare(flux_frame *frame, flux_canvas *canvas, flux_device *device, float scale,
                       void *user) {
    (void)canvas;
    gltf_viewer_app *app = user;
    app->time += 0.016f;

    int32_t log_w = 960, log_h = 540;
    iris_window_get_geometry(&log_w, &log_h);
    uint32_t pw = (uint32_t)lroundf((float)log_w * scale);
    uint32_t ph = (uint32_t)lroundf((float)log_h * scale);
    if (pw == 0 || ph == 0)
        return;

    app->depth = depth_ensure(app->depth, device, pw, ph);
    if (!app->depth)
        return;

    VkCommandBuffer cmd = flux_frame_vk_command_buffer(frame);
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
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
        .width = (float)pw,
        .height = (float)ph,
        .maxDepth = 1.0f,
    };
    VkRect2D sc = {.extent = {pw, ph}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    float aspect = (float)pw / (float)ph;
    flux_camera cam;
    if (app->orbit) {
        float dist = orbit_distance(&app->fr, aspect);
        float diag = app->fr.half_diag * 2.0f;
        float z_near = (app->fr.half_diag > 0.0f) ? app->fr.half_diag * 0.05f : 0.1f;
        float z_far = (dist + diag) * 5.0f + 1.0f;
        flux_camera_perspective(&cam, ORBIT_FOV_Y, aspect, z_near, z_far);

        float yaw = app->time * ORBIT_SPEED;
        float cp = cosf(ORBIT_PITCH);
        flux_vec3 eye = flux_vec3_make(app->fr.center.x + dist * cp * sinf(yaw),
                                       app->fr.center.y + dist * sinf(ORBIT_PITCH),
                                       app->fr.center.z + dist * cp * cosf(yaw));
        flux_camera_look_at(&cam, eye, app->fr.center, (flux_vec3){0, 1, 0});
    } else {
        flux_camera_perspective(&cam, 1.0f, aspect, 0.1f, 100.0f);
        flux_camera_look_at(&cam, (flux_vec3){3, 2.5f, 4}, (flux_vec3){0, 0, 0},
                            (flux_vec3){0, 1, 0});
    }

    flux_scene_light light = FLUX_SCENE_LIGHT_DEFAULT;
    light.direction =
        app->orbit ? (flux_vec3){-0.5f, -0.9f, -0.35f} : (flux_vec3){-0.6f, -1.0f, -0.4f};
    light.ambient = app->orbit ? 0.22f : 0.12f;

    flux_sg_draw_opts draw_opts = {.material = app->mat, .light = &light};
    flux_sg_draw(frame, &cam, app->scene, &draw_opts);

    flux_frame_end_pass(frame);
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

    gltf_viewer_app app = {
        .glb = glb,
        .glb_len = glb_len,
        .orbit = orbit,
    };

    printf("flux gltf_viewer — native Iris window (Esc to quit)\n");
    int rc = iris_app_run(&(iris_app_opts){
        .title = orbit ? "flux gltf viewer — orbit" : "flux gltf viewer",
        .app_id = "org.optics.flux.gltf-viewer",
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

    free(mem_glb);
    free(file_buf);
    return rc;
}
