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

    /* --- Frame resource tracking and retirement (ADR-0090 / ADR-0092) --- */
    {
        flux_surface *surface = nullptr;
        flux_surface_desc sdesc = FLUX_SURFACE_DESC_INIT;
        sdesc.width = 64;
        sdesc.height = 64;
        EXPECT(flux_surface_create(d, &sdesc, &surface) == FLUX_OK);

        flux_canvas *canvas = nullptr;
        flux_canvas_desc cdesc = FLUX_CANVAS_DESC_INIT;
        cdesc.surface = surface;
        EXPECT(flux_canvas_create(&cdesc, &canvas) == FLUX_OK);

        uint32_t pixels[16 * 16];
        for (int i = 0; i < 16 * 16; ++i)
            pixels[i] = 0xFFFFFFFFu;
        flux_image_desc im_desc = FLUX_IMAGE_DESC_INIT;
        im_desc.width = 16;
        im_desc.height = 16;
        im_desc.format = FLUX_FORMAT_RGBA8_UNORM;
        im_desc.initial_data = pixels;
        flux_image *img = nullptr;
        EXPECT(flux_image_create(d, &im_desc, &img) == FLUX_OK);

        /* Case 1: Sampler released immediately after recording draw call.
         * The frame holds a reference until fence retirement. */
        {
            flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
            sd.min_filter = FLUX_FILTER_LINEAR;
            sd.mag_filter = FLUX_FILTER_LINEAR;
            flux_sampler *s = nullptr;
            EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_OK);

            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
            flux_color clear = flux_color_rgba(0, 0, 0, 255);
            EXPECT(flux_canvas_begin_frame(canvas, frame, &clear) == FLUX_OK);

            flux_canvas_draw_image_sampled(canvas, img, s, (flux_rect){0, 0, 32, 32}, nullptr);

            /* Caller drops ownership right away while frame is in-flight. */
            flux_sampler_release(s);

            flux_canvas_end_frame(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
        }

        /* Case 2: DisplayList with sampler submitted and released immediately.
         * The frame tracks the DisplayList and its samplers until completion. */
        {
            flux_sampler_desc sd = FLUX_SAMPLER_DESC_INIT;
            sd.min_filter = FLUX_FILTER_NEAREST;
            sd.mag_filter = FLUX_FILTER_NEAREST;
            flux_sampler *s = nullptr;
            EXPECT(flux_sampler_create(d, &sd, &s) == FLUX_OK);

            flux_encoder *enc = nullptr;
            EXPECT(flux_encoder_create(nullptr, &enc) == FLUX_OK);
            flux_geometry geom = flux_geom_rect((flux_rect){10, 10, 20, 20});
            flux_brush brush = {
                .kind = FLUX_BRUSH_IMAGE_PATTERN,
                .blend = FLUX_BLEND_SRC_OVER,
                .opacity = 1.0f,
                .image =
                    {
                        .image = img,
                        .sampler = s,
                        .src_rect = (flux_rect){0, 0, 16, 16},
                    },
            };
            flux_encoder_draw_geometry(enc, &geom, &brush);
            flux_sampler_release(s);

            flux_display_list *dl = nullptr;
            EXPECT(flux_encoder_finish(enc, &dl) == FLUX_OK);
            flux_encoder_destroy(enc);

            flux_frame *frame = nullptr;
            EXPECT(flux_surface_begin_frame(surface, nullptr, &frame) == FLUX_OK);
            flux_color clear = flux_color_rgba(0, 0, 0, 255);
            EXPECT(flux_canvas_begin_frame(canvas, frame, &clear) == FLUX_OK);

            EXPECT(flux_canvas_submit_display_list(canvas, dl) == FLUX_OK);
            /* Caller drops ownership right away while frame is in-flight. */
            flux_display_list_release(dl);

            flux_canvas_end_frame(canvas);
            EXPECT(flux_frame_submit(frame) == FLUX_OK);
            EXPECT(flux_frame_present(frame) == FLUX_OK);
        }

        flux_image_release(img);
        flux_canvas_release(canvas);
        flux_surface_release(surface);
    }

    flux_device_release(d);
    TEST_SUMMARY();
}
