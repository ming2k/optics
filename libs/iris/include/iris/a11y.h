/*
 * iris/a11y.h — accessibility seam.
 *
 * STATUS: AT-SPI bridge (Accessible + Action + Text + Value) when libsystemd
 * is available at build time; contract-only stub otherwise.
 *
 * The lens side exposes a semantic tree via lens_accessibility_walk
 * (ADR-0035 in docs/adr/). iris_a11y_init connects to the
 * AT-SPI bus and registers the application; iris_a11y_update, called
 * once per frame after lens_end, walks the semantic tree (deep-copying
 * names/values into bridge-owned storage — the lens pointers are per-frame
 * arena memory) and reconciles the AT-SPI object tree with the live
 * widgets, emitting org.a11y.atspi.Event.Object signals so orca (or any
 * other AT-SPI client) sees the current widget names, roles, and focus.
 *
 * Event wire contract (mirrors at-spi2-atk): every event is a signal on
 * the org.a11y.atspi.Event.Object interface with the "siiva{sv}" shape —
 * detail name, detail1, detail2, variant payload, properties dict:
 *   - ChildrenChanged("add"/"remove", index, 0, (so) child-ref) on the
 *     parent when widgets appear/disappear
 *   - PropertyChange("accessible-name", 0, 0, s new-name) on renames,
 *     PropertyChange("accessible-role", 0, 0, u new-role) on role changes,
 *     PropertyChange("accessible-value", 0, 0, i 0) when a slider /
 *     progress readout moves (clients re-query the Value interface)
 *   - StateChanged("focused"/"checked"/"expanded"/"selected", on, 0, i 0)
 *
 * What's covered today:
 *   - Application registration on the AT-SPI bus
 *   - org.a11y.atspi.Accessible: Name, Description, Role, RoleName,
 *     ChildCount, ChildAtIndex, Parent, State (checked/expanded/selected/
 *     focused/disabled mapped onto the AT-SPI state bits)
 *   - org.a11y.atspi.Action: GetNActions/GetName/GetDescription/GetActions/
 *     GetKeyBinding + DoAction — activates through lens_a11y_activate
 *     (ADR-0062): the widget reports `clicked` through lens's normal
 *     interaction path next frame; disabled widgets do not fire, pointer
 *     occlusion does not block. One advertised action: "click".
 *   - org.a11y.atspi.Text: GetCharacterCount/GetText/GetTextAll + caret/
 *     selection queries (read-only; lens exposes no set-caret/selection API,
 *     and its caret is a rect not an offset, so GetCaretOffset reports the
 *     end of the text)
 *   - org.a11y.atspi.Value: CurrentValue (parsed from the slider/progress
 *     readout), MinimumValue/MaximumValue/MinimumIncrement (lens exposes no
 *     ranges, reported as 0)
 *   - The Event.Object signals listed above, plus TextChanged("insert" /
 *     "delete", offset, length, s text) computed as the common
 *     prefix/suffix delta of TEXTFIELD/TEXTAREA values (ADR-0062)
 *
 * What's NOT covered yet:
 *   - Setting slider values / text selection programmatically from the AT
 *     (SetCurrentValue needs per-widget write paths; ADR-0062 defers it).
 *   - Live region announcements, bounds-changed events.
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
 * unavailable (no libsystemd, or AT-SPI bus unreachable).
 *
 * The bridge services assistive-technology method calls on the iris main
 * thread: the platform backend wires the bridge's transport into its own
 * event loop through an internal integration point (src/a11y_internal.h).
 * No fd, poll mask, or pump call is part of this public surface, so a
 * backend whose OS accessibility transport is not pollable (Win32 UI
 * Automation, Cocoa NSAccessibility) can satisfy the same contract. */
IRIS_API int iris_a11y_init(void);

/* The unique D-Bus name our AT-SPI connection owns (e.g. ":1.50"), or NULL
 * when the bridge is not running. Useful for tests and for ATs that want
 * to address us directly. */
IRIS_API const char *iris_a11y_unique_name(void);

/* Reconcile the AT-SPI object tree with lens's live semantic tree. Call
 * once per frame, AFTER lens_end (the walk is only valid then), on the
 * iris main thread. Returns 0 on success, -1 if the bridge is not
 * running. */
IRIS_API int iris_a11y_update(lens *ui);

/* Shutdown the bridge and release D-Bus resources. Safe to call when not
 * running. */
IRIS_API void iris_a11y_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_A11Y_H */
