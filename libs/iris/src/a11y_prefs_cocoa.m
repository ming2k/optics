/* a11y_prefs_cocoa.m — system accessibility-preference query + watch (Cocoa).
 *
 * Query (AppKit, main-thread-safe reads):
 *   reduced_motion  NSWorkspace.accessibilityDisplayShouldReduceMotion
 *   high_contrast   NSWorkspace.accessibilityDisplayIsHighContrastEnabled
 *   text_scale      no direct global knob on macOS (Dynamic Type is a
 *                   per-app SwiftUI/UITextScaling concept); the closest
 *                   system-global signal is the "Large text" accessibility
 *                   Quick Action, which changes the display resolution —
 *                   already surfaced as a scale change via
 *                   windowDidChangeBackingProperties. Report 1.0: honest
 *                   absence rather than an invented factor.
 *
 * Live watch: NSWorkspace posts NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
 * on the main run loop — the iris app loop already drains that run loop in
 * app_cocoa.m, so an NSNotificationCenter observer delivers on the iris
 * main thread directly (no wakeup-seam detour needed). Contrast changes
 * also surface as a distributed notification; the workspace one covers it.
 */

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>

#include "a11y_prefs_internal.h"

#include <iris/a11y_prefs.h>

/* Registered watchers, touched on the iris main thread only. Two
 * independent slots: the host's public one and the platform backend's. */
static iris_a11y_prefs_changed_fn g_cb = NULL;
static void *g_user = NULL;
static iris_a11y_prefs_changed_fn g_backend_cb = NULL;
static void *g_backend_user = NULL;
static iris_a11y_prefs g_last;
static id g_observer = nil;

IRIS_API iris_a11y_prefs iris_a11y_prefs_query(void) {
    iris_a11y_prefs p = {.reduced_motion = false, .high_contrast = false, .text_scale = 1.0f};
    if (@available(macOS 10.10, *)) {
        NSWorkspace *ws = [NSWorkspace sharedWorkspace];
        p.reduced_motion = ws.accessibilityDisplayShouldReduceMotion;
        p.high_contrast = ws.accessibilityDisplayIsHighContrastEnabled;
    }
    return p;
}

static void deliver_now(void) {
    iris_a11y_prefs now = iris_a11y_prefs_query();
    if (now.reduced_motion != g_last.reduced_motion || now.high_contrast != g_last.high_contrast ||
        now.text_scale != g_last.text_scale) {
        g_last = now;
        if (g_cb)
            g_cb(&now, g_user);
        if (g_backend_cb)
            g_backend_cb(&now, g_backend_user);
    }
}

static int ensure_observer(void) {
    if (g_observer != nil)
        return 0;
    if (@available(macOS 10.10, *)) {
        /* NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification is
         * delivered on the workspace's notification centre, which forwards
         * to the main run loop — exactly where the iris app loop drains
         * events (app_cocoa.m nextEventMatchingMask). */
        g_observer = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification
                        object:nil
                         queue:[NSOperationQueue mainQueue]
                    usingBlock:^(NSNotification *note) {
                      (void)note;
                      deliver_now();
                    }];
        return g_observer != nil ? 0 : -1;
    }
    return -1;
}

static void drop_observer(void) {
    if (g_observer != nil) {
        [[NSNotificationCenter defaultCenter] removeObserver:g_observer];
        g_observer = nil;
    }
}

IRIS_API int iris_a11y_prefs_watch(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (ensure_observer() != 0)
        return -1;
    g_cb = cb;
    g_user = user;
    g_last = iris_a11y_prefs_query();
    return 0;
}

IRIS_API void iris_a11y_prefs_unwatch(void) {
    g_cb = NULL;
    g_user = NULL;
    if (!g_backend_cb)
        drop_observer();
}

int iris_a11y_prefs__watch_backend(iris_a11y_prefs_changed_fn cb, void *user) {
    if (!cb)
        return -1;
    if (ensure_observer() != 0)
        return -1;
    g_backend_cb = cb;
    g_backend_user = user;
    g_last = iris_a11y_prefs_query();
    return 0;
}

void iris_a11y_prefs__unwatch_backend(void) {
    g_backend_cb = NULL;
    g_backend_user = NULL;
    if (!g_cb)
        drop_observer();
}
