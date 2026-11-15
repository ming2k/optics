/* test_render_null.c — snapshot publication argument validation (render seam,
 * ADR-0024/0025). CPU-only. */

#include "lens_test.h"
#include "optics_test.h"
#include <lens/lens.h>

int main(void) {
    lens *ui = nullptr;
    OT_EXPECT(lens_create(&(lens_desc){}, &ui) == FLUX_OK);

    OT_EXPECT(lens_test_snapshot_render(nullptr, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(lens_test_snapshot_render(ui, nullptr) == FLUX_ERROR_INVALID_ARGUMENT);

    lens_release(ui);
    return OT_REPORT();
}
