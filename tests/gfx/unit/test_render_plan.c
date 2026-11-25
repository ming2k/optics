#include "../../../libs/gfx/src/core/render_plan.h"
#include "optics_test.h"

#include <string.h>

static optics_gfx_render_target target(void) {
    return (optics_gfx_render_target){
        .width = 128,
        .height = 64,
        .scale = 1.0f,
        .format = OPTICS_GFX_FORMAT_RGBA8_UNORM,
        .color_space = OPTICS_GFX_COLOR_SPACE_SRGB,
        .sample_count = 1,
        .capabilities = OPTICS_GFX_RENDER_CAP_SAMPLED_IMAGE | OPTICS_GFX_RENDER_CAP_TRANSFER |
                        OPTICS_GFX_RENDER_CAP_COLOR_ATTACHMENT,
    };
}

static optics_gfx_render_access read_image(const void *image, uint64_t version, bool full) {
    return (optics_gfx_render_access){
        .resource = {OPTICS_GFX_RENDER_RESOURCE_IMAGE, image, version},
        .mode = OPTICS_GFX_RENDER_ACCESS_READ,
        .region = {.full = full, .count = full ? 0u : 1u, .rects = {{0, 0, 8, 8}}},
    };
}

static optics_gfx_render_access write_image(const void *image, uint64_t version) {
    return (optics_gfx_render_access){
        .resource = {OPTICS_GFX_RENDER_RESOURCE_IMAGE, image, version},
        .mode = OPTICS_GFX_RENDER_ACCESS_WRITE,
        .region = {.full = true},
    };
}

static void valid_chain(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    optics_gfx_render_access write = write_image((const void *)0x10, 1);
    optics_gfx_render_access read = read_image((const void *)0x10, 1, true);
    uint32_t upload = 0, canvas = 0;
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_UPLOAD,
                                                 .accesses = &write,
                                                 .access_count = 1,
                                                 .transient_bytes = 32},
                  &upload) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_CANVAS,
                                                 .accesses = &read,
                                                 .access_count = 1,
                                                 .dependencies = &upload,
                                                 .dependency_count = 1,
                                                 .transient_bytes = 64,
                                                 .required_capabilities =
                                                     OPTICS_GFX_RENDER_CAP_SAMPLED_IMAGE},
                  &canvas) == OPTICS_GFX_OK);
    OT_EXPECT(upload == 0 && canvas == 1);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_OK);
}

static void rejects_unordered_read(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    optics_gfx_render_access write = write_image((const void *)0x10, 1);
    optics_gfx_render_access read = read_image((const void *)0x10, 1, true);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &read, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_UPLOAD, .accesses = &write, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
}

static void rejects_cycle(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan, &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_COMPUTE},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan, &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_COMPUTE},
                  nullptr) == OPTICS_GFX_OK);
    plan.nodes[0].dependencies[0] = 1;
    plan.nodes[0].dependency_count = 1;
    plan.nodes[1].dependencies[0] = 0;
    plan.nodes[1].dependency_count = 1;
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
}

static void rejects_two_writers(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    optics_gfx_render_access a = write_image((const void *)0x10, 1);
    optics_gfx_render_access b = write_image((const void *)0x10, 1);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_COMPUTE, .accesses = &a, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_COMPUTE, .accesses = &b, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
}

static void rejects_budget_and_bad_contract(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    t.transient_budget_bytes = 16;
    optics_gfx_render_plan_init(&plan, &t);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_EFFECT,
                                                 .transient_bytes = 32},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_OUT_OF_RANGE);

    optics_gfx_render_plan_init(&plan, &(optics_gfx_render_target){.width = 0});
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_gfx_render_target unsupported = target();
    unsupported.capabilities = OPTICS_GFX_RENDER_CAP_TRANSFER;
    optics_gfx_render_plan_init(&plan, &unsupported);
    OT_EXPECT(
        optics_gfx_render_plan_add_node(
            &plan,
            &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_CANVAS,
                                           .required_capabilities = OPTICS_GFX_RENDER_CAP_COMPUTE},
            nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_UNSUPPORTED);
}

/* ADR-0089 1B-2B: upload -> canvas -> presentation blit is one dependency
 * chain. The upload writes the sampled image, the canvas pass reads it and
 * writes the content image, and the output blit reads the content image and
 * writes the acquired/presentation image. */
