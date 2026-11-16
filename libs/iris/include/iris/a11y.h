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
#ifndef IRIS_A11Y_H
#define IRIS_A11Y_H

#include <flux/math.h>
#include <iris/app.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Semantic model                                                     */
/* ================================================================== */

/* Stable identity of one semantic node; 0 == none. */
typedef uint64_t iris_node_id;

typedef enum iris_role : uint32_t {
    IRIS_ROLE_NONE = 0, /* decorative; not surfaced to assistive tech */
    IRIS_ROLE_GROUP,
    IRIS_ROLE_LABEL,
    IRIS_ROLE_BUTTON,
    IRIS_ROLE_CHECKBOX,
    IRIS_ROLE_SLIDER,
    IRIS_ROLE_DISCLOSURE,
    IRIS_ROLE_SCROLLAREA,
    IRIS_ROLE_TEXTFIELD,
    IRIS_ROLE_TEXTAREA,
    IRIS_ROLE_MENU,
    IRIS_ROLE_RADIO,
    IRIS_ROLE_DIALOG,
    IRIS_ROLE_PROGRESS, /* read-only value, not a slider */
    IRIS_ROLE_TABLE,
    IRIS_ROLE_ROW,
    IRIS_ROLE_MENUITEM,
    IRIS_ROLE_LINK,
} iris_role;

/* State bits for iris_semantics.flags. */
enum {
    IRIS_A11Y_FOCUSED = 1u << 0,
    IRIS_A11Y_DISABLED = 1u << 1,
    IRIS_A11Y_CHECKED = 1u << 2,
    IRIS_A11Y_EXPANDED = 1u << 3,
    IRIS_A11Y_READONLY = 1u << 4,
    IRIS_A11Y_SELECTED = 1u << 5,
};

typedef struct iris_semantics {
    iris_role role;
    const char *name;  /* accessible name; valid for the duration of the visit */
    const char *value; /* slider readout, field contents, ...                  */
    uint32_t flags;
} iris_semantics;

/* Visitor for one semantic node, in pre-order, with its solved bounds
 * (logical px) and its nearest semantic ancestor. */
typedef void (*iris_a11y_visit_fn)(const iris_semantics *s, flux_rect bounds, iris_node_id id,
                                   iris_node_id parent, void *user);

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
IRIS_API int iris_a11y_init(void);

/* Query whether an assistive technology client (e.g. screen reader, magnifier,
 * UI test automation) is actively listening for accessibility events.
 * When false, callers may skip detailed semantic tree reconciliation. */
[[nodiscard]] IRIS_API bool iris_a11y_is_active(void);

/* Reconcile the platform accessibility hierarchy with the UI layer's live
 * semantic tree, read through `ui->a11y_walk`. iris_app_run calls this once
 * per frame after `ui->frame`; hosts driving their own loop do the same, on
 * the iris main thread.
 *
 * Returns 0 on success, -1 if the bridge is not active. */
IRIS_API int iris_a11y_update(const iris_ui_host *ui);

/* Shutdown the bridge and release platform accessibility resources. Safe to
 * call when not running. */
IRIS_API void iris_a11y_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_H */
