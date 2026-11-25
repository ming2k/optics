/*
 * Ordering between Canvas target passes and deferred effects (ADR-0089,
 * ADR-0147).
 *
 * A compositor renders a capture with more than one Canvas pass, blurs it, and
 * may reuse one scratch image across several effects in a frame (a shadow mask
 * per window). Each of those shapes must produce the pixels the eager order
 * would produce, and a rejection must say why:
 *
 *   1. An effect sampling an image written by two passes depends on the
 *      *last* writer, and the frame keeps recording.
 *   2. A pass may overwrite an image an earlier effect read (write-after-read),
 *      because it is ordered after that effect.
 *   3. A deferred effect whose input is overwritten before it is materialized
 *      still sees its input as of `apply`: deferral never changes pixels.
 *   4. A pass the RenderPlan rejects leaves a diagnostic in the last-error slot.
 */
#include "../../../libs/gfx/src/core/image_internal.h"
#include "../../../libs/gfx/src/core/internal.h"
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>
#include <optics/gfx/effect.h>

#include <stdlib.h>
#include <string.h>

#define W 64u
#define H 64u
#define BYTES (W * H * 4u)

static optics_gfx_image *make_target(optics_gfx_device *d) {
    optics_gfx_image_desc desc = OPTICS_GFX_IMAGE_DESC_INIT;
    desc.width = W;
    desc.height = H;
    desc.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    desc.usage = OPTICS_GFX_IMAGE_USAGE_RENDER_TARGET;
    optics_gfx_image *image = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &desc, &image) == OPTICS_GFX_OK);
    return image;
}

/* One full-extent pass into `image` that fills it with `grey`. */
static optics_gfx_result fill_target(optics_gfx_canvas *canvas, optics_gfx_frame *frame,
                                     optics_gfx_image *image, uint8_t grey) {
    optics_gfx_color clear = optics_gfx_color_rgba(grey, grey, grey, 255);
    optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pass.frame = frame;
    pass.attachment =
        (optics_gfx_canvas_attachment){.kind = OPTICS_GFX_CANVAS_ATTACHMENT_IMAGE, .image = image};
    pass.clear_color = &clear;
    optics_gfx_result r = optics_gfx_canvas_begin(canvas, &pass);
    if (r != OPTICS_GFX_OK)
        return r;
    return optics_gfx_canvas_end(canvas);
}

static optics_gfx_image *blur(optics_gfx_blur_filter *filter, optics_gfx_frame *frame,
                              optics_gfx_image *input) {
    optics_gfx_effect_blur_desc desc = OPTICS_GFX_EFFECT_BLUR_DESC_INIT;
    desc.input = input;
    desc.sigma = 4.0f;
    optics_gfx_image *out = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_apply(
                  filter, &(optics_gfx_blur_filter_apply_opts){
                              .frame = frame, .desc = &desc, .out = &out}) == OPTICS_GFX_OK);
    return out;
}

static int writer_count(const optics_gfx_render_plan *plan, const void *handle) {
    int n = 0;
    for (uint32_t i = 0; i < plan->node_count; ++i)
        for (uint32_t a = 0; a < plan->nodes[i].access_count; ++a) {
            const optics_gfx_render_access *access = &plan->nodes[i].accesses[a];
            if (access->resource.handle == handle && access->mode != OPTICS_GFX_RENDER_ACCESS_READ)
                n++;
        }
    return n;
}

static uint32_t last_writer(const optics_gfx_render_plan *plan, const void *handle) {
    for (uint32_t i = plan->node_count; i-- > 0;)
        for (uint32_t a = 0; a < plan->nodes[i].access_count; ++a) {
            const optics_gfx_render_access *access = &plan->nodes[i].accesses[a];
            if (access->resource.handle == handle && access->mode != OPTICS_GFX_RENDER_ACCESS_READ)
                return i;
        }
    return UINT32_MAX;
}

static int effect_reading(const optics_gfx_render_plan *plan, const void *handle) {
    for (uint32_t i = plan->node_count; i-- > 0;) {
        const optics_gfx_render_node *n = &plan->nodes[i];
        if (n->kind != OPTICS_GFX_RENDER_NODE_EFFECT)
            continue;
        for (uint32_t a = 0; a < n->access_count; ++a)
            if (n->accesses[a].resource.handle == handle &&
                n->accesses[a].mode == OPTICS_GFX_RENDER_ACCESS_READ)
                return (int)i;
    }
    return -1;
}

static bool depends_directly(const optics_gfx_render_plan *plan, uint32_t node, uint32_t dep) {
    for (uint32_t i = 0; i < plan->nodes[node].dependency_count; ++i)
        if (plan->nodes[node].dependencies[i] == dep)
            return true;
    return false;
}

