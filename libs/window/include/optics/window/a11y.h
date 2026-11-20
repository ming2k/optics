/*
 * iris/a11y.h — platform-agnostic accessibility bridge (ADR-0035, ADR-0103).
 *
 * Bridges the live UI semantic tree (iris_ui_host.a11y_walk) to the host
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
 *      with frame composition and the UI frame.
 */
#ifndef OPTICS_WINDOW_A11Y_H
#define OPTICS_WINDOW_A11Y_H

#include <optics/gfx/math.h>
#include <optics/window/loop.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Semantic model                                                     */
/* ================================================================== */

/* Stable identity of one semantic node; 0 == none. */
typedef uint64_t optics_window_node_id;

typedef enum optics_window_role : uint32_t {
    OPTICS_WINDOW_ROLE_NONE = 0, /* decorative; not surfaced to assistive tech */
    OPTICS_WINDOW_ROLE_GROUP,
    OPTICS_WINDOW_ROLE_LABEL,
    OPTICS_WINDOW_ROLE_BUTTON,
    OPTICS_WINDOW_ROLE_CHECKBOX,
    OPTICS_WINDOW_ROLE_SLIDER,
    OPTICS_WINDOW_ROLE_DISCLOSURE,
    OPTICS_WINDOW_ROLE_SCROLLAREA,
    OPTICS_WINDOW_ROLE_TEXTFIELD,
    OPTICS_WINDOW_ROLE_TEXTAREA,
    OPTICS_WINDOW_ROLE_MENU,
    OPTICS_WINDOW_ROLE_RADIO,
    OPTICS_WINDOW_ROLE_DIALOG,
    OPTICS_WINDOW_ROLE_PROGRESS, /* read-only value, not a slider */
    OPTICS_WINDOW_ROLE_TABLE,
    OPTICS_WINDOW_ROLE_ROW,
    OPTICS_WINDOW_ROLE_MENUITEM,
    OPTICS_WINDOW_ROLE_LINK,
} optics_window_role;

/* State bits for iris_semantics.flags. */
enum {
    OPTICS_WINDOW_A11Y_FOCUSED = 1u << 0,
    OPTICS_WINDOW_A11Y_DISABLED = 1u << 1,
    OPTICS_WINDOW_A11Y_CHECKED = 1u << 2,
    OPTICS_WINDOW_A11Y_EXPANDED = 1u << 3,
    OPTICS_WINDOW_A11Y_READONLY = 1u << 4,
    OPTICS_WINDOW_A11Y_SELECTED = 1u << 5,
};

typedef struct optics_window_semantics {
    optics_window_role role;
    const char *name;  /* accessible name; valid for the duration of the visit */
    const char *value; /* slider readout, field contents, ...                  */
    uint32_t flags;
} optics_window_semantics;

/* Visitor for one semantic node, in pre-order, with its solved bounds
 * (logical px) and its nearest semantic ancestor. */
typedef void (*optics_window_a11y_visit_fn)(const optics_window_semantics *s,
                                            optics_gfx_rect bounds, optics_window_node_id id,
                                            optics_window_node_id parent, void *user);

/* ================================================================== */
/*  Bridge                                                             */
/* ================================================================== */

/* Initialise the accessibility bridge: connects to the platform assistive
 * technology infrastructure and registers the root application accessible.
 * Safe to call at application startup before iris_app_run().
 *
 * Returns 0 on success, -1 if the bridge is unavailable on this platform.
 * Asynchronous and fail-soft: never blocks the calling thread on external
 * service activation. */
OPTICS_WINDOW_API int optics_window_a11y_init(void);

/* Query whether an assistive technology client (e.g. screen reader, magnifier,
 * UI test automation) is actively listening for accessibility events.
 * When false, callers may skip detailed semantic tree reconciliation. */
[[nodiscard]] OPTICS_WINDOW_API bool optics_window_a11y_is_active(void);

/* Reconcile the platform accessibility hierarchy with the UI layer's live
 * semantic tree, read through `ui->a11y_walk`. iris_app_run calls this once
 * per frame after `ui->frame`; hosts driving their own loop do the same, on
 * the iris main thread.
 *
 * Returns 0 on success, -1 if the bridge is not active. */
OPTICS_WINDOW_API int optics_window_a11y_update(const optics_window_ui_host *ui);

/* Shutdown the bridge and release platform accessibility resources. Safe to
 * call when not running. */
OPTICS_WINDOW_API void optics_window_a11y_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_H */
