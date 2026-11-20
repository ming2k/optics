/*
 * Scene mesh + material lifecycle. Headless device + retain/release
 * cycles + reject path for unimplemented material kinds.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/scene.h>

int main(void) {
    /* --- NULL device/desc/out are rejected without touching anything --- */
    {
        optics_gfx_mesh *m = nullptr;
        OT_EXPECT(optics_gfx_mesh_create(nullptr, nullptr, &m) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(nullptr, nullptr, &mat) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_mesh: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- mesh: indexed, retain/release ref counting --- */
    {
        optics_gfx_vertex verts[3] = {
            {{-1, -1, 0}, {0, 0, 1}, {0, 0}},
            {{1, -1, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0.5f, 1}},
        };
        uint32_t indices[3] = {0, 1, 2};

        optics_gfx_mesh_desc md = OPTICS_GFX_MESH_DESC_INIT;
        md.vertices = verts;
        md.vertex_count = 3;
        md.indices = indices;
        md.index_count = 3;

        optics_gfx_mesh *m = nullptr;
        OT_EXPECT(optics_gfx_mesh_create(d, &md, &m) == OPTICS_GFX_OK);
        OT_EXPECT(m != nullptr);

        /* retain returns the same pointer; release that extra ref,
         * then release the original — should not free until the
         * count hits zero. */
        OT_EXPECT(optics_gfx_mesh_retain(m) == m);
        optics_gfx_mesh_release(m); /* drop the extra ref */
        optics_gfx_mesh_release(m); /* final release */
    }

    /* --- mesh: missing vertices rejected --- */
    {
        optics_gfx_mesh *m = nullptr;
        optics_gfx_mesh_desc md = OPTICS_GFX_MESH_DESC_INIT;
        OT_EXPECT(optics_gfx_mesh_create(d, &md, &m) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(m == nullptr);
    }

    /* --- material: UNLIT happy path --- */
    {
        optics_gfx_material_desc desc = OPTICS_GFX_MATERIAL_DESC_INIT;
        desc.kind = OPTICS_GFX_MATERIAL_UNLIT;
        desc.base_color = (optics_gfx_vec4){1, 0.5f, 0.25f, 1};
        desc.color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
        desc.depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT;

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_OK);
        OT_EXPECT(mat != nullptr);
        OT_EXPECT(optics_gfx_material_retain(mat) == mat);
        optics_gfx_material_release(mat);
        optics_gfx_material_release(mat);
    }

    /* --- material: PHONG happy path (default + explicit params) --- */
    {
        optics_gfx_material_desc desc = OPTICS_GFX_MATERIAL_DESC_INIT;
        desc.kind = OPTICS_GFX_MATERIAL_PHONG;
        desc.base_color = (optics_gfx_vec4){0.8f, 0.2f, 0.2f, 1};
        desc.color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
        desc.depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT;
        /* shininess/specular left zero: shininess falls back to the
         * library default, specular 0 disables the highlight. */

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_OK);
        OT_EXPECT(mat != nullptr);
        OT_EXPECT(optics_gfx_material_retain(mat) == mat);
        optics_gfx_material_release(mat);
        optics_gfx_material_release(mat);

        desc.shininess = 64.0f;
        desc.specular = 0.5f;
        mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_OK);
        OT_EXPECT(mat != nullptr);
        optics_gfx_material_release(mat);
    }

    /* --- material: PHONG without depth attachment also builds --- */
    {
        optics_gfx_material_desc desc = OPTICS_GFX_MATERIAL_DESC_INIT;
        desc.kind = OPTICS_GFX_MATERIAL_PHONG;
        desc.base_color = (optics_gfx_vec4){1, 1, 1, 1};
        desc.color_format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        desc.depth_format = OPTICS_GFX_FORMAT_UNDEFINED;

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_OK);
        OT_EXPECT(mat != nullptr);
        optics_gfx_material_release(mat);
    }

    /* --- material: undefined color format rejected --- */
    {
        optics_gfx_material_desc desc = OPTICS_GFX_MATERIAL_DESC_INIT;
        desc.kind = OPTICS_GFX_MATERIAL_UNLIT;
        desc.color_format = OPTICS_GFX_FORMAT_UNDEFINED;

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(mat == nullptr);
    }

    /* --- material: out-of-range kind rejected --- */
    {
        optics_gfx_material_desc desc = OPTICS_GFX_MATERIAL_DESC_INIT;
        desc.kind = (optics_gfx_material_kind)0xFF;
        desc.color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM;

        optics_gfx_material *mat = nullptr;
        OT_EXPECT(optics_gfx_material_create(d, &desc, &mat) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(mat == nullptr);
    }

    optics_gfx_mesh_release(nullptr);
    optics_gfx_material_release(nullptr);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
