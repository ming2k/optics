/* theme_cocoa.m — system colour-scheme query + live watching (macOS).
 *
 * Startup query reads NSApplication.effectiveAppearance (macOS 10.14+):
 * bestMatchFromAppearancesWithNames: against DarkAqua decides dark/light.
 * Live watching is KVO on the same property: AppKit updates
 * effectiveAppearance on the main thread while processing events, so the
 * observer callback already runs on the iris main thread and can invoke the
 * iris_color_scheme_changed_fn directly (theme.h contract: callbacks run on
 * the thread executing iris_app_run, serialized with the event loop). A
 * defensive off-main-thread fallback routes through the wakeup seam
 * (platform_wakeup.h) in case AppKit ever delivers the change elsewhere.
 *
 * On macOS < 10.14 the query falls back to PREFER_DARK (the safe default
 * for a UI library, same as the Linux fallback) and watching reports
 * unavailable (-1), matching the documented fallback behaviour.
 *
 * Compiled with ARC (see the darwin branch in libs/iris/meson.build).
 *
 * UNVERIFIED ASSUMPTIONS (no Apple SDK on the authoring machine):
 *  1. NSApplication.effectiveAppearance is KVO-observable (documented by
 *     Apple for 10.14+; the key path is "effectiveAppearance").
 *  2. KVO delivery happens on the main thread in practice. The fallback
 *     path covers the exception, at the cost of one wakeup post.
 *  3. bestMatchFromAppearancesWithNames: with exactly {DarkAqua, Aqua}
 *     always returns one of the two; anything non-DarkAqua is treated as
 *     light.
 */

#import <Cocoa/Cocoa.h>

#include "platform_wakeup.h"
#include "theme_watch_internal.h"

#include <iris/theme.h>

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Query                                                              */
/* ------------------------------------------------------------------ */

static iris_color_scheme current_scheme(void) {
    if (@available(macOS 10.14, *)) {
        NSAppearance *appearance = [[NSApplication sharedApplication] effectiveAppearance];
        NSAppearanceName best = [appearance
            bestMatchFromAppearancesWithNames:@[ NSAppearanceNameDarkAqua, NSAppearanceNameAqua ]];
        if ([best isEqualToString:NSAppearanceNameDarkAqua])
            return IRIS_COLOR_SCHEME_PREFER_DARK;
        return IRIS_COLOR_SCHEME_PREFER_LIGHT;
    }
    /* Safe default: dark (lower glare for unknown environments), matching
     * the Linux fallback. */
    return IRIS_COLOR_SCHEME_PREFER_DARK;
}

IRIS_API iris_color_scheme iris_query_system_color_scheme(void) {
    return current_scheme();
}

IRIS_API bool iris_system_prefers_dark(void) {
    iris_color_scheme s = iris_query_system_color_scheme();
    return s == IRIS_COLOR_SCHEME_PREFER_DARK || s == IRIS_COLOR_SCHEME_NO_PREFERENCE;
}

/* ------------------------------------------------------------------ */
/*  Live watching (KVO on NSApp.effectiveAppearance)                   */
/* ------------------------------------------------------------------ */

/* One watcher per process, carrying TWO independent registrations: the
 * host's public slot (iris_color_scheme_watch) and the platform backend's
 * internal slot (iris_theme__watch_backend, theme_watch_internal.h) — the
 * backend's apply-to-lens registration must not consume or overwrite the
 * host's. The observer object owns both cb/user pairs; the context pointer
 * distinguishes our observation from any subclass's. */
@interface IrisThemeWatcher : NSObject {
  @public
    iris_color_scheme_changed_fn cb;
    void *user;
    iris_color_scheme_changed_fn backend_cb;
    void *backend_user;
    bool observing;
}
@end

static IrisThemeWatcher *g_theme_watcher = nil;

/* Fallback delivery for the (unexpected) case the KVO callback fires off
 * the main thread: heap-carried record drained by the wakeup seam on the
 * loop thread. Dropped when no loop is registered (post returns -1). */
typedef struct theme_delivery {
    iris_color_scheme_changed_fn cb;
    void *user;
    iris_color_scheme_changed_fn backend_cb;
    void *backend_user;
    iris_color_scheme scheme;
} theme_delivery;

