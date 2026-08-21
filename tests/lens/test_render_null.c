/* test_render_null.c — lens_render argument validation (render seam,
 * ADR-0024/0025). CPU-only. */

#include "test_helpers.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    CHECK(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    CHECK(lens_render(NULL, NULL) == FLUX_ERROR_INVALID_ARGUMENT);
    CHECK(lens_render(ui, NULL) == FLUX_ERROR_INVALID_ARGUMENT);

    lens_destroy(ui);
    return TEST_REPORT();
}
