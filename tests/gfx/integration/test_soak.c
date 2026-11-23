/*
 * Soak test: long-running create/destroy churn for every refcounted
 * resource. Catches bindless-slot drift, allocator drift, and
 * (when run under ASAN with detect_leaks=1) any persistent leak that
 * a per-test single-shot run would miss.
 *
 * Per-iteration: one image, one buffer (host-visible), one sampler,
 * one graphics pipeline. Across 256 iterations: 1024 bindless
 * register/release pairs total, exercising the slot pool. Skips if
 * no Vulkan device.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

#include <stdalign.h>
#include <string.h>

/* Shader bytecode, baked in with C23 #embed (--embed-dir is the build dir). */
alignas(uint32_t) static const unsigned char vert_spv[] = {
#embed "triangle.vert.spv"
};
alignas(uint32_t) static const unsigned char frag_spv[] = {
#embed "triangle.frag.spv"
};

#define ITERATIONS 256
#define IMG_W 8
#define IMG_H 8

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_soak: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    uint32_t pixels[IMG_W * IMG_H];
    memset(pixels, 0xAB, sizeof(pixels));

    for (int i = 0; i < ITERATIONS; ++i) {
        /* Image — exercises bindless SAMPLED_IMAGE slot + memory alloc. */
        optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
        id.width = IMG_W;
        id.height = IMG_H;
        id.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        id.initial_data = pixels;
        optics_gfx_image *img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &id, &img) == OPTICS_GFX_OK);

        /* Sampler — exercises bindless SAMPLER slot. */
        optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
        sd.min_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mag_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mipmap_mode = OPTICS_GFX_FILTER_LINEAR;
        sd.address_u = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_v = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_w = OPTICS_GFX_ADDRESS_REPEAT;
        optics_gfx_sampler *smp = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sd, &smp) == OPTICS_GFX_OK);

        /* Buffer — host-visible w/ BDA. */
        optics_gfx_buffer_desc bd = OPTICS_GFX_BUFFER_DESC_INIT;
        bd.size = 4096;
        bd.usage = OPTICS_GFX_BUFFER_USAGE_STORAGE;
        bd.location = OPTICS_GFX_BUFFER_HOST_VISIBLE;
        bd.device_address = true;
        optics_gfx_buffer *buf = nullptr;
        OT_EXPECT(optics_gfx_buffer_create(d, &bd, &buf) == OPTICS_GFX_OK);

        /* Graphics pipeline — exercises shader-module path + pipeline cache. */
        optics_gfx_graphics_pipeline_desc pd = OPTICS_GFX_GRAPHICS_PIPELINE_DESC_INIT;
        pd.vertex_spirv = (const uint32_t *)vert_spv;
        pd.vertex_spirv_word_count = sizeof(vert_spv) / sizeof(uint32_t);
        pd.fragment_spirv = (const uint32_t *)frag_spv;
        pd.fragment_spirv_word_count = sizeof(frag_spv) / sizeof(uint32_t);
        pd.topology = OPTICS_GFX_TOPOLOGY_TRIANGLE_LIST;
        pd.cull = OPTICS_GFX_CULL_NONE;
        pd.blend = OPTICS_GFX_BLEND_PRESET_NONE;
        pd.depth = OPTICS_GFX_DEPTH_NONE;
        pd.color_format = OPTICS_GFX_FORMAT_BGRA8_UNORM;
        pd.depth_format = OPTICS_GFX_FORMAT_UNDEFINED;
        optics_gfx_graphics_pipeline *gp = nullptr;
        OT_EXPECT(optics_gfx_graphics_pipeline_create(d, &pd, &gp) == OPTICS_GFX_OK);

        /* Release in reverse — verifies the orderings don't snag. */
        optics_gfx_graphics_pipeline_release(gp);
        optics_gfx_buffer_release(buf);
        optics_gfx_sampler_release(smp);
        optics_gfx_image_release(img);

        /* Belt-and-braces: every 64 iterations, also exercise an
         * in-flight retain to make sure refcount ordering survives
         * the cycle. */
        if ((i & 63) == 0) {
            optics_gfx_image_desc id2 = OPTICS_GFX_IMAGE_DESC_INIT;
            id2.width = IMG_W;
            id2.height = IMG_H;
            id2.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
            optics_gfx_image *im2 = nullptr;
            OT_EXPECT(optics_gfx_image_create(d, &id2, &im2) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_image_retain(im2) == im2);
            optics_gfx_image_release(im2);
            optics_gfx_image_release(im2);
        }
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
