/*
 * ADR-0089 1B-2B: uploads and Canvas sampled-image reads are stated in the
 * frame's RenderPlan while the existing backend remains authoritative.
 *
 * This is the production wiring test, not an IR unit test: the nodes are
 * created by flux_image_create, flux_image_update_region and a Canvas draw,
 * then inspected through the frame's plan.
 */
#include "../../../libs/gfx/src/core/internal.h"
#include "gfx_test.h"
#include "optics_test.h"
#include <optics/gfx.h>

#include <string.h>

#define W 128u
#define H 96u

static int count_kind(const optics_gfx_render_plan *plan, optics_gfx_render_node_kind kind) {
    int n = 0;
    for (uint32_t i = 0; i < plan->node_count; ++i)
        if (plan->nodes[i].kind == kind)
            n++;
    return n;
}

static int find_kind(const optics_gfx_render_plan *plan, optics_gfx_render_node_kind kind) {
    for (uint32_t i = 0; i < plan->node_count; ++i)
        if (plan->nodes[i].kind == kind)
            return (int)i;
    return -1;
}

static bool node_depends_on(const optics_gfx_render_plan *plan, uint32_t node, uint32_t producer) {
    const optics_gfx_render_node *n = &plan->nodes[node];
    for (uint32_t i = 0; i < n->dependency_count; ++i)
        if (n->dependencies[i] == producer)
            return true;
    return false;
}

static bool node_reads(const optics_gfx_render_plan *plan, uint32_t node, const void *handle) {
    const optics_gfx_render_node *n = &plan->nodes[node];
    for (uint32_t i = 0; i < n->access_count; ++i) {
        const optics_gfx_render_access *a = &n->accesses[i];
        if (a->resource.kind == OPTICS_GFX_RENDER_RESOURCE_IMAGE && a->resource.handle == handle &&
            a->mode != OPTICS_GFX_RENDER_ACCESS_WRITE)
            return true;
    }
    return false;
}