/* Draw `image` over the whole surface, submit, and return the centre's red. */
static uint8_t present_centre(optics_gfx_surface *s, optics_gfx_canvas *canvas,
                              optics_gfx_frame *frame, optics_gfx_image *image, uint8_t *px) {
    optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
    optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pass.frame = frame;
    pass.clear_color = &black;
    OT_EXPECT(optics_gfx_canvas_begin(canvas, &pass) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_image(canvas, image, (optics_gfx_rect){0, 0, (float)W, (float)H},
                                 nullptr);
    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    memset(px, 0, BYTES);
    OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
    return px[((H / 2) * W + W / 2) * 4];
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_plan_effect_order: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }
    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(
                  &(optics_gfx_canvas_desc){.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s},
                  &canvas) == OPTICS_GFX_OK);
    optics_gfx_blur_filter *filter_a = nullptr;
    optics_gfx_blur_filter *filter_b = nullptr;
    OT_EXPECT(optics_gfx_blur_filter_create(d, &filter_a) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_blur_filter_create(d, &filter_b) == OPTICS_GFX_OK);
    optics_gfx_image *capture = make_target(d);
    uint8_t *px = calloc(1, BYTES);
    OT_EXPECT(px != nullptr);

    /* ---- 1. Two writers, then an effect: it binds to the last one ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(fill_target(canvas, frame, capture, 40) == OPTICS_GFX_OK);
        OT_EXPECT(fill_target(canvas, frame, capture, 200) == OPTICS_GFX_OK);
        const void *handle = optics_gfx_image_plan_handle(capture);
        OT_EXPECT(writer_count(&frame->render_plan, handle) == 2);
        uint32_t last = last_writer(&frame->render_plan, handle);

        optics_gfx_image *blurred = blur(filter_a, frame, capture);
        int effect = effect_reading(&frame->render_plan, handle);
        OT_EXPECT(effect >= 0);
        OT_EXPECT(depends_directly(&frame->render_plan, (uint32_t)effect, last));
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);
        /* The second pass's content is what the blur saw. */
        uint8_t centre = present_centre(s, canvas, frame, blurred, px);
        OT_EXPECT(centre > 180 && centre < 220);
    }

    /* ---- 2. Write-after-read: a scratch image reused by two effects ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(fill_target(canvas, frame, capture, 40) == OPTICS_GFX_OK);
        optics_gfx_image *first = blur(filter_a, frame, capture);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, first) == OPTICS_GFX_OK);
        /* The pass that overwrites the scratch is ordered after the blur that
         * read it; the plan must accept it. */
        OT_EXPECT(fill_target(canvas, frame, capture, 200) == OPTICS_GFX_OK);
        optics_gfx_image *second = blur(filter_b, frame, capture);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, second) == OPTICS_GFX_OK);

        /* Each blur kept its own input. */
        optics_gfx_color black = optics_gfx_color_rgba(0, 0, 0, 255);
        optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
        pass.frame = frame;
        pass.clear_color = &black;
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &pass) == OPTICS_GFX_OK);
        optics_gfx_canvas_draw_image(canvas, first, (optics_gfx_rect){0, 0, W / 2.0f, (float)H},
                                     nullptr);
        optics_gfx_canvas_draw_image(canvas, second,
                                     (optics_gfx_rect){W / 2.0f, 0, W / 2.0f, (float)H}, nullptr);
        OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_surface_read_pixels(s, px, BYTES) == OPTICS_GFX_OK);
        uint8_t left = px[((H / 2) * W + W / 4) * 4];
        uint8_t right = px[((H / 2) * W + 3 * W / 4) * 4];
        OT_EXPECT(left > 20 && left < 60);
        OT_EXPECT(right > 180 && right < 220);
    }

    /* ---- 3. Overwritten before materialized: deferral keeps `apply`'s input ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        OT_EXPECT(fill_target(canvas, frame, capture, 40) == OPTICS_GFX_OK);
        optics_gfx_image *blurred = blur(filter_a, frame, capture);
        /* Not materialized yet: the overwrite must not leak into the blur. */
        OT_EXPECT(fill_target(canvas, frame, capture, 200) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_materialize_image(frame, blurred) == OPTICS_GFX_OK);
        uint8_t centre = present_centre(s, canvas, frame, blurred, px);
        OT_EXPECT(centre > 20 && centre < 60);
    }

    /* ---- 4. A rejected pass says why ---- */
    {
        optics_gfx_frame *frame = nullptr;
        OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
        /* Corrupt the plan the way an unordered producer would: a node that
         * reads the surface's capture with no edge to its writer. */
        OT_EXPECT(fill_target(canvas, frame, capture, 40) == OPTICS_GFX_OK);
        optics_gfx_render_access read = {
            .resource = {OPTICS_GFX_RENDER_RESOURCE_IMAGE, optics_gfx_image_plan_handle(capture),
                         0},
            .mode = OPTICS_GFX_RENDER_ACCESS_READ,
            .region = {.full = true},
        };
        optics_gfx_render_node *bad = &frame->render_plan.nodes[frame->render_plan.node_count++];
        memset(bad, 0, sizeof(*bad));
        bad->kind = OPTICS_GFX_RENDER_NODE_COMPUTE;
        bad->label = "test.unordered";
        bad->accesses[0] = read;
        bad->access_count = 1;
        optics_gfx_canvas_pass_desc pass = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
        pass.frame = frame;
        OT_EXPECT(optics_gfx_canvas_begin(canvas, &pass) == OPTICS_GFX_ERROR_INVALID_STATE);
        optics_gfx_error_info info = {};
        optics_gfx_get_last_error(&info);
        OT_EXPECT(info.code == OPTICS_GFX_ERROR_INVALID_STATE);
        OT_EXPECT(info.message != nullptr && strstr(info.message, "test.unordered") != nullptr);
        frame->render_plan.node_count--;
        OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
        OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);
    }

    free(px);
    optics_gfx_image_release(capture);
    optics_gfx_blur_filter_release(filter_b);
    optics_gfx_blur_filter_release(filter_a);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
