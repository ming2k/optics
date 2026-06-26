/*
 * flux_sampler lifecycle + bindless auto-registration.
 */
#include "test_helpers.h"
#include <flux/canvas.h>
#include <flux/flux.h>
#include <flux/vulkan.h>

int main(void) {
    /* --- NULL / wrong-tag rejection (no device) --- */
    {
        flux_sampler *s = nullptr;
        EXPECT(flux_sampler_create(nullptr, nullptr, &s) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(s == nullptr);
    }

    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_sampler: no Vulkan device; skipping\n");
        TEST_SUMMARY();
    }

    /* --- happy path: linear + clamp --- */
    {
        flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
        sd.min_filter = FLUX_FILTER_LINEAR;
        sd.mag_filter = FLUX_FILTER_LINEAR;
        sd.mipmap_mode = FLUX_FILTER_LINEAR;
        sd.address_u = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_v = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_w = FLUX_ADDRESS_CLAMP_TO_EDGE;
        sd.max_anisotropy = 1.0f;

        flux_sampler *s = nullptr;
        EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_OK);
        EXPECT(s != nullptr);
        EXPECT(flux_sampler_vk_sampler(s) != VK_NULL_HANDLE);
        EXPECT(flux_sampler_bindless_handle(s) != FLUX_BINDLESS_INVALID);

        /* retain/release pair shouldn't free. */
        EXPECT(flux_sampler_retain(s) == s);
        flux_sampler_release(s); /* drop the extra ref */
        flux_sampler_release(s); /* final release */
    }

    /* --- nearest + repeat (pixel-art style) --- */
    {
        flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
        sd.min_filter = FLUX_FILTER_NEAREST;
        sd.mag_filter = FLUX_FILTER_NEAREST;
        sd.mipmap_mode = FLUX_FILTER_NEAREST;
        sd.address_u = FLUX_ADDRESS_REPEAT;
        sd.address_v = FLUX_ADDRESS_REPEAT;
        sd.address_w = FLUX_ADDRESS_REPEAT;

        flux_sampler *s = nullptr;
        EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_OK);
        flux_sampler_release(s);
    }

    /* --- anisotropy clamped to device cap (passing 1e6 must not error) --- */
    {
        flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
        sd.min_filter = FLUX_FILTER_LINEAR;
        sd.mag_filter = FLUX_FILTER_LINEAR;
        sd.mipmap_mode = FLUX_FILTER_LINEAR;
        sd.address_u = FLUX_ADDRESS_REPEAT;
        sd.address_v = FLUX_ADDRESS_REPEAT;
        sd.address_w = FLUX_ADDRESS_REPEAT;
        sd.max_anisotropy = 1.0e6f;

        flux_sampler *s = nullptr;
        EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_OK);
        flux_sampler_release(s);
    }

    /* --- wrong sType rejected --- */
    {
        flux_sampler_desc sd = {.type = FLUX_TYPE_UNKNOWN};
        flux_sampler *s = nullptr;
        EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_ERROR_INVALID_ARGUMENT);
        EXPECT(s == nullptr);
    }

    /* --- NULL-safe accessors --- */
    EXPECT(flux_sampler_vk_sampler(nullptr) == VK_NULL_HANDLE);
    EXPECT(flux_sampler_bindless_handle(nullptr) == FLUX_BINDLESS_INVALID);
    EXPECT(flux_sampler_retain(nullptr) == nullptr);
    flux_sampler_release(nullptr);

    /* --- flux_canvas_draw_image_sampled NULL safety ---
     * Exhaustive coverage needs a recording canvas, which needs a
     * surface (no headless canvas path). The function must at
     * minimum tolerate NULL arguments without crashing — that is
     * the public contract documented for every draw call. */
    flux_canvas_draw_image_sampled(nullptr, nullptr, nullptr, (flux_rect){0, 0, 1, 1}, nullptr);

    flux_device_release(d);
    TEST_SUMMARY();
}
