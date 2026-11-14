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

#ifndef LENS_TEST_H
#define LENS_TEST_H

#include <flux/canvas.h>
#include <lens/lens.h>

#include <optics_test.h>

static inline flux_result lens_test_snapshot_render(lens *ui, flux_canvas *canvas) {
    if (!canvas)
        return FLUX_ERROR_INVALID_ARGUMENT;
    lens_scene_snapshot *snapshot = nullptr;
    flux_result result = lens_snapshot_create(ui, &snapshot);
    if (result == FLUX_OK)
        result = lens_snapshot_submit(snapshot, canvas);
    lens_snapshot_release(snapshot);
    return result;
}

/* Complete one frame: build is already done; this reconciles, publishes and
 * activates the snapshot. Tests that intentionally delay presentation call
 * lens_end() themselves and drive the snapshot handshake explicitly. */
static inline void lens_test_end(lens *ui) {
    lens_end(ui);
    if (lens_overflowed(ui))
        return;
    lens_scene_snapshot *snapshot = nullptr;
    flux_result result = lens_snapshot_create(ui, &snapshot);
    OT_EXPECT(result == FLUX_OK);
    if (result == FLUX_OK)
        OT_EXPECT(lens_snapshot_activate(ui, snapshot) == FLUX_OK);
    lens_snapshot_release(snapshot);
}

#endif /* LENS_TEST_H */
