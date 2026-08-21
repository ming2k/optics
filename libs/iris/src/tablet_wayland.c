/* tablet_wayland.c — zwp_tablet_v2 (tablet-tool-unstable-v2) pen input.
 *
 * Bridges Wayland tablet events into the pointer accumulator: pen motion
 * drives the same cursor coordinates, tip contact drives LENS_MOUSE_LEFT,
 * and pressure/eraser ride along in the new size-guarded lens_input pen
 * fields. The tablet-tool protocol state machine (manager → seat →
 * per-tool objects) lives entirely here; app_wayland.c only registers the
 * global and forwards seat creation.
 *
 * Scope: proximity/motion/frame, tip (contact) and button 1..n as mouse
 * buttons, pressure/tilt axes (pressure consumed; tilt reported through
 * tool capabilities but not surfaced — nothing downstream consumes it
 * yet). The ring/strip/mode-switch surfaces of the protocol are not
 * bound; adding them later does not disturb this file's bridge.
 */

#include "tablet_wayland.h"

#ifdef IRIS_BACKEND_WAYLAND

#include "platform_internal.h"

/* Generated protocol glue (meson wayland module, see libs/iris/meson.build). */
#include "tablet-unstable-v2-client-protocol.h"
#include "wayland-client-protocol.h"

/* ------------------------------------------------------------------ */
/*  Pen state accumulated between frames                               */
/* ------------------------------------------------------------------ */

typedef struct wp_tablet {
    struct zwp_tablet_manager_v2 *mgr;
    struct zwp_tablet_seat_v2 *seat;
    /* Tool currently in proximity (at most one drives pointer semantics;
     * extra tools that enter while one is down are ignored until it
     * leaves — same policy GTK uses to avoid crossed streams). */
    struct zwp_tablet_tool_v2 *active_tool;
    bool in_proximity;
    bool down;       /* tip contact or a barrel button is held */
    bool eraser;     /* tool type says eraser end */
    double pressure; /* normalised 0..1, 1.0 when the tool has none */
    uint32_t proximity_serial;
} wp_tablet;

/* Created lazily on first tablet-manager global. The host vtable is
 * captured at bind time and must outlive the backend. */
static wp_tablet g_tablet;
static iris_tablet_host g_host;
/* Seat attach may arrive before or after the manager global (registry
 * order is unspecified); remember the seat for a late manager. */
static struct wl_seat *g_pending_seat;

/* ------------------------------------------------------------------ */
/*  Tool events → pointer accumulator                                  */
/* ------------------------------------------------------------------ */

static void tool_type_eraser(uint32_t type) {
    g_tablet.eraser = (type == ZWP_TABLET_TOOL_V2_TYPE_ERASER);
}

static void tool_apply_down(bool down) {
    if (down == g_tablet.down)
        return;
    g_tablet.down = down;
    if (g_host.button)
        g_host.button(g_host.user, LENS_MOUSE_LEFT, down);
}

static void tablet_tool_proximity_in(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial,
                                     struct zwp_tablet_v2 *tablet, struct wl_surface *surface) {
    (void)data;
    (void)tablet;
    (void)surface;
    if (g_tablet.active_tool && g_tablet.active_tool != tool)
        return; /* a second tool while one is live: ignore until it leaves */
    g_tablet.active_tool = tool;
    g_tablet.in_proximity = true;
    g_tablet.proximity_serial = serial;
    if (g_host.serial)
        g_host.serial(g_host.user, serial); /* selection ops may be pen-initiated */
}

static void tablet_tool_proximity_out(void *data, struct zwp_tablet_tool_v2 *tool) {
    (void)data;
    (void)tool;
    if (!g_tablet.in_proximity)
        return;
    tool_apply_down(false);
    g_tablet.in_proximity = false;
    g_tablet.active_tool = NULL;
    g_tablet.eraser = false;
    g_tablet.pressure = 0.0;
}

static void tablet_tool_down(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial) {
    (void)data;
    (void)tool;
    if (g_host.serial)
        g_host.serial(g_host.user, serial);
    tool_apply_down(true);
}

static void tablet_tool_up(void *data, struct zwp_tablet_tool_v2 *tool) {
    (void)data;
    (void)tool;
    tool_apply_down(false);
}

