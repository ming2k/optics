/* test_render_null.c — snapshot publication argument validation (render seam,
 * ADR-0024/0025). CPU-only. */

#include "test_helpers.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    CHECK(test_snapshot_render(NULL, NULL) == FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(test_snapshot_render(ui, NULL) == FLUX_ERROR_INVALID_ARGUMENT);

    lens_release(ui);
    return TEST_REPORT();
}
