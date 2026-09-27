/*
 * iris/a11y.h — platform-agnostic accessibility bridge (ADR-0035, ADR-0103).
 *
 * Bridges the live UI semantic tree (lens_accessibility_walk) to the host
 * operating system's assistive technology subsystem:
 *   - Linux: Asynchronous AT-SPI2 over D-Bus
 *   - Windows: Microsoft UI Automation (UIA) Provider
 *   - macOS: Apple NSAccessibility protocol elements
 *
 * Concurrency & Performance Invariants (ADR-0103):
 *   1. Zero Blocking on Startup: All host discovery and registry handshakes
 *      are asynchronous or fail-fast (<= 50ms). Never stalls the main thread,
 *      window mapping, or swapchain frame pacing.
 *   2. Zero Cost When Inactive: If no screen reader or assistive technology
 *      client is monitoring the desktop session, iris_a11y_is_active() reports
 *      false, allowing callers to bypass tree diffing and IPC serialization.
 *   3. Thread-Affine: All entry points run on the main UI thread, serialized
 *      with frame composition and lens_end().
 */
#ifndef IRIS_A11Y_H
#define IRIS_A11Y_H

#include <iris/app.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the accessibility bridge: connects to the platform assistive
 * technology infrastructure and registers the root application accessible.
 * Safe to call at application startup before iris_app_run().
 *
 * Returns 0 on success, -1 if the bridge is unavailable on this platform.
 * Asynchronous and fail-soft: never blocks the calling thread on external
 * service activation. */
IRIS_API int iris_a11y_init(void);

/* Query whether an assistive technology client (e.g. screen reader, magnifier,
 * UI test automation) is actively listening for accessibility events.
 * When false, callers may skip detailed semantic tree reconciliation. */
[[nodiscard]] IRIS_API bool iris_a11y_is_active(void);

/* Reconcile the platform accessibility hierarchy with lens's live semantic
 * tree. Call once per frame, AFTER lens_end() (the walk is only valid then),
 * on the iris main thread.
 *
 * Returns 0 on success, -1 if the bridge is not active. */
IRIS_API int iris_a11y_update(lens *ui);

/* Shutdown the bridge and release platform accessibility resources. Safe to
 * call when not running. */
IRIS_API void iris_a11y_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_H */
