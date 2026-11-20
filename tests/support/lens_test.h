/*
 * lens_test.h — the headless frame lifecycle lens tests share.
 *
 * Lens has no window and no presentation owner. A test is the host: it ends
 * the frame, creates the immutable snapshot, and activates it exactly as a
 * real host would after a successful present (ADR-0094).
 *
 * Assertions come from <optics_test.h>; this header only expresses the host
 * protocol so dozens of tests cannot drift into subtly different frame
 * sequences.
 */

#ifndef OPTICS_UI_TEST_H
#define OPTICS_UI_TEST_H

#include <optics/gfx/canvas.h>
#include <optics/ui.h>

#include <optics_test.h>

static inline optics_gfx_result optics_ui_test_snapshot_render(optics_ui *ui,
                                                               optics_gfx_canvas *canvas) {
    if (!canvas)
        return OPTICS_GFX_ERROR_INVALID_ARGUMENT;
    optics_ui_scene_snapshot *snapshot = nullptr;
    optics_gfx_result result = optics_ui_snapshot_create(ui, &snapshot);
    if (result == OPTICS_GFX_OK)
        result = optics_ui_snapshot_submit(snapshot, canvas);
    optics_ui_snapshot_release(snapshot);
    return result;
}

/* Complete one frame: build is already done; this reconciles, publishes and
 * activates the snapshot. Tests that intentionally delay presentation call
 * lens_end() themselves and drive the snapshot handshake explicitly. */
static inline void optics_ui_test_end(optics_ui *ui) {
    optics_ui_end(ui);
    if (optics_ui_overflowed(ui))
        return;
    optics_ui_scene_snapshot *snapshot = nullptr;
    optics_gfx_result result = optics_ui_snapshot_create(ui, &snapshot);
    OT_EXPECT(result == OPTICS_GFX_OK);
    if (result == OPTICS_GFX_OK)
        OT_EXPECT(optics_ui_snapshot_activate(ui, snapshot) == OPTICS_GFX_OK);
    optics_ui_snapshot_release(snapshot);
}

#endif /* LENS_TEST_H */
