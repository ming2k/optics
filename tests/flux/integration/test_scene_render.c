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
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/scene.h>
#include <flux/vulkan.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)
#define DEPTH_FORMAT VK_FORMAT_D32_SFLOAT
#define FLUX_DEPTH_FORMAT FLUX_FORMAT_D32_SFLOAT

static const uint8_t *px_at(const uint8_t *px, uint32_t x, uint32_t y) {
    return px + (y * W + x) * 4u;
}

/* Quad in the XY plane at `z`, normal +Z, wound CCW viewed from +Z
 * (front-facing for a camera on the +Z axis, matching the scene
 * pipeline's CCW front face + back-face culling). */
static flux_mesh *make_quad(flux_device *d, float half, float z) {
    flux_vertex verts[4] = {
        {{-half, -half, z}, {0, 0, 1}, {0, 0}},
        {{half, -half, z}, {0, 0, 1}, {1, 0}},
        {{half, half, z}, {0, 0, 1}, {1, 1}},
        {{-half, half, z}, {0, 0, 1}, {0, 1}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    flux_mesh_desc md = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    flux_mesh *m = nullptr;
    if (flux_mesh_create(d, &md, &m) != FLUX_OK)
        return nullptr;
    return m;
}

static flux_mesh *make_skinned_quad(flux_device *d, float half) {
    flux_vertex verts[4] = {
        {{-half, -half, 0}, {0, 0, 1}, {0, 0}},
        {{half, -half, 0}, {0, 0, 1}, {1, 0}},
        {{half, half, 0}, {0, 0, 1}, {1, 1}},
        {{-half, half, 0}, {0, 0, 1}, {0, 1}},
    };
    flux_skin_vertex skin_verts[4] = {
        {.joints = {0}, .weights = {1, 0, 0, 0}},
        {.joints = {0}, .weights = {1, 0, 0, 0}},
        {.joints = {0}, .weights = {1, 0, 0, 0}},
        {.joints = {0}, .weights = {1, 0, 0, 0}},
    };
    uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    flux_mesh_skin_desc sd = {
        .type = FLUX_TYPE_MESH_SKIN_DESC,
        .vertices = skin_verts,
    };
    flux_mesh_desc md = {
        .type = FLUX_TYPE_MESH_DESC,
        .next = &sd,
        .vertices = verts,
        .vertex_count = 4,
        .indices = idx,
        .index_count = 6,
    };
    flux_mesh *mesh = nullptr;
    return flux_mesh_create(d, &md, &mesh) == FLUX_OK ? mesh : nullptr;
}

/* Shared per-frame plumbing: begin frame, prepare a flux-owned depth target,
 * begin a pass with a dark clear + depth clear, set viewport/scissor through
 * the backend-neutral helpers, run `draw`, end, submit, present. */
typedef void (*draw_fn)(flux_frame *f, const flux_camera *cam, void *user);

static flux_result render_frame(flux_surface *s, flux_target *depth, const flux_camera *cam,
                                draw_fn draw, void *user) {
    flux_frame *frame = nullptr;
    flux_result r = flux_surface_begin_frame(s, nullptr, &frame);
    if (r != FLUX_OK)
        return r;

    flux_frame_prepare_target(frame, depth);

    flux_pass_attachment color = {
        .load_op = FLUX_LOAD_CLEAR,
        .store_op = FLUX_STORE_STORE,
        .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
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

    flux_frame_set_viewport(frame, 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f);
    flux_frame_set_scissor(frame, 0, 0, W, H);

    draw(frame, cam, user);

    flux_frame_end_pass(frame);
    r = flux_frame_submit(frame);
    if (r != FLUX_OK)
        return r;
    return flux_frame_present(frame);
}

/* --- draw callbacks ------------------------------------------------ */

typedef struct depth_case {
    flux_mesh *near_quad, *far_quad;
    flux_material *red, *green;
} depth_case;

static void draw_depth_case(flux_frame *f, const flux_camera *cam, void *user) {
    depth_case *c = user;
    flux_mat4 id = flux_mat4_identity();
    /* Near first. If depth testing is broken the far quad wins. */
    flux_scene_draw_mesh(f, cam, id, c->near_quad, c->red);
    flux_scene_draw_mesh(f, cam, id, c->far_quad, c->green);
}

typedef struct phong_case {
    flux_mesh *quad;
    flux_material *mat;
    flux_scene_light light;
} phong_case;

static void draw_phong_case(flux_frame *f, const flux_camera *cam, void *user) {
    phong_case *c = user;
    flux_scene_draw_mesh_lit(f, cam, flux_mat4_identity(), c->quad, c->mat, &c->light);
}

typedef struct skin_case {
    flux_mesh *quad;
    flux_material *material;
    flux_mat4 palette[1];
} skin_case;

typedef struct texture_case {
    flux_mesh *quad;
    flux_material *material;
} texture_case;

static void draw_skin_case(flux_frame *f, const flux_camera *cam, void *user) {
    skin_case *c = user;
    flux_scene_draw_mesh_skinned(f, cam, flux_mat4_identity(), c->quad, c->material, c->palette, 1);
}

static void draw_texture_case(flux_frame *f, const flux_camera *cam, void *user) {
    texture_case *c = user;
    flux_scene_draw_mesh(f, cam, flux_mat4_identity(), c->quad, c->material);
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_render: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    flux_surface *s = nullptr;
    {
        flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
        sd.width = W;
        sd.height = H;
        EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);
    }

    flux_target_desc depth_desc = {
        .type = FLUX_TYPE_TARGET_DESC,
        .usage = FLUX_TARGET_DEPTH,
        .format = FLUX_DEPTH_FORMAT,
        .width = W,
        .height = H,
    };
    flux_target *depth = nullptr;
    EXPECT(flux_target_create(d, &depth_desc, &depth) == FLUX_OK);

    flux_camera cam;
    flux_camera_perspective(&cam, 1.0f, (float)W / (float)H, 0.1f, 100.0f);
    flux_camera_look_at(&cam, (flux_vec3){0, 0, 4}, (flux_vec3){0, 0, 0}, (flux_vec3){0, 1, 0});

    flux_format color_fmt = flux_format_from_vk(flux_surface_vk_format(s));
    static uint8_t px[BYTES];

    /* --- depth test: near (red) drawn first survives far (green) --- */
    {
        depth_case c = {0};
        c.near_quad = make_quad(d, 0.8f, 1.0f); /* 3 units from the eye */
        c.far_quad = make_quad(d, 2.5f, -1.0f); /* 5 units, projects larger */
        EXPECT(c.near_quad != nullptr && c.far_quad != nullptr);

        flux_material_desc md = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 0, 0, 1},
            .color_format = color_fmt,
            .depth_format = FLUX_DEPTH_FORMAT,
        };
        EXPECT(flux_material_create(d, &md, &c.red) == FLUX_OK);
        md.base_color = (flux_vec4){0, 1, 0, 1};
        EXPECT(flux_material_create(d, &md, &c.green) == FLUX_OK);

        EXPECT(render_frame(s, depth, &cam, draw_depth_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);

        /* Centre: the near red quad, despite being drawn first. */
        const uint8_t *centre = px_at(px, W / 2, H / 2);
        EXPECT(centre[0] > 200 && centre[1] < 50);

        /* Off-centre along x: outside the near quad's projection
         * (extends to ~0.49 of the half-screen) but inside the far
         * quad's (~0.91) — the green quad shows. Symmetric in x, so
         * no assumption about axis orientation. */
        const uint8_t *ring = px_at(px, W / 2 + (uint32_t)(0.65f * W / 2), H / 2);
        EXPECT(ring[1] > 200 && ring[0] < 50);

        /* Corner: beyond both quads — the clear colour. */
        const uint8_t *corner = px_at(px, 1, 1);
        EXPECT(corner[0] < 20 && corner[1] < 20 && corner[2] < 20);

        flux_material_release(c.red);
        flux_material_release(c.green);
        flux_mesh_release(c.near_quad);
        flux_mesh_release(c.far_quad);
    }

    /* --- Phong: head-on light brightens, departing light leaves ambient --- */
    {
        phong_case c = {0};
        c.quad = make_quad(d, 1.5f, 0.0f);
        EXPECT(c.quad != nullptr);

        flux_material_desc md = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_PHONG,
            .base_color = {1, 1, 1, 1},
            .color_format = color_fmt,
            .depth_format = FLUX_DEPTH_FORMAT,
            .shininess = 32.0f,
            .specular = 0.0f, /* diffuse only: predictable bytes */
        };
        EXPECT(flux_material_create(d, &md, &c.mat) == FLUX_OK);

        /* Light travels -Z, straight at the +Z-facing quad: ndotl = 1,
         * so the centre is ambient + full diffuse ≈ saturated white. */
        c.light = (flux_scene_light){
            .direction = {0, 0, -1},
            .color = {1, 1, 1},
            .ambient = 0.1f,
        };
        EXPECT(render_frame(s, depth, &cam, draw_phong_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        /* Copy out of px — the next readback overwrites it. */
        uint8_t lit[3];
        memcpy(lit, px_at(px, W / 2, H / 2), 3);
        EXPECT(lit[0] > 200 && lit[1] > 200 && lit[2] > 200);

        /* Light travels +Z, away from the quad: ndotl = 0, ambient
         * only (0.1 → ~26). The shading must respond to direction. */
        c.light.direction = (flux_vec3){0, 0, 1};
        EXPECT(render_frame(s, depth, &cam, draw_phong_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *dark = px_at(px, W / 2, H / 2);
        EXPECT(dark[0] > 5 && dark[0] < 80);
        EXPECT(dark[0] < lit[0] / 2);

        flux_material_release(c.mat);
        flux_mesh_release(c.quad);
    }

    /* --- GPU skinning: a one-joint palette translates the bind-pose quad. --- */
    {
        skin_case c = {0};
        c.quad = make_skinned_quad(d, 0.4f);
        c.palette[0] = flux_mat4_translate(1.0f, 0.0f, 0.0f);
        EXPECT(c.quad != nullptr);
        flux_material_desc md = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 0, 0, 1},
            .color_format = color_fmt,
            .depth_format = FLUX_DEPTH_FORMAT,
        };
        EXPECT(flux_material_create(d, &md, &c.material) == FLUX_OK);
        EXPECT(render_frame(s, depth, &cam, draw_skin_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *old_centre = px_at(px, W / 2, H / 2);
        const uint8_t *translated = px_at(px, W / 2 + 15, H / 2);
        EXPECT(old_centre[0] < 20);
        EXPECT(translated[0] > 200 && translated[1] < 30);
        flux_material_release(c.material);
        flux_mesh_release(c.quad);
    }

    /* --- Textured UNLIT: sRGB sample, UV transform, OPAQUE/MASK/BLEND. --- */
    {
        texture_case c = {.quad = make_quad(d, 1.5f, 0.0f)};
        /* First texel is half-alpha red; second is transparent green. */
        const uint8_t texels[8] = {255, 0, 0, 128, 0, 255, 0, 0};
        flux_image_desc image_desc = FLUX_IMAGE_DESC_INIT;
        image_desc.width = 2;
        image_desc.height = 1;
        image_desc.format = FLUX_FORMAT_RGBA8_SRGB;
        image_desc.initial_data = texels;
        flux_image *image = nullptr;
        EXPECT(flux_image_create(d, &image_desc, &image) == FLUX_OK);

        flux_sampler_desc sampler_desc = FLUX_SAMPLER_DESC_INIT;
        sampler_desc.min_filter = FLUX_FILTER_NEAREST;
        sampler_desc.mag_filter = FLUX_FILTER_NEAREST;
        sampler_desc.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
        flux_sampler *sampler = nullptr;
        EXPECT(flux_sampler_create(d, &sampler_desc, &sampler) == FLUX_OK);

        flux_material_surface_desc surface = FLUX_MATERIAL_SURFACE_DESC_INIT;
        surface.base_color_image = image;
        surface.base_color_sampler = sampler;
        surface.uv_scale = (flux_vec2){0, 0};
        surface.uv_offset = (flux_vec2){0.25f, 0.5f};
        flux_material_desc material_desc = {
            .type = FLUX_TYPE_MATERIAL_DESC,
            .next = &surface,
            .kind = FLUX_MATERIAL_UNLIT,
            .base_color = {1, 1, 1, 1},
            .color_format = color_fmt,
            .depth_format = FLUX_DEPTH_FORMAT,
        };

        /* OPAQUE ignores sampled alpha: the red texel is fully visible. */
        EXPECT(flux_material_create(d, &material_desc, &c.material) == FLUX_OK);
        EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *opaque = px_at(px, W / 2, H / 2);
        EXPECT(opaque[0] > 220 && opaque[1] < 30 && opaque[2] < 30);
        flux_material_release(c.material);

        /* UV offset selects the transparent green texel; MASK discards it. */
        surface.alpha_mode = FLUX_MATERIAL_ALPHA_MASK;
        surface.alpha_cutoff = 0.5f;
        surface.uv_offset = (flux_vec2){0.75f, 0.5f};
        EXPECT(flux_material_create(d, &material_desc, &c.material) == FLUX_OK);
        EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *masked = px_at(px, W / 2, H / 2);
        EXPECT(masked[0] < 20 && masked[1] < 20 && masked[2] < 20);
        flux_material_release(c.material);

        /* BLEND uses straight source alpha and leaves depth writes disabled. */
        surface.alpha_mode = FLUX_MATERIAL_ALPHA_BLEND;
        surface.uv_offset = (flux_vec2){0.25f, 0.5f};
        surface.double_sided = true;
        EXPECT(flux_material_create(d, &material_desc, &c.material) == FLUX_OK);
        /* Material owns retained references after successful creation. */
        flux_sampler_release(sampler);
        flux_image_release(image);
        EXPECT(render_frame(s, depth, &cam, draw_texture_case, &c) == FLUX_OK);
        memset(px, 0xCD, BYTES);
        EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
        const uint8_t *blended = px_at(px, W / 2, H / 2);
        EXPECT(blended[0] > 100 && blended[0] < 160);
        EXPECT(blended[1] < 30 && blended[2] < 30);

        flux_material_release(c.material);
        flux_mesh_release(c.quad);
    }

    /* --- Working-space scene rendering: 16F target decodes texels at the
     * edge, so lighting runs in linear light (ADR-0069) --- */
    {
        /* Phong with ndotl = 0.5 over a mid-gray sRGB texture. Linear
         * light: srgb_decode(0.502) x 0.5 = 0.108 -> sRGB ~92. The legacy
         * gamma-space path computes 0.502 x 0.5 -> 64. */
        const uint8_t gray[4] = {128, 128, 128, 255};
        flux_image_desc image_desc = FLUX_IMAGE_DESC_INIT;
        image_desc.width = 1;
        image_desc.height = 1;
        image_desc.format = FLUX_FORMAT_RGBA8_UNORM;
        image_desc.initial_data = gray;
        flux_image *tex = nullptr;
        EXPECT(flux_image_create(d, &image_desc, &tex) == FLUX_OK);

        flux_material_surface_desc surface = FLUX_MATERIAL_SURFACE_DESC_INIT;
        surface.base_color_image = tex;
        surface.base_color_sampler = nullptr; /* device default */
        flux_scene_light light = {
            .direction = {0.0f, 0.8660254f, -0.5f}, /* L . N = 0.5 */
            .color = {1, 1, 1},
            .ambient = 0.0f,
        };

        const float want_linear =
            flux_transfer_decode(FLUX_TRANSFER_SRGB, 0.0f, 128.0f / 255.0f) * 0.5f; /* ndotl */
        const int want16f =
            (int)lrintf(flux_transfer_encode(FLUX_TRANSFER_SRGB, 0.0f, want_linear) * 255.0f);

        int got[2] = {0, 0};
        for (int variant = 0; variant < 2; ++variant) {
            /* variant 0: legacy 8-bit target (raw gamma path, unchanged);
             * variant 1: 16F working-space target (edge decode). */
            flux_format tfmt = variant == 0 ? color_fmt : FLUX_FORMAT_RGBA16_SFLOAT;
            flux_image *rt = nullptr;
            EXPECT(flux_image_create_render_target(d, W, H, tfmt, &rt) == FLUX_OK);

            flux_material_desc md = {
                .type = FLUX_TYPE_MATERIAL_DESC,
                .next = &surface,
                .kind = FLUX_MATERIAL_PHONG,
                .base_color = {1, 1, 1, 1},
                .color_format = tfmt,
                .depth_format = FLUX_DEPTH_FORMAT,
                .shininess = 32.0f,
                .specular = 0.0f,
            };
            flux_material *mat = nullptr;
            EXPECT(flux_material_create(d, &md, &mat) == FLUX_OK);
            flux_mesh *quad = make_quad(d, 1.5f, 0.0f);
            EXPECT(quad != nullptr);

            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
            EXPECT(flux_frame_prepare_image_target(frame, rt) == FLUX_OK);
            flux_pass_attachment color_att = {
                .view = flux_image_vk_image_view(rt),
                .format = flux_format_to_vk(tfmt),
                .load_op = FLUX_LOAD_CLEAR,
                .store_op = FLUX_STORE_STORE,
                .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
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
                .color_attachments = &color_att,
                .depth = &depth_att,
                .width = W,
                .height = H,
            };
            flux_frame_begin_pass(frame, &pass);
            flux_frame_set_viewport(frame, 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f);
            flux_frame_set_scissor(frame, 0, 0, W, H);
            flux_scene_draw_mesh_lit(frame, &cam, flux_mat4_identity(), quad, mat, &light);
            flux_frame_end_pass(frame);
            EXPECT(flux_frame_finish_image_target(frame, rt) == FLUX_OK);

            /* Composite the target onto the sRGB surface through the
             * canvas (16F images are working-space linear; 8-bit UNORM
             * decodes as sRGB — the round trip is identity). */
            flux_canvas *canvas = nullptr;
            flux_canvas_desc cd = FLUX_CANVAS_DESC_INIT;
            cd.surface = s;
            EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);
            flux_color black = flux_color_rgba(0, 0, 0, 255);
            EXPECT(flux_canvas_begin(canvas, frame, &black) == FLUX_OK);
            flux_canvas_draw_image(canvas, rt, (flux_rect){0, 0, (float)W, (float)H}, nullptr);
            flux_canvas_end(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);

            memset(px, 0xCD, BYTES);
            EXPECT(flux_surface_read_pixels(s, px, BYTES) == FLUX_OK);
            got[variant] = px_at(px, W / 2, H / 2)[0];

            flux_canvas_destroy(canvas);
            flux_mesh_release(quad);
            flux_material_release(mat);
            flux_image_release(rt);
        }

        EXPECT(got[0] > 58 && got[0] < 70); /* legacy gamma path */
        EXPECT(got[1] > want16f - 5 && got[1] < want16f + 5);
        EXPECT(got[1] > got[0] + 15); /* linear-light lift is unmistakable */

        flux_image_release(tex);
    }

    flux_device_wait_idle(d);
    flux_target_release(depth);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