static void theme_deliver_on_main(void *p) {
    theme_delivery *d = p;
    if (d->cb)
        d->cb(d->scheme, d->user);
    if (d->backend_cb)
        d->backend_cb(d->scheme, d->backend_user);
    free(d);
}

@implementation IrisThemeWatcher

/* Deliver one scheme change to both registered slots, on the main thread
 * if we are already on it, else through the wakeup seam. */
- (void)deliver {
    iris_color_scheme scheme = current_scheme();
    if ([NSThread isMainThread]) {
        if (cb)
            cb(scheme, user);
        if (backend_cb)
            backend_cb(scheme, backend_user);
        return;
    }
    theme_delivery *d = malloc(sizeof *d);
    if (!d)
        return;
    d->cb = cb;
    d->user = user;
    d->backend_cb = backend_cb;
    d->backend_user = backend_user;
    d->scheme = scheme;
    if (iris_platform_wakeup_post(theme_deliver_on_main, d) != 0)
        free(d);
}

- (void)observeValueForKeyPath:(NSString *)keyPath
                      ofObject:(id)object
                        change:(NSDictionary<NSKeyValueChangeKey, id> *)change
                       context:(void *)context {
    if (context != &g_theme_watcher) {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
        return;
    }
    if (!cb && !backend_cb)
        return;
    [self deliver];
}

@end

IRIS_API int iris_color_scheme_watch(iris_color_scheme_changed_fn cb_, void *user_) {
    if (@available(macOS 10.14, *)) {
        NSApplication *app = [NSApplication sharedApplication];
        if (!g_theme_watcher)
            g_theme_watcher = [IrisThemeWatcher new];
        g_theme_watcher->cb = cb_;
        g_theme_watcher->user = user_;
        if (!g_theme_watcher->observing) {
            [app addObserver:g_theme_watcher
                  forKeyPath:@"effectiveAppearance"
                     options:NSKeyValueObservingOptionNew
                     context:&g_theme_watcher];
            g_theme_watcher->observing = true;
        }
        return 0;
    }
    return -1; /* unavailable: caller falls back to the startup-only query */
}

IRIS_API void iris_color_scheme_unwatch(void) {
    if (!g_theme_watcher)
        return;
    g_theme_watcher->cb = NULL;
    g_theme_watcher->user = NULL;
    /* The observer stays live while the backend slot is registered. */
    if (g_theme_watcher->observing && !g_theme_watcher->backend_cb) {
        [[NSApplication sharedApplication] removeObserver:g_theme_watcher
                                               forKeyPath:@"effectiveAppearance"
                                                  context:&g_theme_watcher];
        g_theme_watcher->observing = false;
        g_theme_watcher = nil;
    }
    /* removeObserver is synchronous and we are on the main thread: no
     * callback can fire after this returns (theme.h contract). */
}

int iris_theme__watch_backend(iris_color_scheme_changed_fn cb_, void *user_) {
    if (!cb_)
        return -1;
    if (@available(macOS 10.14, *)) {
        NSApplication *app = [NSApplication sharedApplication];
        if (!g_theme_watcher)
            g_theme_watcher = [IrisThemeWatcher new];
        g_theme_watcher->backend_cb = cb_;
        g_theme_watcher->backend_user = user_;
        if (!g_theme_watcher->observing) {
            [app addObserver:g_theme_watcher
                  forKeyPath:@"effectiveAppearance"
                     options:NSKeyValueObservingOptionNew
                     context:&g_theme_watcher];
            g_theme_watcher->observing = true;
        }
        return 0;
    }
    return -1;
}

void iris_theme__unwatch_backend(void) {
    if (!g_theme_watcher)
        return;
    g_theme_watcher->backend_cb = NULL;
    g_theme_watcher->backend_user = NULL;
    if (g_theme_watcher->observing && !g_theme_watcher->cb) {
        [[NSApplication sharedApplication] removeObserver:g_theme_watcher
                                               forKeyPath:@"effectiveAppearance"
                                                  context:&g_theme_watcher];
        g_theme_watcher->observing = false;
        g_theme_watcher = nil;
    }
}