int main(void) {
    optics_gfx_device *d = optics_gfx_test_make_headless_device();
    if (!d) {
        fprintf(stderr, "test_render_plan_frame: no Vulkan device; skipping\n");
        OT_SKIP("no Vulkan device");
    }

    optics_gfx_surface_desc sd = OPTICS_GFX_SURFACE_DESC_INIT;
    sd.width = W;
    sd.height = H;
    optics_gfx_surface *s = nullptr;
    OT_EXPECT(optics_gfx_surface_create(d, &sd, &s) == OPTICS_GFX_OK);

    optics_gfx_canvas_desc cd = {.type = OPTICS_GFX_TYPE_CANVAS_DESC, .surface = s};
    optics_gfx_canvas *canvas = nullptr;
    OT_EXPECT(optics_gfx_canvas_create(&cd, &canvas) == OPTICS_GFX_OK);

    optics_gfx_frame *frame = nullptr;
    OT_EXPECT(optics_gfx_surface_begin_frame(s, nullptr, &frame) == OPTICS_GFX_OK);
    OT_EXPECT(frame != nullptr);

    /* An image created while the frame records is a managed upload: the plan
     * must state the write and the staging read. */
    static uint32_t pixels[16 * 16];
    for (uint32_t i = 0; i < 16 * 16; ++i)
        pixels[i] = 0xFF336699u;
    optics_gfx_image_desc id = OPTICS_GFX_IMAGE_DESC_INIT;
    id.width = 16;
    id.height = 16;
    id.format = OPTICS_GFX_FORMAT_RGBA8_UNORM;
    id.initial_data = pixels;
    optics_gfx_image *img = nullptr;
    OT_EXPECT(optics_gfx_image_create(d, &id, &img) == OPTICS_GFX_OK);
    OT_EXPECT(count_kind(&frame->render_plan, OPTICS_GFX_RENDER_NODE_UPLOAD) == 1);

    const int first_upload = find_kind(&frame->render_plan, OPTICS_GFX_RENDER_NODE_UPLOAD);
    OT_EXPECT(first_upload >= 0);
    const optics_gfx_render_node *upload = &frame->render_plan.nodes[first_upload];
    bool writes_image = false;
    bool reads_staging = false;
    for (uint32_t i = 0; i < upload->access_count; ++i) {
        const optics_gfx_render_access *a = &upload->accesses[i];
        if (a->resource.kind == OPTICS_GFX_RENDER_RESOURCE_IMAGE &&
            a->resource.handle == (const void *)img && a->mode == OPTICS_GFX_RENDER_ACCESS_WRITE)
            writes_image = true;
        if (a->resource.kind == OPTICS_GFX_RENDER_RESOURCE_BUFFER &&
            a->mode == OPTICS_GFX_RENDER_ACCESS_READ && a->resource.handle)
            reads_staging = true;
    }
    OT_EXPECT(writes_image);
    OT_EXPECT(reads_staging);

    /* A second upload to the same image is a second writer: it must order
     * itself after the first, not silently overwrite it. */
    OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                      .x = 0,
                                                      .y = 0,
                                                      .width = 16,
                                                      .height = 16,
                                                      .data = pixels,
                                                      .bytes = sizeof(pixels)}) == OPTICS_GFX_OK);
    OT_EXPECT(count_kind(&frame->render_plan, OPTICS_GFX_RENDER_NODE_UPLOAD) == 2);
    int second_upload = -1;
    for (uint32_t i = 0; i < frame->render_plan.node_count; ++i)
        if (frame->render_plan.nodes[i].kind == OPTICS_GFX_RENDER_NODE_UPLOAD &&
            (int)i != first_upload)
            second_upload = (int)i;
    OT_EXPECT(second_upload >= 0);
    OT_EXPECT(
        node_depends_on(&frame->render_plan, (uint32_t)second_upload, (uint32_t)first_upload));

    /* A Canvas draw samples the uploaded image. The Canvas node must name the
     * producing upload, and the plan must still validate before the pass ends. */
    optics_gfx_canvas_pass_desc pd = OPTICS_GFX_CANVAS_PASS_DESC_INIT;
    pd.frame = frame;
    OT_EXPECT(optics_gfx_canvas_begin(canvas, &pd) == OPTICS_GFX_OK);
    optics_gfx_canvas_draw_image(canvas, img, (optics_gfx_rect){0, 0, 16, 16}, nullptr);
    const int canvas_node = find_kind(&frame->render_plan, OPTICS_GFX_RENDER_NODE_CANVAS);
    OT_EXPECT(canvas_node >= 0);
    OT_EXPECT(node_reads(&frame->render_plan, (uint32_t)canvas_node, (const void *)img));
    OT_EXPECT(node_depends_on(&frame->render_plan, (uint32_t)canvas_node, (uint32_t)second_upload));

    /* A later upload (the second text batch of one pass, for example) is
     * submitted before the frame command buffer, so the Canvas that already
     * sampled this image must be ordered after it rather than reject the
     * write as an unordered overwrite. */
    OT_EXPECT(optics_gfx_image_update_region(img, &(optics_gfx_image_update_region_opts){
                                                      .x = 0,
                                                      .y = 0,
                                                      .width = 16,
                                                      .height = 16,
                                                      .data = pixels,
                                                      .bytes = sizeof(pixels)}) == OPTICS_GFX_OK);
    OT_EXPECT(count_kind(&frame->render_plan, OPTICS_GFX_RENDER_NODE_UPLOAD) == 3);
    int third_upload = -1;
    for (uint32_t i = 0; i < frame->render_plan.node_count; ++i)
        if (frame->render_plan.nodes[i].kind == OPTICS_GFX_RENDER_NODE_UPLOAD &&
            (int)i != first_upload && (int)i != second_upload)
            third_upload = (int)i;
    OT_EXPECT(third_upload >= 0);
    OT_EXPECT(node_depends_on(&frame->render_plan, (uint32_t)canvas_node, (uint32_t)third_upload));

    OT_EXPECT(optics_gfx_canvas_end(canvas) == OPTICS_GFX_OK);

    /* The plan is validated by canvas end and again at submit; a hidden or
     * unordered read would have failed one of those gates above. */

    OT_EXPECT(optics_gfx_frame_submit(frame) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_frame_present(frame) == OPTICS_GFX_OK);

    optics_gfx_image_release(img);
    optics_gfx_canvas_release(canvas);
    optics_gfx_surface_release(s);
    optics_gfx_device_release(d);
    return OT_REPORT();
}
