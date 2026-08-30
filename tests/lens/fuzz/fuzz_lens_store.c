/* fuzz_lens_store.c — exercise the lens retained store under random churn.
 *
 * Reads the input as a sequence of (op, scope_index) operations:
 *   op = byte % 4
 *     0 = lens_begin/lens_end frame (drives the GC; leaving nodes expire)
 *     1 = touch a widget id (creates or refreshes a node)
 *     2 = touch a container id + child widget (exercises parent linking)
 *     3 = no-op (advances frame time only)
 *
 * The harness asserts only that the calls terminate and the lens context
 * remains intact between frames. Real bugs (heap corruption, use-after-free
 * in the store, double-free in the GC) are caught by ASAN when built with
 * -Db_sanitize=address. */

#include <lens/lens.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const lens_input FRAME0 = {
    .display_size = {1024, 768},
    .dt_seconds = 0.016f,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    lens *ui = NULL;
    if (lens_create(&(lens_desc){0}, &ui) != FLUX_OK)
        return 0;

    uint32_t scope_counter = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t b = data[i];
        uint8_t op = b & 0x03u;

        switch (op) {
        case 0: {
            /* end current frame, begin next */
            lens_end(ui);
            lens_begin(ui, &FRAME0);
            break;
        }
        case 1: {
            /* widget: derive a stable label from the input position */
            char label[32];
            uint32_t n = (uint32_t)snprintf(label, sizeof label, "w%u",
                                            (unsigned)(scope_counter++ & 0xFFFF));
            (void)n;
            lens_button(ui, &(lens_button_opts){.label = label}).clicked;
            break;
        }
        case 2: {
            /* container + child */
            char clabel[32], wlabel[32];
            uint32_t cs = scope_counter++;
            snprintf(clabel, sizeof clabel, "c%u", (unsigned)(cs & 0xFFFF));
            snprintf(wlabel, sizeof wlabel, "w%u", (unsigned)((cs + 1) & 0xFFFF));
            lens_column(ui);
            /* container identity is established by the call sequence; the
             * label is decorative for this fuzzer's purposes. */
            (void)clabel;
            lens_button(ui, &(lens_button_opts){.label = wlabel}).clicked;
            lens_close(ui);
            break;
        }
        default:
            break;
        }

        if (lens_overflowed(ui))
            break; /* arena exhausted; stop pushing more state */
    }

    lens_end(ui);
    lens_destroy(ui);
    return 0;
}
