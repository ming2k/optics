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
 * successful publication is a real flux_display_list that releases cleanly.
 * ASan/UBSan cover the rest (overreads, overflows, use-after-free).
 */
#include <flux/canvas.h>

#include <stdint.h>
#include <stdlib.h>

static const void *deny_resource(void *user, flux_display_list_resource_kind kind, uint32_t id) {
    (void)user;
    (void)kind;
    (void)id;
    return NULL;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    flux_display_list_deserialize_opts opts = FLUX_DISPLAY_LIST_DESERIALIZE_OPTS_INIT;
    opts.resolve = deny_resource;

    flux_display_list *list = NULL;
    flux_result r = flux_display_list_deserialize(data, size, &opts, &list);
    if (r != FLUX_OK && list != NULL)
        abort();
    if (list)
        flux_display_list_release(list);
    return 0;
}