static void tablet_tool_motion(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t x,
                               wl_fixed_t y) {
    (void)data;
    (void)tool;
    if (g_host.motion)
        g_host.motion(g_host.user, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void tablet_tool_pressure(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t p) {
    (void)data;
    (void)tool;
    /* Normalised 0..65535 per the protocol. Tools that report no pressure
     * axis deliver none of these events; the default below covers them. */
    g_tablet.pressure = p / 65535.0;
}

static void tablet_tool_distance(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t d) {
    (void)data;
    (void)tool;
    (void)d; /* hover distance: not consumed yet */
}

static void tablet_tool_tilt(void *data, struct zwp_tablet_tool_v2 *tool, wl_fixed_t tx,
                             wl_fixed_t ty) {
    (void)data;
    (void)tool;
    (void)tx;
    (void)ty; /* tilt: not consumed yet */
}

static void tablet_tool_button(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t serial,
                               uint32_t button, uint32_t state) {
    (void)data;
    (void)tool;
    if (g_host.serial)
        g_host.serial(g_host.user, serial);
    /* Barrel buttons map onto mouse semantics: BTN_STYLUS → RIGHT,
     * BTN_STYLUS2 → MIDDLE, matching the evdev conventions lens's mouse
     * contract is built from. The tip contact keeps LEFT. */
    int i = -1;
    if (button == 0x14b) /* BTN_STYLUS */
        i = LENS_MOUSE_RIGHT;
    else if (button == 0x14c) /* BTN_STYLUS2 */
        i = LENS_MOUSE_MIDDLE;
    if (i < 0 || !g_host.button)
        return;
    g_host.button(g_host.user, i, state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED);
}

static void tablet_tool_frame(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t time) {
    (void)time;
    (void)data;
    (void)tool; /* events were applied as they arrived; nothing to batch */
}

static void tablet_tool_removed(void *data, struct zwp_tablet_tool_v2 *tool) {
    tablet_tool_proximity_out(data, tool);
    zwp_tablet_tool_v2_destroy(tool);
}

static void tablet_tool_type(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t type) {
    (void)data;
    (void)tool;
    tool_type_eraser(type);
}

static void tablet_tool_capability(void *data, struct zwp_tablet_tool_v2 *tool, uint32_t cap) {
    (void)data;
    (void)tool;
    (void)cap; /* tilt/ring/strip presence: informational */
}

static void tablet_tool_done(void *data, struct zwp_tablet_tool_v2 *tool) {
    (void)data;
    (void)tool;
}

static const struct zwp_tablet_tool_v2_listener tablet_tool_listener = {
    .type = tablet_tool_type,
    .hardware_serial = NULL,
    .hardware_id_wacom = NULL,
    .capability = tablet_tool_capability,
    .done = tablet_tool_done,
    .removed = tablet_tool_removed,
    .proximity_in = tablet_tool_proximity_in,
    .proximity_out = tablet_tool_proximity_out,
    .down = tablet_tool_down,
    .up = tablet_tool_up,
    .motion = tablet_tool_motion,
    .pressure = tablet_tool_pressure,
    .distance = tablet_tool_distance,
    .tilt = tablet_tool_tilt,
    .rotation = NULL,
    .slider = NULL,
    .wheel = NULL,
    .button = tablet_tool_button,
    .frame = tablet_tool_frame,
};

/* ------------------------------------------------------------------ */
/*  Seat → tool plumbing                                               */
/* ------------------------------------------------------------------ */

static void tablet_seat_tool_added(void *data, struct zwp_tablet_seat_v2 *seat,
                                   struct zwp_tablet_tool_v2 *tool) {
    (void)seat;
    zwp_tablet_tool_v2_add_listener(tool, &tablet_tool_listener, data);
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
    .tablet_added = NULL,
    .tool_added = tablet_seat_tool_added,
    .pad_added = NULL,
};

/* ------------------------------------------------------------------ */
/*  Public bridge (called from app_wayland.c)                          */
/* ------------------------------------------------------------------ */

void iris_wayland__tablet_bind_manager(struct wl_registry *reg, uint32_t name, uint32_t version,
                                       const iris_tablet_host *host) {
    if (g_tablet.mgr)
        return;
    /* v1 is the only protocol version. */
    g_tablet.mgr = wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1);
    if (!g_tablet.mgr)
        return;
    if (host)
        g_host = *host;
    if (g_pending_seat) {
        g_tablet.seat = zwp_tablet_manager_v2_get_tablet_seat(g_tablet.mgr, g_pending_seat);
        zwp_tablet_seat_v2_add_listener(g_tablet.seat, &tablet_seat_listener, NULL);
    }
}

void iris_wayland__tablet_attach_seat(struct wl_seat *seat) {
    g_pending_seat = seat;
    if (!g_tablet.mgr || g_tablet.seat || !seat)
        return;
    g_tablet.seat = zwp_tablet_manager_v2_get_tablet_seat(g_tablet.mgr, seat);
    zwp_tablet_seat_v2_add_listener(g_tablet.seat, &tablet_seat_listener, NULL);
}

void iris_wayland__tablet_global_removed(uint32_t name) {
    /* The manager global carries no name here; tablet seats/tools are
     * child objects and die with the manager. Nothing to unbind. */
    (void)name;
}

void iris_wayland__tablet_fill_input(lens_input *in) {
    in->pen_active = g_tablet.in_proximity && g_tablet.down;
    /* Tools without a pressure axis never send pressure events; contact
     * should still read as full pressure so opacity math degrades
     * gracefully to mouse semantics. */
    in->pen_pressure = g_tablet.down && g_tablet.pressure == 0.0 ? 1.0f : (float)g_tablet.pressure;
    in->pen_eraser = g_tablet.in_proximity && g_tablet.eraser;
}

void iris_wayland__tablet_reset(void) {
    g_tablet = (wp_tablet){0};
}

bool iris_wayland__tablet_in_proximity(void) {
    return g_tablet.in_proximity;
}

#endif /* IRIS_BACKEND_WAYLAND */
