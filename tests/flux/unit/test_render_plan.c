#include "../../../libs/flux/src/core/render_plan.h"
#include "test_helpers.h"

static flux_render_target target(void) {
    return (flux_render_target){
        .width = 128,
        .height = 64,
        .scale = 1.0f,
        .format = FLUX_FORMAT_RGBA8_UNORM,
        .color_space = FLUX_COLOR_SPACE_SRGB,
        .sample_count = 1,
        .capabilities = FLUX_RENDER_CAP_SAMPLED_IMAGE | FLUX_RENDER_CAP_TRANSFER |
                        FLUX_RENDER_CAP_COLOR_ATTACHMENT,
    };
}

static flux_render_access read_image(const void *image, uint64_t version, bool full) {
    return (flux_render_access){
        .resource = {FLUX_RENDER_RESOURCE_IMAGE, image, version},
        .mode = FLUX_RENDER_ACCESS_READ,
        .region = {.full = full, .count = full ? 0u : 1u, .rects = {{0, 0, 8, 8}}},
    };
}

static flux_render_access write_image(const void *image, uint64_t version) {
    return (flux_render_access){
        .resource = {FLUX_RENDER_RESOURCE_IMAGE, image, version},
        .mode = FLUX_RENDER_ACCESS_WRITE,
        .region = {.full = true},
    };
}

static void valid_chain(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    flux_render_access write = write_image((const void *)0x10, 1);
    flux_render_access read = read_image((const void *)0x10, 1, true);
    uint32_t upload = 0, canvas = 0;
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_UPLOAD,
                                                              .accesses = &write,
                                                              .access_count = 1,
                                                              .transient_bytes = 32},
                                     &upload) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(
               &plan,
               &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                        .accesses = &read,
                                        .access_count = 1,
                                        .dependencies = &upload,
                                        .dependency_count = 1,
                                        .transient_bytes = 64,
                                        .required_capabilities = FLUX_RENDER_CAP_SAMPLED_IMAGE},
               &canvas) == FLUX_OK);
    EXPECT(upload == 0 && canvas == 1);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_OK);
}

static void rejects_unordered_read(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    flux_render_access write = write_image((const void *)0x10, 1);
    flux_render_access read = read_image((const void *)0x10, 1, true);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                                              .accesses = &read,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_UPLOAD,
                                                              .accesses = &write,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);
}

static void rejects_cycle(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_COMPUTE},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_COMPUTE},
                                     nullptr) == FLUX_OK);
    plan.nodes[0].dependencies[0] = 1;
    plan.nodes[0].dependency_count = 1;
    plan.nodes[1].dependencies[0] = 0;
    plan.nodes[1].dependency_count = 1;
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);
}

static void rejects_two_writers(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    flux_render_access a = write_image((const void *)0x10, 1);
    flux_render_access b = write_image((const void *)0x10, 1);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_COMPUTE,
                                                              .accesses = &a,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_COMPUTE,
                                                              .accesses = &b,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);
}

static void rejects_budget_and_bad_contract(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    t.transient_budget_bytes = 16;
    flux_render_plan_init(&plan, &t);
    EXPECT(flux_render_plan_add_node(
               &plan,
               &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_EFFECT, .transient_bytes = 32},
               nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_OUT_OF_RANGE);

    flux_render_plan_init(&plan, &(flux_render_target){.width = 0});
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_ARGUMENT);

    flux_render_target unsupported = target();
    unsupported.capabilities = FLUX_RENDER_CAP_TRANSFER;
    flux_render_plan_init(&plan, &unsupported);
    EXPECT(flux_render_plan_add_node(
               &plan,
               &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                        .required_capabilities = FLUX_RENDER_CAP_COMPUTE},
               nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_UNSUPPORTED);
}

/* ADR-0089 1B-2B: upload -> canvas -> presentation blit is one dependency
 * chain. The upload writes the sampled image, the canvas pass reads it and
 * writes the content image, and the output blit reads the content image and
 * writes the acquired/presentation image. */
