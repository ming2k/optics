/*
 * Scene vector coverage material integration (ADR-0117).
 *
 * Exercises OPTICS_GFX_MATERIAL_VECTOR in a 3D perspective scene:
 *   - Object-space input (INV-SCV-02) via segments storage buffer
 *   - No bake (INV-SCV-01): pure mathematical evaluation, no atlas or texture
 *   - Projective correctness (INV-SCV-03): crisp analytic coverage under strong
 *     3D perspective foreshortening, asserting > 16 distinct coverage levels
 *   - Module isolation (INV-SCV-05): tests scene without canvas headers.
 */
#include "gfx_test.h"
#include "optics_test.h"

#include <math.h>
#include <optics/gfx.h>
#include <optics/gfx/core.h>
#include <optics/gfx/curve.h>
#include <optics/gfx/math.h>
#include <optics/gfx/scene.h>
#include <optics/gfx/vulkan.h>
#include <stdlib.h>
#include <string.h>

#define W 128u
#define H 128u

typedef struct GpuSegment {
    float x0, y0;
    float x1, y1;
} GpuSegment;

static int count_edge_levels(const uint8_t *px, uint32_t w, uint32_t h) {
    bool seen[256];
    memset(seen, 0, sizeof(seen));
    for (uint32_t y = 10; y < h - 10; ++y) {
        for (uint32_t x = 10; x < w - 10; ++x) {
            uint8_t g = px[(size_t)y * w * 4 + x * 4 + 1]; /* green channel */
            seen[g] = true;
        }
    }
    int n = 0;
    for (int i = 0; i < 256; ++i)
        n += seen[i];
    return n;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_vector: no Vulkan device available; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    optics_gfx_format color_fmt = optics_gfx_format_from_vk(optics_gfx_surface_vk_format(s));

    /* 1. Quad mesh in object space [-1, 1] x [-1, 1] on the Z=0 plane */
    optics_gfx_vertex verts[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    optics_gfx_mesh_desc mdesc = {
        .type = OPTICS_GFX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = indices,
        .index_count = 6,
    };
    optics_gfx_mesh *mesh = nullptr;
    OT_EXPECT(optics_gfx_mesh_create(d, &mdesc, &mesh) == OPTICS_GFX_OK);

    /* 2. Vector content: a high-contrast oblique polygon in object space [-0.8, 0.8] */
    GpuSegment segs[4] = {
        {-0.7f, -0.6f, 0.7f, -0.4f},
        {0.7f, -0.4f, 0.7f, 0.7f},
        {0.7f, 0.7f, -0.7f, 0.7f},
        {-0.7f, 0.7f, -0.7f, -0.6f},
    };

    optics_gfx_buffer_desc bdesc = {
        .type = OPTICS_GFX_TYPE_BUFFER_DESC,
        .size = sizeof(segs),
        .usage = OPTICS_GFX_BUFFER_USAGE_STORAGE,
        .location = OPTICS_GFX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = segs,
    };
    optics_gfx_buffer *seg_buf = nullptr;
    OT_EXPECT(optics_gfx_buffer_create(d, &bdesc, &seg_buf) == OPTICS_GFX_OK);

    /* 3. ADR-0117 Vector Coverage Material */
    optics_gfx_material_vector_desc vdesc = {
        .type = OPTICS_GFX_TYPE_MATERIAL_VECTOR_DESC,
        .segments_buffer = seg_buf,
        .segment_count = 4,
        .fill_rule = 0u, /* NonZero */
    };

    optics_gfx_material_desc mat_desc = {
        .type = OPTICS_GFX_TYPE_MATERIAL_DESC,
        .next = &vdesc,
        .kind = OPTICS_GFX_MATERIAL_VECTOR,
        .base_color = {0.1f, 0.9f, 0.3f, 1.0f}, /* Green */
        .color_format = color_fmt,
        .depth_format = OPTICS_GFX_FORMAT_UNDEFINED,
    };
    optics_gfx_material *material = nullptr;
    OT_EXPECT(optics_gfx_material_create(d, &mat_desc, &material) == OPTICS_GFX_OK);

    /* 4. Camera with strong 3D perspective foreshortening */
    optics_gfx_camera cam;
    optics_gfx_camera_perspective(
        &cam,
        &(optics_gfx_camera_perspective_opts){
            .fov_y_rad = 1.047f /* 60 deg */, .aspect = 1.0f, .z_near = 0.1f, .z_far = 100.0f});
    optics_gfx_camera_look_at(&cam, (optics_gfx_vec3){0.0f, -0.5f, 2.5f},
                              (optics_gfx_vec3){0.0f, 0.0f, 0.0f},
                              (optics_gfx_vec3){0.0f, 1.0f, 0.0f});

    /* World matrix: rotated quad in 3D perspective space (INV-SCV-03) */
    optics_gfx_mat4 rot_y =
        optics_gfx_mat4_rotation_quat(optics_gfx_quat_axis_angle((optics_gfx_vec3){0, 1, 0}, 0.5f));
    optics_gfx_mat4 rot_x =
        optics_gfx_mat4_rotation_quat(optics_gfx_quat_axis_angle((optics_gfx_vec3){1, 0, 0}, 0.3f));
    optics_gfx_mat4 world = optics_gfx_mat4_multiply(rot_y, rot_x);

    /* 5. Render onto surface */
    static uint8_t px[W * H * 4];
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);

        optics_gfx_pass_attachment color = {
            .load_op = OPTICS_GFX_LOAD_CLEAR,
            .store_op = OPTICS_GFX_STORE_STORE,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        optics_gfx_pass_desc pass = {
            .type = OPTICS_GFX_TYPE_PASS_DESC,
            .color_attachment_count = 1,
            .color_attachments = &color,
        };
        optics_gfx_frame_begin_pass(frame, &pass);
        optics_gfx_frame_set_viewport(frame,
                                      &(optics_gfx_frame_set_viewport_opts){.x = 0.0f,
                                                                            .y = 0.0f,
                                                                            .width = (float)W,
                                                                            .height = (float)H,
                                                                            .min_depth = 0.0f,
                                                                            .max_depth = 1.0f});
        optics_gfx_frame_set_scissor(
            frame, &(optics_gfx_frame_set_scissor_opts){.x = 0, .y = 0, .width = W, .height = H});

        optics_gfx_scene_draw_mesh(
            frame, &(optics_gfx_scene_draw_mesh_opts){
                       .camera = &cam, .world = world, .mesh = mesh, .material = material});

        optics_gfx_frame_end_pass(frame);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, sizeof(px)) == OPTICS_GFX_OK);
    }

    int levels = count_edge_levels(px, W, H);
    fprintf(stderr,
            "  ADR-0117 3D Scene Vector Material: %d distinct coverage levels under perspective\n",
            levels);

    /* Assert analytic continuous coverage in 3D perspective space (> 16 levels, INV-SCV-03) */
    OT_EXPECT(levels > 16);
    OT_EXPECT(levels >= 40);

    optics_gfx_material_release(material);
    optics_gfx_buffer_release(seg_buf);
    optics_gfx_mesh_release(mesh);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
