/*
 * GPU-bound liquid-glass microbenchmark (ADR-0148 evidence).
 *
 * The `material.liquid_glass_apply` Tracy zone measures CPU *recording* latency: it
 * does not contain the dispatch's execution, and on a 64x64 target there is
 * almost no execution to contain. This harness renders many overlapping bodies
 * onto a large attachment and forces a device idle before stopping the clock, so
 * the reported figure is GPU work plus recording — what a frame actually costs.
 *
 * Not part of the test suite: it reports, it does not assert. Run it directly:
 *
 *     ./build/tests/material/integration/bench_liquid_glass_gpu [bodies]
 *
 * Output is one labelled line so a number can be quoted as evidence without
 * needing the invocation beside it. An unlabelled float is not evidence.
 */
#include <optics/gfx.h>
#include <optics/gfx/canvas.h>
#include <optics/gfx/effect.h>
#include <optics/material.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAX_BODIES 64u
#define BENCH_W 1024u
#define BENCH_H 1024u
#define WARMUP_FRAMES 3u
#define TIMED_FRAMES 60u

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

typedef struct bench {
    optics_gfx_device *device;
    optics_gfx_surface *surface;
    optics_gfx_canvas *canvas;
    optics_gfx_image *capture;
    optics_gfx_blur_filter *blur;
    optics_material_liquid_glass_filter *glass;
    optics_material_liquid_glass_shape shapes[MAX_BODIES];
    optics_material_liquid_glass_group groups[MAX_BODIES];
    uint32_t body_count;
} bench;

/* One frame's work, shared by warm-up and timing so the measured path is
 * literally the warmed path. Returns false on the first failure, so a partial
 * pipeline reports instead of silently timing nothing. */
static bool bench_frame(bench *b) {
    optics_gfx_frame *frame = nullptr;
    if (optics_gfx_surface_begin_frame(b->surface, nullptr, &frame) != OPTICS_GFX_OK)
        return false;

    optics_gfx_color bg = optics_gfx_color_rgba(20, 24, 40, 255);
    optics_gfx_canvas_pass_desc pass = {
        .type = OPTICS_GFX_TYPE_CANVAS_PASS_DESC,
        .frame = frame,
        .attachment = {.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = b->capture},
        .clear_color = &bg,
    };
    if (optics_gfx_canvas_begin(b->canvas, &pass) != OPTICS_GFX_OK)
        return false;
    optics_gfx_canvas_fill_rect_color(
        b->canvas, (optics_gfx_rect){0.0f, 0.0f, (float)BENCH_W / 2.0f, (float)BENCH_H},
        optics_gfx_color_rgba_premul(255, 255, 255, 255));
    if (optics_gfx_canvas_end(b->canvas) != OPTICS_GFX_OK)
        return false;

    optics_gfx_effect_blur_desc bd = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
    bd.input = b->capture;
    bd.sigma = 14.0f;
    optics_gfx_image *blurred = nullptr;
    if (optics_gfx_blur_filter_apply(
            b->blur, &(optics_gfx_blur_filter_apply_opts){
                         .frame = frame, .desc = &bd, .out = &blurred}) != OPTICS_GFX_OK)
        return false;

    /* The glass samples a deferred blur output, so materialize it before the
     * glass records its dispatches (ADR-0147). Without this the glass would
     * sample an unexecuted pass, which is a contract violation rather than a
     * slower frame. */
    if (optics_gfx_frame_materialize_image(frame, blurred) != OPTICS_GFX_OK)
        return false;

    optics_material_liquid_glass_desc gd = OPTICS_MATERIAL_LIQUID_GLASS_DESC_INIT;
    gd.input = b->capture;
    gd.blurred_input = blurred;
    gd.groups = b->groups;
    gd.group_count = b->body_count;
    optics_material_output out = {};
    if (optics_material_liquid_glass_filter_apply(
            b->glass, &(optics_material_liquid_glass_filter_apply_opts){
                          .frame = frame, .desc = &gd, .out = &out}) != OPTICS_GFX_OK)
        return false;

    if (optics_gfx_frame_submit(frame) != OPTICS_GFX_OK)
        return false;
    if (optics_gfx_frame_present(frame) != OPTICS_GFX_OK)
        return false;

    /* The clock must not stop until the GPU is done; present only queues. */
    optics_gfx_device_wait_idle(b->device);
    return true;
}

static void bench_destroy(bench *b) {
    if (b->glass)
        optics_material_liquid_glass_filter_release(b->glass);
    if (b->blur)
        optics_gfx_blur_filter_release(b->blur);
    if (b->canvas)
        optics_gfx_canvas_release(b->canvas);
    if (b->capture)
        optics_gfx_image_release(b->capture);
    if (b->surface)
        optics_gfx_surface_release(b->surface);
    if (b->device)
        optics_gfx_device_release(b->device);
    *b = (bench){};
}

