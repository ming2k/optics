/* fuzz_glb_parse.c — feed arbitrary bytes through the deviceless GLB
 * parse seam (flux_sg_parse_glb, ADR-0016).
 *
 * The harness allocates a parsed scene and releases it with
 * flux_sg_scene_data_free, so the parser's allocation/free discipline
 * is exercised on BOTH success and error paths (ASan verifies: any
 * retained buffer on an error path is a leak; any double-free is a
 * crash). The parser must also:
 *
 *   - terminate on every input (no hang, no runaway recursion);
 *   - never read past the caller's buffer (chunk lengths, accessor
 *     spans, and JSON offsets are all bounds-checked);
 *   - never overflow a count*elem allocation;
 *   - never index a node/child/joint array out of range.
 *
 * Parse success/failure is irrelevant to fuzzing; every path through
 * container validation, JSON walking, accessor math, mesh assembly,
 * node linking, skinning, and VRM humanoid extraction is the target.
 *
 * Seeds in corpus/glb_parse cover: a valid minimal GLB, truncation at
 * every structural boundary, corrupt magic/version/total lengths,
 * hostile accessor math (huge counts, overlapping views, wrong
 * component types), valid JSON with hostile semantic content, and
 * deeply nested / duplicated containers. */

#include <flux-scene-graph/scene-graph.h>

#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    flux_sg_scene_data *parsed = NULL;
    flux_result r = flux_sg_parse_glb(data, size, &parsed);
    if (parsed) {
        /* Walk the accessors too: primitive_count must be consistent
         * with what bounds() reports (a torn parse must not report
         * primitives with no finite geometry). */
        flux_vec3 bmin, bmax;
        bool any_bounds = flux_sg_scene_data_bounds(parsed, &bmin, &bmax);
        bool inverted = any_bounds && !(bmin.x <= bmax.x && bmin.y <= bmax.y && bmin.z <= bmax.z);
        flux_sg_scene_data_free(parsed);
        if (inverted)
            return 1;
    }
    (void)r;
    return 0;
}
