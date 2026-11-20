/*
 * scene_studio/main.c — Flagship 3D glTF 2.0 Scene Studio showcase (ADR-0171).
 *
 * Combines optics_scene + optics_gfx (3D depth-buffered Phong pass) with a
 * floating optics_ui studio toolbar and live camera/lighting inspector.
 *
 * Usage: scene_studio [--file path/to/model.glb]
 */

#include "../common/design.h"

#include <optics/app.h>
#include <optics/gfx.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>
#include <optics/scene.h>
#include <optics/ui.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STUDIO_DEPTH_FORMAT OPTICS_GFX_FORMAT_D32_SFLOAT
#define STUDIO_VK_DEPTH_FORMAT VK_FORMAT_D32_SFLOAT

typedef struct studio_framing {
    optics_gfx_vec3 center;
    float half_diag;
} studio_framing;

typedef struct studio_app {
    const void *glb;
    size_t glb_len;
    const char *asset_name;

    bool orbit;
    bool show_inspector;
    float time;
    float orbit_speed;
    float pitch;
    float fov_deg;
    float ambient;

    optics_scene *scene;
    optics_gfx_material *mat;
    optics_gfx_target *depth;
    studio_framing fr;
} studio_app;

static float orbit_distance(const studio_framing *fr, float fov_y, float aspect) {
    float fov_x = 2.0f * atanf(tanf(fov_y * 0.5f) * aspect);
    float fov_min = (aspect >= 1.0f) ? fov_y : fov_x;
    return (fr->half_diag / sinf(fov_min * 0.5f)) * 1.35f;
}

static optics_gfx_target *depth_ensure(optics_gfx_target *t, optics_gfx_device *device, uint32_t w,
                                       uint32_t h) {
    if (t) {
        if (optics_gfx_target_width(t) == w && optics_gfx_target_height(t) == h)
            return t;
        optics_gfx_target_release(t);
    }
    optics_gfx_target_desc desc = {
        .type = OPTICS_GFX_TYPE_TARGET_DESC,
        .usage = OPTICS_GFX_TARGET_DEPTH,
        .format = STUDIO_DEPTH_FORMAT,
        .width = w,
        .height = h,
    };
    optics_gfx_target *nt = nullptr;
    (void)optics_gfx_target_create(device, &desc, &nt);
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
        return nullptr;

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
        return nullptr;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return nullptr;
    }
    uint8_t *buf = malloc((size_t)len);
    if (!buf) {
        fclose(f);
        return nullptr;
    }
    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read_bytes != (size_t)len) {
        free(buf);
        return nullptr;
    }
    *out_len = (size_t)len;
    return buf;
}

static bool on_start(optics_ui *ui, optics_gfx_device *device, void *user) {
    studio_app *app = user;
    ds_apply_showcase_theme(ui, true);

    optics_gfx_result r = optics_scene_load_glb(
        device, &(optics_scene_load_glb_opts){
                    .bytes = app->glb, .byte_count = app->glb_len, .out = &app->scene});
    if (r != OPTICS_GFX_OK) {
        fprintf(stderr, "scene_studio: optics_scene_load_glb failed: %d\n", (int)r);
        return false;
    }

    optics_gfx_vec3 bmin = {}, bmax = {};
    if (optics_scene_bounds(app->scene,
                            &(optics_scene_bounds_opts){.out_min = &bmin, .out_max = &bmax})) {
        app->fr.center = optics_gfx_vec3_scale(optics_gfx_vec3_add(bmin, bmax), 0.5f);
        app->fr.half_diag = optics_gfx_vec3_length(optics_gfx_vec3_sub(bmax, bmin)) * 0.5f;
    }
    if (app->fr.half_diag < 0.01f) {
        app->fr.half_diag = 1.732f;
    }

    optics_gfx_material_desc mdesc = {
        .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
        .kind = OPTICS_GFX_MATERIAL_PHONG,
        .base_color = {1.0f, 1.0f, 1.0f, 1.0f},
        .color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM,
        .depth_format = STUDIO_DEPTH_FORMAT,
        .shininess = 32.0f,
        .specular = 0.35f,
    };
    if (optics_gfx_material_create(device, &mdesc, &app->mat) != OPTICS_GFX_OK) {
        optics_scene_release(app->scene);
        return false;
    }

    return true;
}

