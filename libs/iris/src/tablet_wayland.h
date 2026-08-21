/* tablet_wayland.h — pen/tablet bridge between app_wayland.c and
 * tablet_wayland.c (both libiris-internal; hidden visibility).
 *
 * app_wayland.c owns the registry, the platform, and the input
 * accumulator; the tablet state machine stays out of the already-largest
 * backend file by driving the accumulator through this small injection
 * interface. All entry points are inert when no tablet-manager global
 * exists.
 */
#ifndef IRIS_TABLET_WAYLAND_H
#define IRIS_TABLET_WAYLAND_H

#ifdef IRIS_BACKEND_WAYLAND

#include <lens/lens.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

/* How the bridge writes pointer-shaped state into the host accumulator.
 * `user` is the platform. Button indices are lens's (LENS_MOUSE_*). */
typedef struct iris_tablet_host {
    void *user;
    void (*motion)(void *user, double x, double y);
    void (*button)(void *user, int lens_button, bool down);
    void (*serial)(void *user, uint32_t serial);
} iris_tablet_host;

/* Registry saw zwp_tablet_manager_v2. `host` must outlive the backend. */
void iris_wayland__tablet_bind_manager(struct wl_registry *reg, uint32_t name, uint32_t version,
                                       const iris_tablet_host *host);

/* A wl_seat appeared after the manager (order is not guaranteed); attach
 * the tablet seat to it. Safe to call repeatedly. */
void iris_wayland__tablet_attach_seat(struct wl_seat *seat);

/* A global vanished (manager or seat); tablet children die with it. */
void iris_wayland__tablet_global_removed(uint32_t name);

/* Fill the pen fields of `in` for this frame (size-guarded upstream). */
void iris_wayland__tablet_fill_input(lens_input *in);

/* Drop all tablet state (backend teardown). */
void iris_wayland__tablet_reset(void);

/* Whether a tool is currently in proximity (diagnostics/hover policy). */
bool iris_wayland__tablet_in_proximity(void);

#endif /* IRIS_BACKEND_WAYLAND */
#endif /* IRIS_TABLET_WAYLAND_H */
