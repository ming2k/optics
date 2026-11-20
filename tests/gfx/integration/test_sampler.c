/*
 * flux_sampler lifecycle + bindless auto-registration.
 */
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/vulkan.h>

int main(void) {
    /* --- NULL / wrong-tag rejection (no device) --- */
    {
        optics_gfx_sampler *s = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(nullptr, nullptr, &s) ==
                  OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(s == nullptr);
    }

    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_sampler: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    /* --- happy path: linear + clamp --- */
    {
        optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
        sd.min_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mag_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mipmap_mode = OPTICS_GFX_FILTER_LINEAR;
        sd.address_u = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_v = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sd.address_w = OPTICS_GFX_ADDRESS_CLAMP_TO_EDGE;
        sd.max_anisotropy = 1.0f;

        optics_gfx_sampler *s = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_OK);
        OT_EXPECT(s != nullptr);
        OT_EXPECT(optics_gfx_sampler_vk_sampler(s) != VK_NULL_HANDLE);
        OT_EXPECT(optics_gfx_sampler_bindless_handle(s) != OPTICS_GFX_BINDLESS_INVALID);

        /* retain/release pair shouldn't free. */
        OT_EXPECT(optics_gfx_sampler_retain(s) == s);
        optics_gfx_sampler_release(s); /* drop the extra ref */
        optics_gfx_sampler_release(s); /* final release */
    }

    /* --- nearest + repeat (pixel-art style) --- */
    {
        optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
        sd.min_filter = OPTICS_GFX_FILTER_NEAREST;
        sd.mag_filter = OPTICS_GFX_FILTER_NEAREST;
        sd.mipmap_mode = OPTICS_GFX_FILTER_NEAREST;
        sd.address_u = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_v = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_w = OPTICS_GFX_ADDRESS_REPEAT;

        optics_gfx_sampler *s = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_OK);
        optics_gfx_sampler_release(s);
    }

    /* --- anisotropy clamped to device cap (passing 1e6 must not error) --- */
    {
        optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
        sd.min_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mag_filter = OPTICS_GFX_FILTER_LINEAR;
        sd.mipmap_mode = OPTICS_GFX_FILTER_LINEAR;
        sd.address_u = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_v = OPTICS_GFX_ADDRESS_REPEAT;
        sd.address_w = OPTICS_GFX_ADDRESS_REPEAT;
        sd.max_anisotropy = 1.0e6f;

        optics_gfx_sampler *s = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_OK);
        optics_gfx_sampler_release(s);
    }

    /* --- wrong sType rejected --- */
    {
        optics_gfx_sampler_desc sd = {.type = OPTICS_GFX_TYPE_UNKNOWN};
        optics_gfx_sampler *s = nullptr;
        OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);
        OT_EXPECT(s == nullptr);
    }

    /* --- NULL-safe accessors --- */
    OT_EXPECT(optics_gfx_sampler_vk_sampler(nullptr) == VK_NULL_HANDLE);
    OT_EXPECT(optics_gfx_sampler_bindless_handle(nullptr) == OPTICS_GFX_BINDLESS_INVALID);
    OT_EXPECT(optics_gfx_sampler_retain(nullptr) == nullptr);
    optics_gfx_sampler_release(nullptr);

    /* --- flux_canvas_draw_image_sampled NULL safety ---
     * Exhaustive coverage needs a recording canvas, which needs a
     * surface (no headless canvas path). The function must at
     * minimum tolerate NULL arguments without crashing — that is
     * the public contract documented for every draw call. */
    optics_gfx_canvas_draw_image_sampled(nullptr, nullptr, nullptr, (optics_gfx_rect){0, 0, 1, 1},
                                         nullptr);

    /* --- Frame resource tracking and retirement (ADR-0090 / ADR-0092) --- */
    {
        optics_gfx_surface *surface = nullptr;
        optics_gfx_surface_desc sdesc = OPTICS_GFX_SURFACE_DESC_INIT;
        sdesc.width = 64;
        sdesc.height = 64;
        OT_EXPECT(optics_gfx_surface_create(d, &sdesc, &surface) == OPTICS_GFX_OK);

        optics_gfx_canvas *canvas = nullptr;
        optics_gfx_canvas_desc cdesc = OPTICS_GFX_CANVAS_DESC_INIT;
        cdesc.surface = surface;
        OT_EXPECT(optics_gfx_canvas_create(&cdesc, &canvas) == OPTICS_GFX_OK);

        uint32_t pixels[16 * 16];
        for (int i = 0; i < 16 * 16; ++i)
            pixels[i] = 0xFFFFFFFFu;
        optics_gfx_image_desc im_desc = OPTICS_GFX_IMAGE_DESC_INIT;
        im_desc.width = 16;
        im_desc.height = 16;
        im_desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
        im_desc.initial_data = pixels;
        optics_gfx_image *img = nullptr;
        OT_EXPECT(optics_gfx_image_create(d, &im_desc, &img) == OPTICS_GFX_OK);

        /* Case 1: Sampler released immediately after recording draw call.
         * The frame holds a reference until fence retirement. */
        {
            optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
            sd.min_filter = OPTICS_GFX_FILTER_LINEAR;
            sd.mag_filter = OPTICS_GFX_FILTER_LINEAR;
            optics_gfx_sampler *s = nullptr;
            OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_OK);

            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
            optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);

            optics_gfx_canvas_draw_image_sampled(canvas, img, s, (optics_gfx_rect){0, 0, 32, 32},
                                                 nullptr);

            /* Caller drops ownership right away while frame is in-flight. */
            optics_gfx_sampler_release(s);

            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        }

        /* Case 2: DisplayList with sampler submitted and released immediately.
         * The frame tracks the DisplayList and its samplers until completion. */
        {
            optics_gfx_sampler_desc sd = OPTICS_GFX_SAMPLER_DESC_INIT;
            sd.min_filter = OPTICS_GFX_FILTER_NEAREST;
            sd.mag_filter = OPTICS_GFX_FILTER_NEAREST;
            optics_gfx_sampler *s = nullptr;
            OT_EXPECT(optics_gfx_sampler_create(d, &sd, &s) == OPTICS_GFX_OK);

            optics_gfx_encoder *enc = nullptr;
            OT_EXPECT(optics_gfx_encoder_create(nullptr, &enc) == OPTICS_GFX_OK);
            optics_gfx_geometry geom = optics_gfx_geom_rect((optics_gfx_rect){10, 10, 20, 20});
            optics_gfx_brush brush = {
                .kind = OPTICS_GFX_BRUSH_IMAGE_PATTERN,
                .blend = OPTICS_GFX_BLEND_SRC_OVER,
                .opacity = 1.0f,
                .image =
                    {
                        .image = img,
                        .sampler = s,
                        .src_rect = (optics_gfx_rect){0, 0, 16, 16},
                    },
            };
            optics_gfx_encoder_draw_geometry(
                enc, &(optics_gfx_encoder_draw_geometry_opts){.geom = &geom, .brush = &brush});
            optics_gfx_sampler_release(s);

            optics_gfx_display_list *dl = nullptr;
            OT_EXPECT(optics_gfx_encoder_finish(enc, &dl) == OPTICS_GFX_OK);
            optics_gfx_encoder_destroy(enc);

            optics_gfx_frame *frame = nullptr;
            OT_EXPECT(optics_gfx_surface_begin_frame(surface, nullptr, &frame) == OPTICS_GFX_OK);
            optics_gfx_color clear = optics_gfx_color_rgba(0, 0, 0, 255);
            OT_EXPECT(optics_gfx_canvas_begin(canvas, &(optics_gfx_canvas_pass_desc){
                                                          .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
                                                          .frame = frame,
                                                          .clear_color = &clear}) == OPTICS_GFX_OK);

            OT_EXPECT(optics_gfx_canvas_submit_display_list(
                          canvas, &(optics_gfx_canvas_submit_display_list_opts){.list = dl}) ==
                      OPTICS_GFX_OK);
            /* Caller drops ownership right away while frame is in-flight. */
            optics_gfx_display_list_release(dl);

            OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
            OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        }

        optics_gfx_image_release(img);
        optics_gfx_canvas_release(canvas);
        optics_gfx_surface_release(surface);
    }

    optics_gfx_device_release(d);
    return OT_REPORT();
}
