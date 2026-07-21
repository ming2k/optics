/*
 * iris/a11y.h — accessibility seam.
 *
 * STATUS: AT-SPI bridge (Accessible + Action + Text + Value) when libsystemd
 * is available at build time; contract-only stub otherwise.
 *
 * The lens side exposes a semantic tree via lens_accessibility_walk
 * (ADR-0035 in docs/adr/). iris_a11y_init connects to the
 * AT-SPI bus and registers the application; iris_a11y_update, called
 * once per frame after lens_end, walks the semantic tree and reconciles
 * the AT-SPI object tree with the live widgets — emitting ChildrenChanged
 * and StateChanged signals so orca (or any other AT-SPI client) sees the
 * current widget names, roles, and focus.
 *
 * What's covered today:
 *   - Application registration on the AT-SPI bus
 *   - org.a11y.atspi.Accessible: Name, Description, Role, RoleName,
 *     ChildCount, ChildAtIndex, Parent, State
 *   - org.a11y.atspi.Action: GetNActions/GetName/GetDescription/GetActions/
 *     GetKeyBinding + DoAction (synthesize a click at the widget centre —
 *     lens is input-driven, so actions route back through the input queue)
 *   - org.a11y.atspi.Text: GetCharacterCount/GetText/GetTextAll + caret/
 *     selection queries (read-only; lens exposes no set-caret/selection API,
 *     and its caret is a rect not an offset, so GetCaretOffset reports the
 *     end of the text)
 *   - org.a11y.atspi.Value: CurrentValue (parsed from the slider readout),
 *     MinimumValue/MaximumValue/MinimumIncrement (lens exposes no ranges,
 *     reported as 0)
 *   - ChildrenChanged signals when widgets are added/removed
 *   - StateChanged signals when focus changes
 *
 * What's NOT covered yet:
 *   - Setting slider values / text selection programmatically from the AT
 *     (lens has no set-by-value seam; widgets are input-driven).
 *   - Live region announcements.
 */
#ifndef IRIS_A11Y_H
#define IRIS_A11Y_H

#include <iris/app.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialise the accessibility bridge: connect to the AT-SPI bus, register
 * the application, expose the root accessible object. Call once at startup,
 * before iris_app_run. Returns 0 on success, -1 if the bridge is
 * unavailable (no libsystemd, or AT-SPI bus unreachable). */
IRIS_API int iris_a11y_init(void);

/* The unique D-Bus name our AT-SPI connection owns (e.g. ":1.50"), or NULL
 * when the bridge is not running. Useful for tests and for ATs that want
 * to address us directly. */
IRIS_API const char *iris_a11y_unique_name(void);

/* The fd to poll(2) for incoming AT-SPI method calls, or -1 when the bridge
 * is not running. When readable, call iris_a11y_pump to dispatch. */
IRIS_API int iris_a11y_fd(void);

/* The poll(2) event mask to wait on for the a11y fd (sd_bus_get_events). As
 * with the colour-scheme watcher, the underlying sd-bus socket is level-
 * triggered and must be polled with this mask rather than a hard-coded POLLIN,
 * or an idle bus spins the event loop. Returns 0 when the bridge isn't running
 * (caller should then skip the fd). */
IRIS_API short iris_a11y_poll_events(void);

/* Drain pending AT-SPI method calls (GetName / GetRole / ...) from
 * assistive technology clients and reply. Safe to call spuriously. */
IRIS_API void iris_a11y_pump(void);

/* Reconcile the AT-SPI object tree with lens's live semantic tree. Call
 * once per frame, AFTER lens_end (the walk is only valid then). Returns
 * 0 on success, -1 if the bridge is not running. */
IRIS_API int iris_a11y_update(lens *ui);

/* Shutdown the bridge and release D-Bus resources. Safe to call when not
 * running. */
IRIS_API void iris_a11y_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_H */