static void on_stop(optics_ui *ui, optics_gfx_device *device, void *user) {
    (void)ui;
    (void)device;
    studio_app *app = user;
    if (app->depth)
        optics_gfx_target_release(app->depth);
    if (app->mat)
        optics_gfx_material_release(app->mat);
    if (app->scene)
        optics_scene_release(app->scene);
}

static void on_build(optics_ui *ui, const optics_ui_input *in, void *user) {
    studio_app *app = user;

    for (uint32_t k = 0; k < in->key_count; k++) {
        if (!in->keys[k].pressed)
            continue;
        int key = in->keys[k].key;
        if (key == OPTICS_UI_KEY_ESCAPE || key == 'q' || key == 'Q') {
            optics_app_quit();
            return;
        } else if (key == ' ') {
            app->orbit = !app->orbit;
        } else if (key == 'i' || key == 'I') {
            app->show_inspector = !app->show_inspector;
        }
    }
    if (app->orbit) {
        optics_app_request_frame();
    }

    const ds_palette p = ds_palette_for(true);
    optics_ui_style hud_st = OPTICS_UI_STYLE_INIT;
    hud_st.fields = OPTICS_UI_STYLE_BG | OPTICS_UI_STYLE_BORDER | OPTICS_UI_STYLE_BORDER_WIDTH |
                    OPTICS_UI_STYLE_CORNER_RADIUS;
    hud_st.bg = optics_gfx_color_rgba(20, 24, 34, 218);
    hud_st.border = optics_gfx_color_rgba(255, 255, 255, 32);
    hud_st.border_width = 1.0f;
    hud_st.corner_radius = 12.0f;

    optics_ui_column_begin(
        ui,
        &(optics_ui_layout_opts){.pad = DS_SPACE_LG, .gap = DS_SPACE_MD, .cross = OPTICS_UI_START});

    /* Top floating studio bar */
    optics_ui_row_begin(ui, &(optics_ui_layout_opts){
                                .pad = DS_SPACE_MD,
                                .gap = DS_SPACE_MD,
                                .cross = OPTICS_UI_CENTER,
                                .box = {.style = hud_st},
                            });
    optics_ui_label(ui, &(optics_ui_label_opts){
                            .text = "Scene Studio",
                            .size = 15.0f,
                            .weight = 650.0f,
                        });
    ds_badge(ui, &p, app->asset_name);

    if (optics_ui_button(
            ui,
            &(optics_ui_button_opts){
                .label = app->orbit ? "Pause Orbit" : "Resume Orbit",
                .variant = app->orbit ? OPTICS_UI_BUTTON_PRIMARY : OPTICS_UI_BUTTON_SUBTLE,
            })
            .clicked) {
        app->orbit = !app->orbit;
    }
    if (optics_ui_button(ui,
                         &(optics_ui_button_opts){
                             .label = app->show_inspector ? "Hide Inspector" : "Inspector",
                             .variant = OPTICS_UI_BUTTON_SUBTLE,
                         })
            .clicked) {
        app->show_inspector = !app->show_inspector;
    }
    optics_ui_close(ui);

    /* Floating camera & lighting inspector */
    if (app->show_inspector) {
        optics_ui_column_begin(ui, &(optics_ui_layout_opts){
                                       .pad = DS_SPACE_LG,
                                       .gap = DS_SPACE_MD,
                                       .cross = OPTICS_UI_STRETCH,
                                       .box = {.width = 292.0f, .style = hud_st},
                                   });
        optics_ui_label(ui,
                        &(optics_ui_label_opts){
                            .text = "CAMERA & LIGHTING",
                            .size = 11.5f,
                            .weight = 650.0f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p.text_muted}},
                        });

        char spd_s[24], pit_s[24], fov_s[24], amb_s[24];
        snprintf(spd_s, sizeof spd_s, "%.2f", app->orbit_speed);
        snprintf(pit_s, sizeof pit_s, "%.2f", app->pitch);
        snprintf(fov_s, sizeof fov_s, "%.0f deg", app->fov_deg);
        snprintf(amb_s, sizeof amb_s, "%.2f", app->ambient);

        optics_ui_row_begin(
            ui, &(optics_ui_layout_opts){.gap = DS_SPACE_SM, .cross = OPTICS_UI_CENTER});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Speed", .box = {.width = 64.0f}});
        optics_ui_flex(ui, 1.0f);
        optics_ui_slider(ui, &(optics_ui_slider_opts){
                                 .box = {.id = "ss_speed"},
                                 .value = &app->orbit_speed,
                                 .min = 0.0f,
                                 .max = 2.0f,
                             });
        optics_ui_label(ui, &(optics_ui_label_opts){.text = spd_s, .box = {.width = 48.0f}});
        optics_ui_close(ui);

        optics_ui_row_begin(
            ui, &(optics_ui_layout_opts){.gap = DS_SPACE_SM, .cross = OPTICS_UI_CENTER});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Pitch", .box = {.width = 64.0f}});
        optics_ui_flex(ui, 1.0f);
        optics_ui_slider(ui, &(optics_ui_slider_opts){
                                 .box = {.id = "ss_pitch"},
                                 .value = &app->pitch,
                                 .min = -0.8f,
                                 .max = 1.1f,
                             });
        optics_ui_label(ui, &(optics_ui_label_opts){.text = pit_s, .box = {.width = 48.0f}});
        optics_ui_close(ui);

        optics_ui_row_begin(
            ui, &(optics_ui_layout_opts){.gap = DS_SPACE_SM, .cross = OPTICS_UI_CENTER});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "FOV", .box = {.width = 64.0f}});
        optics_ui_flex(ui, 1.0f);
        optics_ui_slider(ui, &(optics_ui_slider_opts){
                                 .box = {.id = "ss_fov"},
                                 .value = &app->fov_deg,
                                 .min = 30.0f,
                                 .max = 90.0f,
                             });
        optics_ui_label(ui, &(optics_ui_label_opts){.text = fov_s, .box = {.width = 48.0f}});
        optics_ui_close(ui);

        optics_ui_row_begin(
            ui, &(optics_ui_layout_opts){.gap = DS_SPACE_SM, .cross = OPTICS_UI_CENTER});
        optics_ui_label(ui, &(optics_ui_label_opts){.text = "Ambient", .box = {.width = 64.0f}});
        optics_ui_flex(ui, 1.0f);
        optics_ui_slider(ui, &(optics_ui_slider_opts){
                                 .box = {.id = "ss_amb"},
                                 .value = &app->ambient,
                                 .min = 0.05f,
                                 .max = 0.65f,
                             });
        optics_ui_label(ui, &(optics_ui_label_opts){.text = amb_s, .box = {.width = 48.0f}});
        optics_ui_close(ui);

        optics_ui_label(ui,
                        &(optics_ui_label_opts){
                            .text = "[Space] Orbit  [I] Inspector  [Esc] Quit",
                            .size = 11.5f,
                            .box = {.style = {.fields = OPTICS_UI_STYLE_FG, .fg = p.text_muted}},
                        });
        optics_ui_close(ui);
    }

    optics_ui_close(ui);
}

