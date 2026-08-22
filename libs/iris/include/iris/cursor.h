/*
 * iris/cursor.h — host-driven cursor appearance.
 *
 * iris owns the window, so it owns the cursor surface. Hosts tell iris
 * what cursor to show via iris_set_cursor(); iris loads the matching
 * image from the system cursor theme (wayland-cursor on Linux) and
 * attaches it to the seat's pointer surface.
 *
 * The enum is intentionally small: covering the common desktop cursors
 * lets an app (editor, browser, tool) drive UX affordances without
 * owning any window-system code. New values can be added without
 * breaking the ABI (callers default to IRIS_CURSOR_DEFAULT).
 */
#ifndef IRIS_CURSOR_H
#define IRIS_CURSOR_H

#include <stdint.h>

/* IRIS_API lives in <iris/app.h>. Every declaring iris header includes
 * it so each header is self-contained: including any single one, alone,
 * must compile (a header that only works when another header happened to
 * be included first is a latent consumer break). */
#include <iris/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Cursor enum                                                       */
/* ================================================================== */

/* The cursor appearance iris currently shows for the app's window.
 * Pass IRIS_CURSOR_DEFAULT to reset to the platform's arrow. Unknown
 * values (forward-compat from a newer libiris) fall back to DEFAULT. */
typedef enum {
    IRIS_CURSOR_DEFAULT = 0, /* arrow                                    */
    IRIS_CURSOR_TEXT,        /* I-beam — over text the user can edit    */
    IRIS_CURSOR_POINTER,     /* pointing hand — over clickable elements */
    IRIS_CURSOR_BUSY,        /* hourglass / spinner — app is working     */
    IRIS_CURSOR_CROSSHAIR,   /* precise selection (e.g. image crop)     */
    IRIS_CURSOR_NOT_ALLOWED, /* forbidden action                         */
    IRIS_CURSOR_RESIZE_EW,   /* horizontal resize                        */
    IRIS_CURSOR_RESIZE_NS,   /* vertical resize                          */
} iris_cursor;

/* Set the cursor the next (and subsequent) pointer enter / motion event
 * will attach to the surface. The call is idempotent; passing the same
 * value twice does no work. The change takes effect on the next pointer
 * frame the compositor delivers — typically the same frame, since most
 * motion events are immediately followed by a frame.
 *
 * No-op on backends without cursor-theme support (e.g. a build without
 * wayland-cursor linked). Thread-affine: call from the same thread that
 * drives iris_app_run. */
IRIS_API void iris_set_cursor(iris_cursor cursor);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_CURSOR_H */