static bool bench_init(bench *b, uint32_t body_count) {
    optics_gfx_device_desc dd = OPTICS_GFX_DEVICE_DESC_INIT;
    if (optics_gfx_device_create(&dd, &b->device) != OPTICS_GFX_OK)
        return false;

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = BENCH_W;
    sd.height = BENCH_H;
    if (optics_gfx_surface_create(b->device, &sd, &b->surface) != OPTICS_GFX_OK)
        return false;

    VkFormat sf = optics_gfx_surface_vk_format(b->surface);
    optics_gfx_format fmt = (sf == VK_FORMAT_B8G8R8A8_UNORM) ? OPTICS_GFX_FORMAT_BGRA8_UNORM
                                                             : OPTICS_GFX_FORMAT_RGBA8_UNORM;
    if (optics_gfx_image_create(
            b->device,
            &(optics_gfx_image_desc){.type = OPTICS_GFX_TYPE_IMAGE_DESC,
                                     .width = BENCH_W,
                                     .height = BENCH_H,
                                     .format = fmt,
                                     .usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET},
            &b->capture) != OPTICS_GFX_OK)
        return false;

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = b->surface};
    if (optics_gfx_canvas_create(&cd, &b->canvas) != OPTICS_GFX_OK)
        return false;
    if (optics_gfx_blur_filter_create(b->device, &b->blur) != OPTICS_GFX_OK)
        return false;
    if (optics_material_liquid_glass_filter_create(b->device, &b->glass) != OPTICS_GFX_OK)
        return false;
    if (optics_material_liquid_glass_filter_warmup(
            b->glass, &(optics_material_liquid_glass_warmup_opts){
                          .format = OPTICS_GFX_FORMAT_RGBA8_UNORM, .stats = true}) != OPTICS_GFX_OK)
        return false;

    b->body_count = body_count;
    const float bw = 150.0f, bh = 64.0f;
    for (uint32_t i = 0; i < body_count; ++i) {
        float x = (float)(i % 8u) * (bw + 12.0f) + 16.0f;
        float y = (float)(i / 8u) * (bh + 12.0f) + 16.0f;
        b->shapes[i] =
            (optics_material_liquid_glass_shape){.bounds = {x, y, bw, bh}, .corner_radius = 24.0f};
        b->groups[i] = (optics_material_liquid_glass_group)OPTICS_MATERIAL_LIQUID_GLASS_GROUP_INIT;
        b->groups[i].shapes = &b->shapes[i];
        b->groups[i].shape_count = 1;
        /* Half the bodies take the G2 squircle path and half the Euclid one, so
         * the figure covers both curvature branches rather than one. */
        b->groups[i].curvature = (i % 2u) ? 1.0f : 0.0f;
        b->groups[i].shadow_alpha = 0.3f;
        b->groups[i].shadow_blur = 20.0f;
        b->groups[i].shadow_offset_y = 6.0f;
    }
    return true;
}

int main(int argc, char **argv) {
    uint32_t body_count = MAX_BODIES;
    if (argc > 1) {
        unsigned long parsed = strtoul(argv[1], nullptr, 10);
        if (parsed == 0)
            parsed = MAX_BODIES;
        body_count = parsed > MAX_BODIES ? MAX_BODIES : (uint32_t)parsed;
    }

    bench b = {};
    if (!bench_init(&b, body_count)) {
        optics_gfx_error_info info = {};
        optics_gfx_get_last_error(&info);
        fprintf(stderr, "bench_liquid_glass_gpu: setup failed: %s\n",
                info.message ? info.message : "no detail");
        bench_destroy(&b);
        return 1;
    }

    for (uint32_t i = 0; i < WARMUP_FRAMES; ++i) {
        if (!bench_frame(&b)) {
            fprintf(stderr, "bench_liquid_glass_gpu: warm-up frame failed\n");
            bench_destroy(&b);
            return 1;
        }
    }

    double t0 = now_ms();
    for (uint32_t i = 0; i < TIMED_FRAMES; ++i) {
        if (!bench_frame(&b)) {
            fprintf(stderr, "bench_liquid_glass_gpu: timed frame %u failed\n", i);
            bench_destroy(&b);
            return 1;
        }
    }
    double t1 = now_ms();

    const double per_frame = (t1 - t0) / (double)TIMED_FRAMES;
    printf("liquid_glass_gpu: %.4f ms/frame  (%ux%u target, %u bodies, %u frames, "
           "GPU-idle fenced)\n",
           per_frame, BENCH_W, BENCH_H, body_count, TIMED_FRAMES);

    bench_destroy(&b);
    return 0;
}
