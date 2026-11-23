/* fuzz_display_list_deserialize.c — feed arbitrary bytes through the
 * ADR-0157 DisplayList wire parser.
 *
 * The resolver deliberately refuses every resource. A stream that reaches
 * the encoder therefore exercises the header, resource table, command
 * opcodes, payload lengths and alignment, while resource-bearing streams
 * fail at the table before any GPU object is touched. The harness has no
 * device dependency, so it runs in the sanitizer lane without a Vulkan ICD.
 *
 * Contract checked on every input: every failure publishes no list, and a
 * successful publication is a real optics_gfx_display_list that releases cleanly.
 * ASan/UBSan cover the rest (overreads, overflows, use-after-free).
 */
#include <optics/gfx/canvas.h>

#include <stdint.h>
#include <stdlib.h>

static const void *deny_resource(void *user, optics_gfx_display_list_resource_kind kind,
                                 uint32_t id) {
    (void)user;
    (void)kind;
    (void)id;
    return nullptr;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    optics_gfx_display_list *list = nullptr;
    optics_gfx_display_list_deserialize_opts opts = OPTICS_GFX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
    opts.data = data;
    opts.size = size;
    opts.resolve = deny_resource;
    opts.out_list = &list;

    optics_gfx_result r = optics_gfx_display_list_deserialize(&opts);
    if (r != OPTICS_GFX_OK && list != nullptr)
        abort();
    if (list)
        optics_gfx_display_list_release(list);
    return 0;
}