static void upload_canvas_output_chain(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);

    const void *uploaded = (const void *)0x20;
    const void *surface = (const void *)0x30;
    const void *presented = (const void *)0x40;
    optics_gfx_render_access upload_write = write_image(uploaded, 0);
    optics_gfx_render_access upload_read = read_image(uploaded, 0, true);
    optics_gfx_render_access surface_write = write_image(surface, 0);
    optics_gfx_render_access surface_read = read_image(surface, 0, true);
    optics_gfx_render_access presented_write = write_image(presented, 0);

    uint32_t upload = 0, canvas = 0, output = 0;
    OT_EXPECT(
        optics_gfx_render_plan_add_node(
            &plan,
            &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_UPLOAD,
                                           .accesses = &upload_write,
                                           .access_count = 1,
                                           .required_capabilities = OPTICS_GFX_RENDER_CAP_TRANSFER},
            &upload) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS,
                      .accesses = (optics_gfx_render_access[]){upload_read, surface_write},
                      .access_count = 2,
                      .dependencies = &upload,
                      .dependency_count = 1,
                      .required_capabilities = OPTICS_GFX_RENDER_CAP_COLOR_ATTACHMENT},
                  &canvas) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_OUTPUT,
                      .accesses = (optics_gfx_render_access[]){surface_read, presented_write},
                      .access_count = 2,
                      .dependencies = &canvas,
                      .dependency_count = 1,
                      .required_capabilities = OPTICS_GFX_RENDER_CAP_TRANSFER},
                  &output) == OPTICS_GFX_OK);
    OT_EXPECT(upload == 0 && canvas == 1 && output == 2);
    OT_EXPECT(optics_gfx_render_plan_last_writer(&plan, uploaded) == upload);
    OT_EXPECT(optics_gfx_render_plan_last_writer(&plan, surface) == canvas);
    OT_EXPECT(optics_gfx_render_plan_last_writer(&plan, presented) == output);
    OT_EXPECT(optics_gfx_render_plan_last_writer(&plan, (const void *)0x99) == UINT32_MAX);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_OK);
}

/* The canvas reads what the upload wrote; without naming the upload as a
 * producer the read is unordered and the plan must reject it. */
static void rejects_upload_read_without_edge(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    const void *uploaded = (const void *)0x20;
    optics_gfx_render_access write = write_image(uploaded, 0);
    optics_gfx_render_access read = read_image(uploaded, 0, true);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_UPLOAD, .accesses = &write, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &read, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);

    /* Naming the producer makes the same graph valid. */
    OT_EXPECT(optics_gfx_render_plan_node_add_dependency(&plan, 1, 0) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_OK);
}

/* The presentation blit reads the content image the Canvas pass wrote. */
static void rejects_output_blit_without_canvas_edge(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    const void *surface = (const void *)0x30;
    const void *presented = (const void *)0x40;
    optics_gfx_render_access write = write_image(surface, 0);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &write, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    optics_gfx_render_access reads[2] = {read_image(surface, 0, true), write_image(presented, 0)};
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_OUTPUT, .accesses = reads, .access_count = 2},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(optics_gfx_render_plan_node_add_dependency(&plan, 1, 0) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_OK);
}

/* A managed write that lands after a plan-managed read is unordered too; the
 * upload must not silently overwrite an image the plan already consumed. */
static void rejects_write_after_plan_read(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    const void *image = (const void *)0x50;
    optics_gfx_render_access read = read_image(image, 0, true);
    optics_gfx_render_access write = write_image(image, 0);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &read, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_UPLOAD, .accesses = &write, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
}

/* The same write-after-read with the writer ordered after the read is legal:
 * a scratch image reused by two effects in one frame. The reader must not be
 * required to depend on a writer that comes after it. */
static void accepts_ordered_write_after_read(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    const void *image = (const void *)0x60;
    optics_gfx_render_access first = write_image(image, 0);
    optics_gfx_render_access read = read_image(image, 0, true);
    optics_gfx_render_access second = write_image(image, 0);
    uint32_t w0 = 0, r = 0;
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &first, .access_count = 1},
                  &w0) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_EFFECT,
                                                 .accesses = &read,
                                                 .access_count = 1,
                                                 .dependencies = &w0,
                                                 .dependency_count = 1},
                  &r) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){.kind = OPTICS_GFX_RENDER_NODE_CANVAS,
                                                 .accesses = &second,
                                                 .access_count = 1,
                                                 .dependencies = &r,
                                                 .dependency_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_OK);
}

/* A rejection names its rule and the two nodes it concerns. */
static void check_reports_the_unordered_pair(void) {
    optics_gfx_render_plan plan;
    optics_gfx_render_target t = target();
    optics_gfx_render_plan_init(&plan, &t);
    optics_gfx_render_access write = write_image((const void *)0x70, 1);
    optics_gfx_render_access read = read_image((const void *)0x70, 1, true);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_UPLOAD, .accesses = &write, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    OT_EXPECT(optics_gfx_render_plan_add_node(
                  &plan,
                  &(optics_gfx_render_node_desc){
                      .kind = OPTICS_GFX_RENDER_NODE_CANVAS, .accesses = &read, .access_count = 1},
                  nullptr) == OPTICS_GFX_OK);
    optics_gfx_render_plan_issue issue = {};
    OT_EXPECT(optics_gfx_render_plan_check(&plan, &issue) == OPTICS_GFX_ERROR_INVALID_STATE);
    OT_EXPECT(issue.rule != nullptr && strstr(issue.rule, "unordered") != nullptr);
    OT_EXPECT(issue.node == 1u && issue.other == 0u);
    OT_EXPECT(optics_gfx_render_plan_validate(&plan) == OPTICS_GFX_ERROR_INVALID_STATE);
}

int main(void) {
    valid_chain();
    rejects_unordered_read();
    rejects_cycle();
    rejects_two_writers();
    rejects_budget_and_bad_contract();
    upload_canvas_output_chain();
    rejects_upload_read_without_edge();
    rejects_output_blit_without_canvas_edge();
    rejects_write_after_plan_read();
    accepts_ordered_write_after_read();
    check_reports_the_unordered_pair();
    return OT_REPORT();
}
