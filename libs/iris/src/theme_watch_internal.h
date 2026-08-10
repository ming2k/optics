/* theme_watch_internal.h — shared header for the theme watcher sources.
 *
 * Houses the IRIS_HAVE_PORTAL_WATCH gate so theme_watch_portal.c and
 * theme_watch_stub.c stay mutually exclusive, plus the backend-reserved
 * watcher slot.
 *
 * Why a second slot: theme.h's public contract is "one watcher per
 * process, re-watching replaces the registration". The platform backends
 * (app_wayland.c / app_win32.c / app_cocoa.m) need their own "apply the
 * new scheme to the lens theme" registration that must NOT consume the
 * host's public slot — previously the backends registered through the
 * public API, so a host watch silently overwrote the backend's (or vice
 * versa) and backend teardown killed the host's watch. The backend slot
 * below is a separate registration with the same delivery contract (cb
 * runs on the iris main thread, serialized with the event loop); hosts
 * keep exclusive ownership of the public iris_color_scheme_watch slot.
 */
#ifndef IRIS_THEME_WATCH_INTERNAL_H
#define IRIS_THEME_WATCH_INTERNAL_H

#include <iris/theme.h>

/* Backend-reserved counterparts of iris_color_scheme_watch/unwatch. Only
 * the platform backends call these. Same return/delivery semantics as the
 * public API; independent of the host's public registration. */
int iris_theme__watch_backend(iris_color_scheme_changed_fn cb, void *user);
void iris_theme__unwatch_backend(void);

#endif /* IRIS_THEME_WATCH_INTERNAL_H */