static void upload_canvas_output_chain(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);

    const void *uploaded = (const void *)0x20;
    const void *surface = (const void *)0x30;
    const void *presented = (const void *)0x40;
    flux_render_access upload_write = write_image(uploaded, 0);
    flux_render_access upload_read = read_image(uploaded, 0, true);
    flux_render_access surface_write = write_image(surface, 0);
    flux_render_access surface_read = read_image(surface, 0, true);
    flux_render_access presented_write = write_image(presented, 0);

    uint32_t upload = 0, canvas = 0, output = 0;
    EXPECT(flux_render_plan_add_node(
               &plan,
               &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_UPLOAD,
                                        .accesses = &upload_write,
                                        .access_count = 1,
                                        .required_capabilities = FLUX_RENDER_CAP_TRANSFER},
               &upload) == FLUX_OK);
    EXPECT(
        flux_render_plan_add_node(
            &plan,
            &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                     .accesses = (flux_render_access[]){upload_read, surface_write},
                                     .access_count = 2,
                                     .dependencies = &upload,
                                     .dependency_count = 1,
                                     .required_capabilities = FLUX_RENDER_CAP_COLOR_ATTACHMENT},
            &canvas) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(
               &plan,
               &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_OUTPUT,
                                        .accesses =
                                            (flux_render_access[]){surface_read, presented_write},
                                        .access_count = 2,
                                        .dependencies = &canvas,
                                        .dependency_count = 1,
                                        .required_capabilities = FLUX_RENDER_CAP_TRANSFER},
               &output) == FLUX_OK);
    EXPECT(upload == 0 && canvas == 1 && output == 2);
    EXPECT(flux_render_plan_last_writer(&plan, uploaded) == upload);
    EXPECT(flux_render_plan_last_writer(&plan, surface) == canvas);
    EXPECT(flux_render_plan_last_writer(&plan, presented) == output);
    EXPECT(flux_render_plan_last_writer(&plan, (const void *)0x99) == UINT32_MAX);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_OK);
}

/* The canvas reads what the upload wrote; without naming the upload as a
 * producer the read is unordered and the plan must reject it. */
static void rejects_upload_read_without_edge(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    const void *uploaded = (const void *)0x20;
    flux_render_access write = write_image(uploaded, 0);
    flux_render_access read = read_image(uploaded, 0, true);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_UPLOAD,
                                                              .accesses = &write,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                                              .accesses = &read,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);

    /* Naming the producer makes the same graph valid. */
    EXPECT(flux_render_plan_node_add_dependency(&plan, 1, 0) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_OK);
}

/* The presentation blit reads the content image the Canvas pass wrote. */
static void rejects_output_blit_without_canvas_edge(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    const void *surface = (const void *)0x30;
    const void *presented = (const void *)0x40;
    flux_render_access write = write_image(surface, 0);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                                              .accesses = &write,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    flux_render_access reads[2] = {read_image(surface, 0, true), write_image(presented, 0)};
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_OUTPUT,
                                                              .accesses = reads,
                                                              .access_count = 2},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);
    EXPECT(flux_render_plan_node_add_dependency(&plan, 1, 0) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_OK);
}

/* A managed write that lands after a plan-managed read is unordered too; the
 * upload must not silently overwrite an image the plan already consumed. */
static void rejects_write_after_plan_read(void) {
    flux_render_plan plan;
    flux_render_target t = target();
    flux_render_plan_init(&plan, &t);
    const void *image = (const void *)0x50;
    flux_render_access read = read_image(image, 0, true);
    flux_render_access write = write_image(image, 0);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_CANVAS,
                                                              .accesses = &read,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_add_node(&plan,
                                     &(flux_render_node_desc){.kind = FLUX_RENDER_NODE_UPLOAD,
                                                              .accesses = &write,
                                                              .access_count = 1},
                                     nullptr) == FLUX_OK);
    EXPECT(flux_render_plan_validate(&plan) == FLUX_ERROR_INVALID_STATE);
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
    TEST_SUMMARY();
}
