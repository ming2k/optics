/*
 * ADR-0089 1B-2B: uploads and Canvas sampled-image reads are stated in the
 * frame's RenderPlan while the existing backend remains authoritative.
 *
 * This is the production wiring test, not an IR unit test: the nodes are
 * created by flux_image_create, flux_image_update_region and a Canvas draw,
 * then inspected through the frame's plan.
 */
#include "../../../libs/flux/src/core/internal.h"
#include "test_helpers.h"
#include <flux/flux.h>

#include <string.h>

#define W 128u
#define H 96u

static int count_kind(const flux_render_plan *plan, flux_render_node_kind kind) {
    int n = 0;
    for (uint32_t i = 0; i < plan->node_count; ++i)
        if (plan->nodes[i].kind == kind)
            n++;
    return n;
}

static int find_kind(const flux_render_plan *plan, flux_render_node_kind kind) {
    for (uint32_t i = 0; i < plan->node_count; ++i)
        if (plan->nodes[i].kind == kind)
            return (int)i;
    return -1;
}

static bool node_depends_on(const flux_render_plan *plan, uint32_t node, uint32_t producer) {
    const flux_render_node *n = &plan->nodes[node];
    for (uint32_t i = 0; i < n->dependency_count; ++i)
        if (n->dependencies[i] == producer)
            return true;
    return false;
}

static bool node_reads(const flux_render_plan *plan, uint32_t node, const void *handle) {
    const flux_render_node *n = &plan->nodes[node];
    for (uint32_t i = 0; i < n->access_count; ++i) {
        const flux_render_access *a = &n->accesses[i];
        if (a->resource.kind == FLUX_RENDER_RESOURCE_IMAGE && a->resource.handle == handle &&
            a->mode != FLUX_RENDER_ACCESS_WRITE)
            return true;
    }
    return false;
}

int main(void) {
    flux_device *d = test_helpers_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_render_plan_frame: no Vulkan device; skipping\n");
        TEST_SUMMARY();
        return 0;
    }

    flux_surface_desc sd = FLUX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    flux_surface *s = nullptr;
    EXPECT(flux_surface_create(d, &sd, &s) == FLUX_OK);

    flux_canvas_desc cd = {.type = FLUX_TYPE_CANVAS_DESC, .surface = s};
    flux_canvas *canvas = nullptr;
    EXPECT(flux_canvas_create(&cd, &canvas) == FLUX_OK);

    flux_frame *frame = nullptr;
    EXPECT(flux_surface_begin_frame(s, nullptr, &frame) == FLUX_OK);
    EXPECT(frame != nullptr);

    /* An image created while the frame records is a managed upload: the plan
     * must state the write and the staging read. */
    static uint32_t pixels[16 * 16];
    for (uint32_t i = 0; i < 16 * 16; ++i)
        pixels[i] = 0xFF336699u;
    flux_image_desc id = FLUX_IMAGE_DESC_INIT;
    id.width = 16;
    id.height = 16;
    id.format = FLUX_FORMAT_RGBA8_UNORM;
    id.initial_data = pixels;
    flux_image *img = nullptr;
    EXPECT(flux_image_create(d, &id, &img) == FLUX_OK);
    EXPECT(count_kind(&frame->render_plan, FLUX_RENDER_NODE_UPLOAD) == 1);

    const int first_upload = find_kind(&frame->render_plan, FLUX_RENDER_NODE_UPLOAD);
    EXPECT(first_upload >= 0);
    const flux_render_node *upload = &frame->render_plan.nodes[first_upload];
    bool writes_image = false;
    bool reads_staging = false;
    for (uint32_t i = 0; i < upload->access_count; ++i) {
        const flux_render_access *a = &upload->accesses[i];
        if (a->resource.kind == FLUX_RENDER_RESOURCE_IMAGE &&
            a->resource.handle == (const void *)img && a->mode == FLUX_RENDER_ACCESS_WRITE)
            writes_image = true;
        if (a->resource.kind == FLUX_RENDER_RESOURCE_BUFFER && a->mode == FLUX_RENDER_ACCESS_READ &&
            a->resource.handle)
            reads_staging = true;
    }
    EXPECT(writes_image);
    EXPECT(reads_staging);

    /* A second upload to the same image is a second writer: it must order
     * itself after the first, not silently overwrite it. */
    EXPECT(flux_image_update_region(img, 0, 0, 16, 16, pixels, sizeof(pixels)) == FLUX_OK);
    EXPECT(count_kind(&frame->render_plan, FLUX_RENDER_NODE_UPLOAD) == 2);
    int second_upload = -1;
    for (uint32_t i = 0; i < frame->render_plan.node_count; ++i)
        if (frame->render_plan.nodes[i].kind == FLUX_RENDER_NODE_UPLOAD && (int)i != first_upload)
            second_upload = (int)i;
    EXPECT(second_upload >= 0);
    EXPECT(node_depends_on(&frame->render_plan, (uint32_t)second_upload, (uint32_t)first_upload));

    /* A Canvas draw samples the uploaded image. The Canvas node must name the
     * producing upload, and the plan must still validate before the pass ends. */
    flux_canvas_pass_desc pd = FLUX_CANVAS_PASS_DESC_INIT;
    pd.frame = frame;
    EXPECT(flux_canvas_begin(canvas, &pd) == FLUX_OK);
    flux_canvas_draw_image(canvas, img, (flux_rect){0, 0, 16, 16}, nullptr);
    const int canvas_node = find_kind(&frame->render_plan, FLUX_RENDER_NODE_CANVAS);
    EXPECT(canvas_node >= 0);
    EXPECT(node_reads(&frame->render_plan, (uint32_t)canvas_node, (const void *)img));
    EXPECT(node_depends_on(&frame->render_plan, (uint32_t)canvas_node, (uint32_t)second_upload));

    /* A later upload (the second text batch of one pass, for example) is
     * submitted before the frame command buffer, so the Canvas that already
     * sampled this image must be ordered after it rather than reject the
     * write as an unordered overwrite. */
    EXPECT(flux_image_update_region(img, 0, 0, 16, 16, pixels, sizeof(pixels)) == FLUX_OK);
    EXPECT(count_kind(&frame->render_plan, FLUX_RENDER_NODE_UPLOAD) == 3);
    int third_upload = -1;
    for (uint32_t i = 0; i < frame->render_plan.node_count; ++i)
        if (frame->render_plan.nodes[i].kind == FLUX_RENDER_NODE_UPLOAD && (int)i != first_upload &&
            (int)i != second_upload)
            third_upload = (int)i;
    EXPECT(third_upload >= 0);
    EXPECT(node_depends_on(&frame->render_plan, (uint32_t)canvas_node, (uint32_t)third_upload));

    EXPECT(flux_canvas_end(canvas) == FLUX_OK);

    /* The plan is validated by canvas end and again at submit; a hidden or
     * unordered read would have failed one of those gates above. */

    EXPECT(flux_frame_submit(frame) == FLUX_OK);
    EXPECT(flux_frame_present(frame) == FLUX_OK);

    flux_image_release(img);
    flux_canvas_release(canvas);
    flux_surface_release(s);
    flux_device_release(d);
    TEST_SUMMARY();
}
