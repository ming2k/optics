/* test_render_null.c — snapshot publication argument validation (render seam,
 * ADR-0024/0025). CPU-only. */

#include "optics_test.h"
#include "ui_test.h"
#include <optics/ui.h>

int main(void) {
    optics_ui *ui = nullptr;
    OT_EXPECT(optics_ui_create(&(optics_ui_desc){}, &ui) == OPTICS_GFX_OK);

    OT_EXPECT(optics_ui_test_snapshot_render(nullptr, nullptr) ==
              OPTICS_GFX_ERROR_INVALID_ARGUMENT);
    OT_EXPECT(optics_ui_test_snapshot_render(ui, nullptr) == OPTICS_GFX_ERROR_INVALID_ARGUMENT);

    optics_ui_release(ui);
    return OT_REPORT();
}
