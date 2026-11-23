/*
 * optics_gfx_graphics_pipeline lifecycle. Builds a real pipeline against
 * the embedded test triangle shaders, exercises retain/release, and
 * covers the rejection paths. Skips if no Vulkan device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <stdalign.h>

/* Shader bytecode, baked in with C23 #embed (--embed-dir is the build dir). */
alignas(uint32_t) static const unsigned char vert_spv[] = {
#embed "triangle.vert.spv"
};
alignas(uint32_t) static const unsigned char frag_spv[] = {
#embed "triangle.frag.spv"
};

static optics_gfx_graphics_pipeline_desc base_desc(void) {
    optics_gfx_graphics_pipeline_desc desc = OPTICS_GFX_GRAPHICS_PIPELINE_DESC_INIT;
    desc.vertex_spirv = (const uint32_t *)vert_spv;
    desc.vertex_spirv_word_count = sizeof(vert_spv) / sizeof(uint32_t);
    desc.fragment_spirv = (const uint32_t *)frag_spv;
    desc.fragment_spirv_word_count = sizeof(frag_spv) / sizeof(uint32_t);
    desc.topology = OPTICS_GFX_TOPOLOGY_TRIANGLE_LIST;
    desc.cull = OPTICS_GFX_CULL_NONE;
    desc.blend = OPTICS_GFX_BLEND_PRESET_NONE;
    desc.depth = OPTICS_GFX_DEPTH_NONE;
    desc.color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
    desc.depth_format = OPTICS_GFX_FORMAT_UNDEFINED;
    return desc;
}

int main(void) {
    /* --- NULL rejection without a device --- */
    {
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(nullptr, nullptr, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_graphics_pipeline: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- happy path --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) == OPTICS_GFX_OK);
        OT_EXPECT(p != nullptr);
        OT_EXPECT(optics_gfx_graphics_pipeline_vk_pipeline(p) != VK_NULL_HANDLE);
        OT_EXPECT(optics_gfx_graphics_pipeline_vk_layout(p) != VK_NULL_HANDLE);

        OT_EXPECT(optics_gfx_graphics_pipeline_retain(p) == p);
        optics_gfx_graphics_pipeline_release(p); /* drop the retain */
        optics_gfx_graphics_pipeline_release(p); /* final */
    }

    /* --- blend presets cover the same shader without re-error --- */
    {
        for (uint32_t bp = OPTICS_GFX_BLEND_PRESET_NONE; bp <= OPTICS_GFX_BLEND_PRESET_ADDITIVE;
             ++bp) {
            optics_gfx_graphics_pipeline_desc desc = base_desc();
            desc.blend = (optics_gfx_blend_preset)bp;
            optics_gfx_graphics_pipeline *p = nullptr;
            OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) == OPTICS_GFX_OK);
            optics_gfx_graphics_pipeline_release(p);
        }
    }

    /* --- depth-tested pipeline with depth format --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.depth = OPTICS_GFX_DEPTH_TEST_AND_WRITE;
        desc.depth_format = OPTICS_GFX_FORMAT_D32_SFLOAT;
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) == OPTICS_GFX_OK);
        optics_gfx_graphics_pipeline_release(p);
    }

    /* --- vertex binding with attributes --- */
    {
        optics_gfx_vertex_attribute attrs[2] = {
            {.location = 0, .format = OPTICS_GFX_FORMAT_RGB32_SFLOAT, .offset = 0},
            {.location = 1, .format = OPTICS_GFX_FORMAT_RG32_SFLOAT, .offset = 12},
        };
        optics_gfx_vertex_binding binding = {
            .stride = 20,
            .attribute_count = 2,
            .attributes = attrs,
        };
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.vertex_binding = &binding;
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) == OPTICS_GFX_OK);
        optics_gfx_graphics_pipeline_release(p);
    }

    /* --- wrong sType --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.type = OPTICS_GFX_TYPE_UNKNOWN;
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- missing shaders rejected --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.vertex_spirv = nullptr;
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- missing colour format rejected --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.color_format = OPTICS_GFX_FORMAT_UNDEFINED;
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- push constants over device cap rejected --- */
    {
        optics_gfx_graphics_pipeline_desc desc = base_desc();
        desc.push_constant_bytes = 1u << 20; /* 1 MiB — no driver allows */
        optics_gfx_graphics_pipeline *p = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &desc, &p) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    }

    /* --- NULL-safe accessors and release --- */
    OT_EXPECT(optics_gfx_graphics_pipeline_vk_pipeline(nullptr) == VK_NULL_HANDLE);
    OT_EXPECT(optics_gfx_graphics_pipeline_vk_layout(nullptr) == VK_NULL_HANDLE);
    OT_EXPECT(optics_gfx_graphics_pipeline_retain(nullptr) == nullptr);
    optics_gfx_graphics_pipeline_release(nullptr);

    optics_gfx_device_release(d);
    return OT_REPORT();
}
