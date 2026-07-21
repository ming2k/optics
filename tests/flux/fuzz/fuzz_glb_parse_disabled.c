/* fuzz_glb_parse_disabled.c — placeholder.
 *
 * The GLB parser entry point currently requires a flux_device* (see
 * flux_sg_load_glb in libs/flux/scene_graph/include/flux-scene-graph/
 * scene-graph.h). Without a deviceless parse API, we cannot fuzz the
 * parser from CI without a Vulkan ICD, which would couple the fuzz
 * suite to a GPU.
 *
 * This file documents the gap and provides a no-op LLVMFuzzerTestOneInput
 * so the build wiring in meson.build stays consistent. When a pure-parse
 * seam lands (per ADR-0016), replace this file with a real harness that
 * calls the new entry point.
 */

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    (void)data;
    (void)size;
    return 0;
}
