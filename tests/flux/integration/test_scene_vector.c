/*
 * Scene vector coverage material integration (ADR-0117).
 *
 * Exercises FLUX_MATERIAL_VECTOR in a 3D perspective scene:
 *   - Object-space input (INV-SCV-02) via segments storage buffer
 *   - No bake (INV-SCV-01): pure mathematical evaluation, no atlas or texture
 *   - Projective correctness (INV-SCV-03): crisp analytic coverage under strong
 *     3D perspective foreshortening, asserting > 16 distinct coverage levels
 *   - Module isolation (INV-SCV-05): tests scene without canvas headers.
 */
#include "test_helpers.h"

#include <flux/core.h>
#include <flux/curve.h>
#include <flux/flux.h>
#include <flux/math.h>
#include <flux/scene.h>
#include <flux/vulkan.h>
#include <math.h>
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
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_vector: no Vulkan device; skipping\n");
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    flux_format color_fmt = flux_format_from_vk(flux_surface_vk_format(s));

    /* 1. Quad mesh in object space [-1, 1] x [-1, 1] on the Z=0 plane */
    flux_vertex verts[4] = {
        {{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
        {{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
        {{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
        {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},
    };
    uint32_t indices[6] = {0, 1, 2, 0, 2, 3};

    flux_mesh_desc mdesc = {
        .type = FLUX_TYPE_MESH_DESC,
        .vertices = verts,
        .vertex_count = 4,
        .indices = indices,
        .index_count = 6,
    };
    flux_mesh *mesh = nullptr;
    EXPECT(flux_mesh_create(d, &mdesc, &mesh) == FLUX_OK);

    /* 2. Vector content: a high-contrast oblique polygon in object space [-0.8, 0.8] */
    GpuSegment segs[4] = {
        {-0.7f, -0.6f, 0.7f, -0.4f},
        {0.7f, -0.4f, 0.7f, 0.7f},
        {0.7f, 0.7f, -0.7f, 0.7f},
        {-0.7f, 0.7f, -0.7f, -0.6f},
    };

    flux_buffer_desc bdesc = {
        .type = FLUX_TYPE_BUFFER_DESC,
        .size = sizeof(segs),
        .usage = FLUX_BUFFER_USAGE_STORAGE,
        .location = FLUX_BUFFER_HOST_VISIBLE,
        .device_address = true,
        .initial_data = segs,
    };
    flux_buffer *seg_buf = nullptr;
    EXPECT(flux_buffer_create(d, &bdesc, &seg_buf) == FLUX_OK);

    /* 3. ADR-0117 Vector Coverage Material */
    flux_material_vector_desc vdesc = {
        .type = FLUX_TYPE_MATERIAL_VECTOR_DESC,
        .segments_buffer = seg_buf,
        .segment_count = 4,
        .fill_rule = 0u, /* NonZero */
    };

    flux_material_desc mat_desc = {
        .type = FLUX_TYPE_MATERIAL_DESC,
        .next = &vdesc,
        .kind = FLUX_MATERIAL_VECTOR,
        .base_color = {0.1f, 0.9f, 0.3f, 1.0f}, /* Green */
        .color_format = color_fmt,
        .depth_format = FLUX_FORMAT_UNDEFINED,
    };
    flux_material *material = nullptr;
    EXPECT(flux_material_create(d, &mat_desc, &material) == FLUX_OK);

    /* 4. Camera with strong 3D perspective foreshortening */
    flux_camera cam;
    flux_camera_perspective(&cam, 1.047f /* 60 deg */, 1.0f, 0.1f, 100.0f);
    flux_camera_look_at(&cam, (flux_vec3){0.0f, -0.5f, 2.5f}, (flux_vec3){0.0f, 0.0f, 0.0f},
                        (flux_vec3){0.0f, 1.0f, 0.0f});

    /* World matrix: rotated quad in 3D perspective space (INV-SCV-03) */
    flux_mat4 rot_y = flux_mat4_rotation_quat(flux_quat_axis_angle((flux_vec3){0, 1, 0}, 0.5f));
    flux_mat4 rot_x = flux_mat4_rotation_quat(flux_quat_axis_angle((flux_vec3){1, 0, 0}, 0.3f));
    flux_mat4 world = flux_mat4_multiply(rot_y, rot_x);

    /* 5. Render onto surface */
    static uint8_t px[W * H * 4];
    {
        flux_frame *frame = nullptr;
        EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);

        flux_pass_attachment color = {
            .load_op = FLUX_LOAD_CLEAR,
            .store_op = FLUX_STORE_STORE,
            .clear_color = {0.0f, 0.0f, 0.0f, 1.0f},
        };
        flux_pass_desc pass = {
            .type = FLUX_TYPE_PASS_DESC,
            .color_attachment_count = 1,
            .color_attachments = &color,
        };
        flux_frame_begin_pass(frame, &pass);
        flux_frame_set_viewport(frame, 0.0f, 0.0f, (float)W, (float)H, 0.0f, 1.0f);
        flux_frame_set_scissor(frame, 0, 0, W, H);

        flux_scene_draw_mesh(frame, &cam, world, mesh, material);

        flux_frame_end_pass(frame);
        EXPECT(flux_frame_submit(frame) == FLUX_OK);
        EXPECT(flux_frame_present(frame) == FLUX_OK);
        EXPECT(flux_surface_read_pixels(s, px, sizeof(px)) == FLUX_OK);
    }

    int levels = count_edge_levels(px, W, H);
    fprintf(stderr,
            "  ADR-0117 3D Scene Vector Material: %d distinct coverage levels under perspective\n",
            levels);

    /* Assert analytic continuous coverage in 3D perspective space (> 16 levels, INV-SCV-03) */
    EXPECT(levels > 16);
    EXPECT(levels >= 40);

    flux_material_release(material);
    flux_buffer_release(seg_buf);
    flux_mesh_release(mesh);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
