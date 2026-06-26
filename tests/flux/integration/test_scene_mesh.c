/*
 * Scene mesh + material lifecycle. Headless device + retain/release
 * cycles + reject path for unimplemented material kinds.
 */
#include "test_helpers.h"
#include <flux/flux.h>
#include <flux/scene.h>

int main(void) {
    /* --- NULL device/desc/out are rejected without touching anything --- */
    {
        flux_mesh *m = nullptr;
        EXPECT(flux_mesh_create(nullptr, nullptr, &m) == FLUX_ERROR_INVALID_ARGUMENT);

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(nullptr, nullptr, &mat) == FLUX_ERROR_INVALID_ARGUMENT);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_scene_mesh: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* --- mesh: indexed, retain/release ref counting --- */
    {
        flux_vertex verts[3] = {
            {{-1, -1, 0}, {0, 0, 1}, {0, 0}},
            {{1, -1, 0}, {0, 0, 1}, {1, 0}},
            {{0, 1, 0}, {0, 0, 1}, {0.5f, 1}},
        };
        uint32_t indices[3] = {0, 1, 2};

        flux_mesh_desc md = FLUX_MESH_DESC_INIT;
        md.vertices = verts;
        md.vertex_count = 3;
        md.indices = indices;
        md.index_count = 3;

        flux_mesh *m = nullptr;
        EXPECT(flux_mesh_create(d, &md, &m) == FLUX_OK);
        EXPECT(m != nullptr);

        /* retain returns the same pointer; release that extra ref,
         * then release the original — should not free until the
         * count hits zero. */
        EXPECT(flux_mesh_retain(m) == m);
        flux_mesh_release(m); /* drop the extra ref */
        flux_mesh_release(m); /* final release */
    }

    /* --- mesh: missing vertices rejected --- */
    {
        flux_mesh *m = nullptr;
        flux_mesh_desc md = FLUX_MESH_DESC_INIT;
        EXPECT(flux_mesh_create(d, &md, &m) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(m == nullptr);
    }

    /* --- material: UNLIT happy path --- */
    {
        flux_material_desc desc = FLUX_MATERIAL_DESC_INIT;
        desc.kind = FLUX_MATERIAL_UNLIT;
        desc.base_color = (flux_vec4){1, 0.5f, 0.25f, 1};
        desc.color_format = FLUX_FORMAT_BGRA8_UNORM;
        desc.depth_format = FLUX_FORMAT_D32_SFLOAT;

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_OK);
        EXPECT(mat != nullptr);
        EXPECT(flux_material_retain(mat) == mat);
        flux_material_release(mat);
        flux_material_release(mat);
    }

    /* --- material: PHONG happy path (default + explicit params) --- */
    {
        flux_material_desc desc = FLUX_MATERIAL_DESC_INIT;
        desc.kind = FLUX_MATERIAL_PHONG;
        desc.base_color = (flux_vec4){0.8f, 0.2f, 0.2f, 1};
        desc.color_format = FLUX_FORMAT_BGRA8_UNORM;
        desc.depth_format = FLUX_FORMAT_D32_SFLOAT;
        /* shininess/specular left zero: shininess falls back to the
         * library default, specular 0 disables the highlight. */

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_OK);
        EXPECT(mat != nullptr);
        EXPECT(flux_material_retain(mat) == mat);
        flux_material_release(mat);
        flux_material_release(mat);

        desc.shininess = 64.0f;
        desc.specular = 0.5f;
        mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_OK);
        EXPECT(mat != nullptr);
        flux_material_release(mat);
    }

    /* --- material: PHONG without depth attachment also builds --- */
    {
        flux_material_desc desc = FLUX_MATERIAL_DESC_INIT;
        desc.kind = FLUX_MATERIAL_PHONG;
        desc.base_color = (flux_vec4){1, 1, 1, 1};
        desc.color_format = FLUX_FORMAT_RGBA8_UNORM;
        desc.depth_format = FLUX_FORMAT_UNDEFINED;

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_OK);
        EXPECT(mat != nullptr);
        flux_material_release(mat);
    }

    /* --- material: undefined color format rejected --- */
    {
        flux_material_desc desc = FLUX_MATERIAL_DESC_INIT;
        desc.kind = FLUX_MATERIAL_UNLIT;
        desc.color_format = FLUX_FORMAT_UNDEFINED;

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(mat == nullptr);
    }

    /* --- material: out-of-range kind rejected --- */
    {
        flux_material_desc desc = FLUX_MATERIAL_DESC_INIT;
        desc.kind = (flux_material_kind)0xFF;
        desc.color_format = FLUX_FORMAT_BGRA8_UNORM;

        flux_material *mat = nullptr;
        EXPECT(flux_material_create(d, &desc, &mat) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(mat == nullptr);
    }

    flux_mesh_release(nullptr);
    flux_material_release(nullptr);
    flux_device_release(d);
    TEST_SUMMARY();
}
