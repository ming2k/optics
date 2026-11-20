/*
 * Scene GPU rendering: draw meshes into an offscreen surface
 * (ADR-0013) and assert the read-back pixels.
 *
 * Covers two things no other test reaches:
 *
 *   1. Depth testing through the public seam. A near quad is drawn
 *      BEFORE a far quad; without a working depth attachment the far
 *      quad would paint over it (regression for flux_frame_begin_pass
 *      dropping flux_pass_desc.depth on the floor).
 *   2. The Blinn-Phong lit path end-to-end. A head-on directional
 *      light must brighten the quad; a light travelling away from it
 *      must leave only the ambient term.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define OPTICS_GFX_DEPTH_FORMAT OPTICS_GFX_FORMAT_D32_SFLOAT

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

/* Quad in the XY plane at `z`, normal +Z, wound CCW viewed from +Z
 * (front-facing for a camera on the +Z axis, matching the scene
 * pipeline's CCW front face + back-face culling). */
static optics_gfx_mesh *make_quad(optics_gfx_device *d, float half, float z) {
    optics_gfx_vertex verts[4] = {
        {{-half, -half, z}, {0, 0, 1}, {0, 0}},
        {{half, -half, z}, {0, 0, 1}, {1, 0}},
        {{half, half, z}, {0, 0, 1}, {1, 1}},
        {{-half, half, z}, {0, 0, 1}, {0, 1}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    optics_gfx_mesh_desc md = {
        .type = OPTICS_GFX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    optics_gfx_mesh *m = nullptr;
    if (optics_gfx_mesh_create(d, &md, &m) != OPTICS_GFX_OK)
        return nullptr;
    return m;
}

static optics_gfx_mesh *make_skinned_quad(optics_gfx_device *d, float half) {
    optics_gfx_vertex verts[4] = {
        {{-half, -half, 0}, {0, 0, 1}, {0, 0}},
        {{half, -half, 0}, {0, 0, 1}, {1, 0}},
        {{half, half, 0}, {0, 0, 1}, {1, 1}},
        {{-half, half, 0}, {0, 0, 1}, {0, 1}},
    };
    optics_gfx_skin_vertex skin_verts[4] = {
        {.joints = {}, .weights = {1, 0, 0, 0}},
        {.joints = {}, .weights = {1, 0, 0, 0}},
        {.joints = {}, .weights = {1, 0, 0, 0}},
        {.joints = {}, .weights = {1, 0, 0, 0}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    optics_gfx_mesh_skin_desc sd = {
        .type = OPTICS_GFX_TYPE_MESH_SKIN_DESC,
        .vertices = skin_verts,
    };
    optics_gfx_mesh_desc md = {
        .type = OPTICS_GFX_TYPE_MESH_DESC,
        .next = &sd,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    optics_gfx_mesh *mesh = nullptr;
    return optics_gfx_mesh_create(d, &md, &mesh) == OPTICS_GFX_OK ? mesh : nullptr;
}

/* Shared per-frame plumbing: begin frame, prepare a flux-owned depth target,
 * begin a pass with a dark clear + depth clear, set viewport/scissor through
 * the backend-neutral helpers, run `draw`, end, submit, present. */
typedef void (*draw_fn)(optics_gfx_frame *f, const optics_gfx_camera *cam, void *user);

static optics_gfx_result render_frame(optics_gfx_surface *s, optics_gfx_target *depth,
                                      const optics_gfx_camera *cam, draw_fn draw, void *user) {
    optics_gfx_frame *frame = nullptr;
    optics_gfx_result r = optics_gfx_surface_begin_frame(s, nullptr, &frame);
    if (r != OPTICS_GFX_OK)
        return r;

    optics_gfx_frame_prepare_target(frame, depth);

    optics_gfx_pass_attachment color = {
        .load_op = OPTICS_GFX_LOAD_CLEAR,
        .store_op = OPTICS_GFX_STORE_STORE,
        .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
    };
    optics_gfx_pass_depth_attachment depth_att = {
        .view = optics_gfx_target_vk_view(depth),
        .format = DEPTH_FORMAT,
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

    optics_gfx_frame_set_viewport(frame, &(optics_gfx_frame_set_viewport_opts){.x = 0.0f,
                                                                               .y = 0.0f,
                                                                               .width = (float)W,
                                                                               .height = (float)H,
                                                                               .min_depth = 0.0f,
                                                                               .max_depth = 1.0f});
    optics_gfx_frame_set_scissor(
        frame, &(optics_gfx_frame_set_scissor_opts){.x = 0, .y = 0, .width = W, .height = H});

    draw(frame, cam, user);

    optics_gfx_frame_end_pass(frame);
    r = optics_gfx_frame_submit(frame);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_frame_present(frame);
}

/* --- draw callbacks ------------------------------------------------ */

typedef struct depth_case {
    optics_gfx_mesh *near_quad, *far_quad;
    optics_gfx_material *red, *green;
} depth_case;

static void draw_depth_case(optics_gfx_frame *f, const optics_gfx_camera *cam, void *user) {
    depth_case *c = user;
    optics_gfx_mat4 id = optics_gfx_mat4_identity();
    /* Near first. If depth testing is broken the far quad wins. */
    optics_gfx_scene_draw_mesh(
        f, &(optics_gfx_scene_draw_mesh_opts){
               .camera = cam, .world = id, .mesh = c->near_quad, .material = c->red});
    optics_gfx_scene_draw_mesh(
        f, &(optics_gfx_scene_draw_mesh_opts){
               .camera = cam, .world = id, .mesh = c->far_quad, .material = c->green});
}

typedef struct phong_case {
    optics_gfx_mesh *quad;
    optics_gfx_material *mat;
    optics_gfx_scene_light light;
} phong_case;

static void draw_phong_case(optics_gfx_frame *f, const optics_gfx_camera *cam, void *user) {
    phong_case *c = user;
    optics_gfx_scene_draw_mesh(
        f, &(optics_gfx_scene_draw_mesh_opts){.camera = cam,
                                              .world = optics_gfx_mat4_identity(),
                                              .mesh = c->quad,
                                              .material = c->mat,
                                              .light = &c->light});
}

typedef struct skin_case {
    optics_gfx_mesh *quad;
    optics_gfx_material *material;
    optics_gfx_mat4 palette[1];
} skin_case;

typedef struct texture_case {
    optics_gfx_mesh *quad;
    optics_gfx_material *material;
} texture_case;

static void draw_skin_case(optics_gfx_frame *f, const optics_gfx_camera *cam, void *user) {
    skin_case *c = user;
    optics_gfx_scene_draw_mesh(
        f, &(optics_gfx_scene_draw_mesh_opts){.camera = cam,
                                              .world = optics_gfx_mat4_identity(),
                                              .mesh = c->quad,
                                              .material = c->material,
                                              .joint_matrices = c->palette,
                                              .joint_count = 1});
}

static void draw_texture_case(optics_gfx_frame *f, const optics_gfx_camera *cam, void *user) {
    texture_case *c = user;
    optics_gfx_scene_draw_mesh(
        f, &(optics_gfx_scene_draw_mesh_opts){.camera = cam,
                                              .world = optics_gfx_mat4_identity(),
                                              .mesh = c->quad,
                                              .material = c->material});
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_render: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface *s = nullptr;
    {
        optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    }

    optics_gfx_target_desc depth_desc = {
        .type = OPTICS_GFX_TYPE_TARGET_DESC,
        .usage = OPTICS_GFX_TARGET_DEPTH,
        .format = OPTICS_GFX_DEPTH_FORMAT,
        .width = W,
        .height = H,
    };
    optics_gfx_target *depth = nullptr;
    OT_EXPECT(optics_gfx_target_create(d, &depth_desc, &depth) == OPTICS_GFX_OK);

    optics_gfx_camera cam;
    optics_gfx_camera_perspective(
        &cam,
        &(optics_gfx_camera_perspective_opts){
            .fov_y_rad = 1.0f, .aspect = (float)W / (float)H, .z_near = 0.1f, .z_far = 100.0f});
    optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0, 0, 4}, (optics_gfx_vec3){0, 0, 0},
                              (optics_gfx_vec3){0, 1, 0});

    optics_gfx_format color_fmt = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(s));
    static uint8_t px[BYTES];

    /* --- depth test: near (red) drawn first survives far (green) --- */
    {
        depth_case c = {};
        c.near_quad = make_quad(d, 0.8f, 1.0f); /* 3 units from the eye */
        c.far_quad = make_quad(d, 2.5f, -1.0f); /* 5 units, projects larger */
        OT_EXPECT(c.near_quad != nullptr && c.far_quad != nullptr);

        optics_gfx_material_desc md = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {1, 0, 0, 1},
            .color_format = color_fmt,
            .depth_format = OPTICS_GFX_DEPTH_FORMAT,
        };
        OT_EXPECT(optics_gfx_material_create(d, &md, &c.red) == OPTICS_GFX_OK);
        md.base_color = (optics_gfx_vec4){0, 1, 0, 1};
        OT_EXPECT(optics_gfx_material_create(d, &md, &c.green) == OPTICS_GFX_OK);

        OT_EXPECT(render_frame(s, depth, &cam, draw_depth_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);

        /* Centre: the near red quad, despite being drawn first. */
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        OT_EXPECT(centre[0] > 200 && centre[1] < 50);

        /* Off-centre along x: outside the near quad's projection
         * (extends to ~0.49 of the half-screen) but inside the far
         * quad's (~0.91) — the green quad shows. Symmetric in x, so
         * no assumption about axis orientation. */
        const uint8_t *ring = px_at(px, W / 2 + (uint32_t)(0.65f * W / 2), H / 2);
        OT_EXPECT(ring[1] > 200 && ring[0] < 50);

        /* Corner: beyond both quads — the clear colour. */
        const uint8_t *corner = px_at(px, 1, 1);
        OT_EXPECT(corner[0] < 20 && corner[1] < 20 && corner[2] < 20);

        optics_gfx_material_release(c.red);
        optics_gfx_material_release(c.green);
        optics_gfx_mesh_release(c.near_quad);
        optics_gfx_mesh_release(c.far_quad);
    }

    /* --- Phong: head-on light brightens, departing light leaves ambient --- */
    {
        phong_case c = {};
        c.quad = make_quad(d, 1.5f, 0.0f);
        OT_EXPECT(c.quad != nullptr);

        optics_gfx_material_desc md = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_PHONG,
            .base_color = {1, 1, 1, 1},
            .color_format = color_fmt,
            .depth_format = OPTICS_GFX_DEPTH_FORMAT,
            .shininess = 32.0f,
            .specular = 0.0f, /* diffuse only: predictable bytes */
        };
        OT_EXPECT(optics_gfx_material_create(d, &md, &c.mat) == OPTICS_GFX_OK);

        /* Light travels -Z, straight at the +Z-facing quad: ndotl = 1,
         * so the centre is ambient + full diffuse ≈ saturated white. */
        c.light = (optics_gfx_scene_light){
            .direction = {0, 0, -1},
            .color = {1, 1, 1},
            .ambient = 0.1f,
        };
        OT_EXPECT(render_frame(s, depth, &cam, draw_phong_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        /* Copy out of px — the next readback overwrites it. */
        uint8_t lit[3];
        memcpy(lit, px_at(px, W / 2, H / 2), 3);
        OT_EXPECT(lit[0] > 200 && lit[1] > 200 && lit[2] > 200);

        /* Light travels +Z, away from the quad: ndotl = 0, ambient
         * only (0.1 → ~26). The shading must respond to direction. */
        c.light.direction = (optics_gfx_vec3){0, 0, 1};
        OT_EXPECT(render_frame(s, depth, &cam, draw_phong_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *dark = px_at(px, W / 2, H / 2);
        OT_EXPECT(dark[0] > 5 && dark[0] < 80);
        OT_EXPECT(dark[0] < lit[0] / 2);

        optics_gfx_material_release(c.mat);
        optics_gfx_mesh_release(c.quad);
    }

    /* --- GPU skinning: a one-joint palette translates the bind-pose quad. --- */
    {
        skin_case c = {};
        c.quad = make_skinned_quad(d, 0.4f);
        c.palette[0] = optics_gfx_mat4_translate(1.0f, 0.0f, 0.0f);
        OT_EXPECT(c.quad != nullptr);
        optics_gfx_material_desc md = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {1, 0, 0, 1},
            .color_format = color_fmt,
            .depth_format = OPTICS_GFX_DEPTH_FORMAT,
        };
        OT_EXPECT(optics_gfx_material_create(d, &md, &c.material) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame(s, depth, &cam, draw_skin_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *old_centre = px_at(px, W / 2, H / 2);
        const uint8_t *translated = px_at(px, W / 2 + 15, H / 2);
        OT_EXPECT(old_centre[0] < 20);
        OT_EXPECT(translated[0] > 200 && translated[1] < 30);
        optics_gfx_material_release(c.material);
        optics_gfx_mesh_release(c.quad);
    }

    /* --- Textured UNLIT: sRGB sample, UV transform, OPAQUE/MASK/BLEND. --- */
    {
        texture_case c = {.quad = make_quad(d, 1.5f, 0.0f)};
        /* First texel is half-alpha red; second is transparent green. */
        const uint8_t texels[8] = {255, 0, 0, 128, 0, 255, 0, 0};
        optics_gfx_image_desc image_desc = OPTICS_GFX_IMAGE_DESC_INIT;
        image_desc.width = 2;
        image_desc.height = 1;
        image_desc.format = OPTICS_GFX_FORMAT_RGBA8_SRGB;
        image_desc.initial_data = texels;
        optics_gfx_image *image = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &image_desc, &image) == OPTICS_GFX_OK);

        optics_gfx_sampler_desc sampler_desc = OPTICS_GFX_SAMPLER_DESC_INIT;
        sampler_desc.min_filter = OPTICS_GFX_FILTER_NEAREST;
        sampler_desc.mag_filter = OPTICS_GFX_FILTER_NEAREST;
        sampler_desc.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        optics_gfx_sampler *sampler = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sampler_desc, &sampler) == OPTICS_GFX_OK);

        optics_gfx_material_surface_desc surface = OPTICS_GFX_MATERIAL_SURFACE_DESC_INIT;
        surface.base_color_image = image;
        surface.base_color_sampler = sampler;
        surface.uv_scale = (optics_gfx_vec2){0, 0};
        surface.uv_offset = (optics_gfx_vec2){0.25f, 0.5f};
        optics_gfx_material_desc material_desc = {
            .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
            .next = &surface,
            .kind = OPTICS_GFX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = color_fmt,
            .depth_format = OPTICS_GFX_DEPTH_FORMAT,
        };

        /* OPAQUE ignores sampled alpha: the red texel is fully visible. */
        OT_EXPECT(optics_gfx_material_create(d, &material_desc, &c.material) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *opaque = px_at(px, W / 2, H / 2);
        OT_EXPECT(opaque[0] > 220 && opaque[1] < 30 && opaque[2] < 30);
        optics_gfx_material_release(c.material);

        /* UV offset selects the transparent green texel; MASK discards it. */
        surface.alpha_mode = OPTICS_GFX_MATERIAL_ALPHA_MASK;
        surface.alpha_cutoff = 0.5f;
        surface.uv_offset = (optics_gfx_vec2){0.75f, 0.5f};
        OT_EXPECT(optics_gfx_material_create(d, &material_desc, &c.material) == OPTICS_GFX_OK);
        OT_EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *masked = px_at(px, W / 2, H / 2);
        OT_EXPECT(masked[0] < 20 && masked[1] < 20 && masked[2] < 20);
        optics_gfx_material_release(c.material);

        /* BLEND uses straight source alpha and leaves depth writes disabled. */
        surface.alpha_mode = OPTICS_GFX_MATERIAL_ALPHA_BLEND;
        surface.uv_offset = (optics_gfx_vec2){0.25f, 0.5f};
        surface.double_sided = true;
        OT_EXPECT(optics_gfx_material_create(d, &material_desc, &c.material) == OPTICS_GFX_OK);
        /* Material owns retained references after successful creation. */
        optics_gfx_sampler_release(sampler);
        optics_gfx_image_release(image);
        OT_EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == OPTICS_GFX_OK);
        memset(px, 0xCD, BYTES);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        const uint8_t *blended = px_at(px, W / 2, H / 2);
        OT_EXPECT(blended[0] > 100 && blended[0] < 160);
        OT_EXPECT(blended[1] < 30 && blended[2] < 30);

        optics_gfx_material_release(c.material);
        optics_gfx_mesh_release(c.quad);
    }

    /* --- Working-space scene rendering: 16F target decodes texels at the
     * edge, so lighting runs in linear light (ADR-0069) --- */
    {
        /* Phong with ndotl = 0.5 over a mid-gray sRGB texture. Linear
         * light: srgb_decode(0.502) x 0.5 = 0.108 -> sRGB ~92. The legacy
         * gamma-space path computes 0.502 x 0.5 -> 64. */
        const uint8_t gray[4] = {128, 128, 128, 255};
        optics_gfx_image_desc image_desc = OPTICS_GFX_IMAGE_DESC_INIT;
        image_desc.width = 1;
        image_desc.height = 1;
        image_desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        image_desc.initial_data = gray;
        optics_gfx_image *tex = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &image_desc, &tex) == OPTICS_GFX_OK);

        optics_gfx_material_surface_desc surface = OPTICS_GFX_MATERIAL_SURFACE_DESC_INIT;
        surface.base_color_image = tex;
        surface.base_color_sampler = nullptr; /* device default */
        optics_gfx_scene_light light = {
            .direction = {0.0f, 0.8660254f, -0.5f}, /* L . N = 0.5 */
            .color = {1, 1, 1},
            .ambient = 0.0f,
        };

        const float want_linear =
            optics_gfx_transfer_decode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, 128.0f / 255.0f) *
            0.5f; /* ndotl */
        const int want16f = (int)lrintf(
            optics_gfx_transfer_encode(OPTICS_GFX_TRANSFER_SRGB, 0.0f, want_linear) * 255.0f);

        int got[2] = {0, 0};
        for (int variant = 0; variant < 2; ++variant) {
            /* variant 0: legacy 8-bit target (raw gamma path, unchanged);
             * variant 1: 16F working-space target (edge decode). */
            optics_gfx_format tfmt = variant == 0 ? color_fmt : OPTICS_GFX_FORMAT_RGBA16_SFLOAT;
            optics_gfx_image *rt = nullptr;
            OT_EXPECT(optics_gfx_image_create(
                          d,
                          &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                                   .width = W,
                                                   .height = H,
                                                   .format = tfmt,
                                                   .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
                          &rt) == OPTICS_GFX_OK);

            optics_gfx_material_desc md = {
                .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
                .next = &surface,
                .kind = OPTICS_GFX_MATERIAL_PHONG,
                .base_color = {1, 1, 1, 1},
                .color_format = tfmt,
                .depth_format = OPTICS_GFX_DEPTH_FORMAT,
                .shininess = 32.0f,
                .specular = 0.0f,
            };
            optics_gfx_material *mat = nullptr;
            OT_EXPECT(optics_gfx_material_create(d, &md, &mat) == OPTICS_GFX_OK);
            optics_gfx_mesh *quad = make_quad(d, 1.5f, 0.0f);
            OT_EXPECT(quad != nullptr);

            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_prepare_image_target(frame, rt) == OPTICS_GFX_OK);
            optics_gfx_pass_attachment color_att = {
                .view = optics_gfx_image_vk_image_view(rt),
                .format = optics_gfx_format_to_vk(tfmt),
                .load_op = OPTICS_GFX_LOAD_CLEAR,
                .store_op = OPTICS_GFX_STORE_STORE,
                .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
            };
            optics_gfx_pass_depth_attachment depth_att = {
                .view = optics_gfx_target_vk_view(depth),
                .format = DEPTH_FORMAT,
                .load_op = OPTICS_GFX_LOAD_CLEAR,
                .store_op = OPTICS_GFX_STORE_DONT_CARE,
                .clear_depth = 1.0f,
            };
            optics_gfx_pass_desc pass = {
                .type = OPTICS_GFX_TYPE_PASS_DESC,
                .color_attachment_count = 1,
                .color_attachments = &color_att,
                .depth = &depth_att,
                .width = W,
                .height = H,
            };
            optics_gfx_frame_begin_pass(frame, &pass);
            optics_gfx_frame_set_viewport(frame,
                                          &(optics_gfx_frame_set_viewport_opts){.x = 0.0f,
                                                                                .y = 0.0f,
                                                                                .width = (float)W,
                                                                                .height = (float)H,
                                                                                .min_depth = 0.0f,
                                                                                .max_depth = 1.0f});
            optics_gfx_frame_set_scissor(frame, &(optics_gfx_frame_set_scissor_opts){
                                                    .x = 0, .y = 0, .width = W, .height = H});
            optics_gfx_scene_draw_mesh(
                frame, &(optics_gfx_scene_draw_mesh_opts){.camera = &cam,
                                                          .world = optics_gfx_mat4_identity(),
                                                          .mesh = quad,
                                                          .material = mat,
                                                          .light = &light});
            optics_gfx_frame_end_pass(frame);
            OT_EXPECT(optics_gfx_frame_finish_image_target(frame, rt) == OPTICS_GFX_OK);

            /* Composite the target onto the sRGB surface through the
             * canvas (16F images are working-space linear; 8-bit UNORM
             * decodes as sRGB — the round trip is identity). */
            optics_gfx_canvas *canvas = nullptr;
            optics_gfx_canvas_desc cd = OPTICS_GFX_CANVAS_DESC_INIT;
            cd.surface = s;
            OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);
            optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &black}) == OPTICS_GFX_OK);
            optics_gfx_canvas_draw_image(canvas, rt, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                         nullptr);
            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

            memset(px, 0xCD, BYTES);
            OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
            got[variant] = px_at(px, W / 2, H / 2)[0];

            optics_gfx_canvas_release(canvas);
            optics_gfx_mesh_release(quad);
            optics_gfx_material_release(mat);
            optics_gfx_image_release(rt);
        }

        OT_EXPECT(got[0] > 58 && got[0] < 70); /* legacy gamma path */
        OT_EXPECT(got[1] > want16f - 5 && got[1] < want16f + 5);
        OT_EXPECT(got[1] > got[0] + 15); /* linear-light lift is unmistakable */

        optics_gfx_image_release(tex);
    }

    optics_gfx_device_wait_idle(d);
    optics_gfx_target_release(depth);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
