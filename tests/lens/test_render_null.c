/* test_render_null.c — snapshot publication argument validation (render seam,
 * ADR-0024/0025). CPU-only. */

#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = NULL;
    OT_EXPECT(lens_create(&(lens_desc){0}, &ui) == FLUX_OK);

    OT_EXPECT(lens_test_snapshot_render(NULL, NULL) == FLUX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(lens_test_snapshot_render(ui, NULL) == FLUX_ERROR_INVALID_ARGUMENT);

    lens_release(ui);
    return OT_REPORT();
}