static void on_prepare(optics_gfx_frame *frame, optics_gfx_canvas *canvas,
                       optics_gfx_device *device, float scale, void *user) {
    (void)canvas;
    studio_app *app = user;
    if (app->orbit) {
        app->time += 0.016f * app->orbit_speed;
    }

    int32_t log_w = 1120, log_h = 680;
    optics_app_get_geometry(&log_w, &log_h);
    uint32_t pw = (uint32_t)lroundf((float)log_w * scale);
    uint32_t ph = (uint32_t)lroundf((float)log_h * scale);
    if (pw == 0 || ph == 0)
        return;

    app->depth = depth_ensure(app->depth, device, pw, ph);
    if (!app->depth)
        return;

    VkCommandBuffer cmd = optics_gfx_frame_vk_command_buffer(frame);
    VkImageMemoryBarrier2 b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
        .dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .image = optics_gfx_target_vk_image(app->depth),
        .subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                             .levelCount = 1,
                             .layerCount = 1},
    };
    VkDependencyInfo di = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &b,
    };
    vkCmdPipelineBarrier2(cmd, &di);

    optics_gfx_pass_attachment color = {
        .view = VK_NULL_HANDLE,
        .load_op = OPTICS_GFX_LOAD_CLEAR,
        .store_op = OPTICS_GFX_STORE_STORE,
        .clear_color = {0.06f, 0.07f, 0.10f, 1.0f},
    };
    optics_gfx_pass_depth_attachment depth_att = {
        .view = optics_gfx_target_vk_view(app->depth),
        .format = STUDIO_VK_DEPTH_FORMAT,
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

    VkViewport vp = {.width = (float)pw, .height = (float)ph, .maxDepth = 1.0f};
    VkRect2D sc = {.extent = {pw, ph}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    float aspect = (float)pw / (float)ph;
    float fov_rad = app->fov_deg * (3.14159265f / 180.0f);
    float dist = orbit_distance(&app->fr, fov_rad, aspect);
    float diag = app->fr.half_diag * 2.0f;
    float z_near = app->fr.half_diag * 0.05f;
    float z_far = (dist + diag) * 5.0f + 1.0f;

    optics_gfx_camera cam;
    optics_gfx_camera_perspective(
        &cam, &(optics_gfx_camera_perspective_opts){
                  .fov_y_rad = fov_rad, .aspect = aspect, .z_near = z_near, .z_far = z_far});

    float yaw = app->time;
    float cp = cosf(app->pitch);
    optics_gfx_vec3 eye = optics_gfx_vec3_make(app->fr.center.x + dist * cp * sinf(yaw),
                                               app->fr.center.y + dist * sinf(app->pitch),
                                               app->fr.center.z + dist * cp * cosf(yaw));
    optics_gfx_camera_look_at(&cam, eye, app->fr.center, (optics_gfx_vec3){0, 1, 0});

    optics_gfx_scene_light light = OPTICS_GFX_SCENE_LIGHT_DEFAULT;
    light.direction = (optics_gfx_vec3){-0.5f, -0.9f, -0.35f};
    light.ambient = app->ambient;

    optics_scene_draw(frame, &(optics_scene_draw_opts){
                                 .camera = &cam,
                                 .scene = app->scene,
                                 .material = app->mat,
                                 .light = &light,
                             });

    optics_gfx_frame_end_pass(frame);
}

int main(int argc, char **argv) {
    const char *file_path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--file") == 0 && i + 1 < argc)
            file_path = argv[++i];
    }

    uint8_t *file_buf = nullptr;
    uint8_t *mem_glb = nullptr;
    const void *glb = nullptr;
    size_t glb_len = 0;
    const char *asset_name = "Duck.glb";

    if (file_path) {
        file_buf = read_file(file_path, &glb_len);
        if (!file_buf) {
            fprintf(stderr, "scene_studio: cannot open %s\n", file_path);
            return 1;
        }
        glb = file_buf;
        asset_name = file_path;
    } else {
        file_buf = read_file(OPTICS_STUDIO_DUCK_ASSET, &glb_len);
        if (file_buf) {
            glb = file_buf;
        } else {
            mem_glb = build_cube_glb(&glb_len);
            glb = mem_glb;
            asset_name = "Procedural Cube";
        }
    }

    studio_app app = {
        .glb = glb,
        .glb_len = glb_len,
        .asset_name = asset_name,
        .orbit = true,
        .show_inspector = true,
        .orbit_speed = 0.65f,
        .pitch = 0.32f,
        .fov_deg = 52.0f,
        .ambient = 0.24f,
    };

    int rc = optics_app_run(&(optics_app_opts){
        .title = "Optics — Scene Studio",
        .app_id = "org.optics.showcase.scene-studio",
        .width = 1120,
        .height = 680,
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
