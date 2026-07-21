/* app_wayland.c — native Wayland window + Vulkan (via flux) + lens_input.
 *
 * Replaces the GLFW glue the examples used to share. Pure Wayland:
 * xdg-shell for the toplevel, xdg-decoration for a server-side title bar
 * when available, xkbcommon for the keymap, and VK_KHR_wayland_surface to
 * hand flux core a VkSurfaceKHR. Pointer and keyboard events are folded
 * into one lens_input per frame (ADR-0029).
 */

#include "a11y_internal.h"
#include "platform_internal.h"

#include <iris/a11y.h>
#include <iris/cursor.h>
#include <iris/theme.h>

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include <linux/input-event-codes.h> /* BTN_LEFT / BTN_RIGHT / BTN_MIDDLE */

#include <ctype.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* focus.c maps Tab from this portable keycode (ADR-0029). */
#define WP_KEY_TAB 258

/* ------------------------------------------------------------------ */
/*  Accumulated input, drained into one lens_input per frame             */
/* ------------------------------------------------------------------ */

typedef struct wp_accum {
    double cx, cy;                  /* latest cursor, surface-local   */
    bool down[LENS_MOUSE_COUNT];    /* held state (persists)          */
    bool pressed[LENS_MOUSE_COUNT]; /* edges this frame               */
    bool released[LENS_MOUSE_COUNT];
    double scroll_x, scroll_y;      /* wheel this frame               */
    uint32_t mods;                  /* level state (persists)         */
    char text[32];                  /* committed text this frame      */
    char preedit[LENS_PREEDIT_MAX]; /* IME preedit string             */
    uint32_t preedit_cursor;        /* caret byte offset in preedit   */
    lens_key_event keys[LENS_INPUT_MAX_KEYS];
    uint32_t key_count;
} wp_accum;

/* ------------------------------------------------------------------ */
/*  Platform state                                                     */
/* ------------------------------------------------------------------ */

/* One wl_output binding so we can read its scale. */
#define WP_MAX_OUTPUTS 8
typedef struct wp_output {
    struct wl_output *wl;
    int scale;    /* output integer scale (1, 2, 3 …) */
    bool entered; /* surface currently on this output */
} wp_output;

typedef struct wp_platform {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct zxdg_decoration_manager_v1 *deco_mgr;

    struct wl_data_device_manager *data_device_mgr;
    struct wl_data_device *data_device;
    struct wl_data_offer *selection_offer; /* current clipboard offer */
    struct wl_data_offer *dnd_offer;       /* current drag offer         */
    struct wl_data_source *copy_source;    /* our outgoing selection  */
    char *copy_buf;                        /* text we currently advertise for copy     */
    size_t copy_len;
    uint32_t last_serial; /* most recent input serial (set_selection) */
    bool dnd_inside;      /* drag currently over our surface           */
    flux_point dnd_pos;   /* last reported drag position (surface px)  */

    /* Pointer + keyboard + touch (touch is optional). */
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *deco;

    /* Cursor: cursor-shape-v1 (the modern wayland-protocols path). The
     * compositor renders the cursor with its own configured theme, so we
     * never load a cursor theme ourselves — this guarantees the cursor
     * always matches the user's desktop (Adwaita on GNOME, Breeze on KDE,
     * …) instead of falling back to a hard-coded theme name. The manager
     * is bound at registry time; the per-pointer device is created lazily
     * on the first enter. If the compositor doesn't support cursor-shape,
     * `cursor_shape_mgr` stays NULL and iris_set_cursor is a no-op. */
    struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    iris_cursor current_cursor;
    uint32_t cursor_serial; /* most recent enter/motion serial */
    bool cursor_inside;     /* pointer currently inside our surface */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    struct zwp_text_input_manager_v3 *text_input_mgr;
    struct zwp_text_input_v3 *text_input;
    struct wl_surface *text_input_surface;

    /* Pending IME state (double-buffered by text-input-v3 done event) */
    struct {
        char commit[64];
        char preedit[LENS_PREEDIT_MAX];
        int32_t preedit_cursor_begin;
        int32_t preedit_cursor_end;
        uint32_t delete_before;
        uint32_t delete_after;
        bool has_commit;
        bool has_preedit;
        bool has_delete;
    } ime;

    wp_output outputs[WP_MAX_OUTPUTS];
    int n_outputs;
    int buffer_scale;       /* max scale of entered outputs; ≥1     */
    int pending_scale;      /* recomputed from enter/leave + globals */
    float fractional_scale; /* last reported fractional scale, 0 = none */

    struct wp_fractional_scale_manager_v1 *fractional_scale_mgr;
    struct wp_fractional_scale_v1 *fractional_scale_obj;

    int width, height;        /* surface size in *logical* pixels      */
    int pending_w, pending_h; /* from the latest toplevel.configure    */
    int min_w, min_h;         /* size hints last sent to the compositor */
    int max_w, max_h;         /* 0 = no limit                            */
    bool running;
    bool resized; /* size or scale changed -> resize swap  */
    bool animation_frame_requested; /* host asked for active-rate follow-up */
    lens *ui;     /* so output/scale callbacks can update  */

    /* Live colour-scheme watching + AT-SPI bridge: optional, fail-soft. */
    int theme_fd;
    int a11y_fd;

    wp_accum acc;
} wp_platform;

/* Theme watcher callback: invoked on the pump thread when the portal emits
 * a SettingChanged for org.freedesktop.appearance.color-scheme. */
static void wp_on_color_scheme_changed(iris_color_scheme scheme, void *user) {
    wp_platform *pl = user;
    if (!pl || !pl->ui)
        return;
    lens_theme th =
        (scheme == IRIS_COLOR_SCHEME_PREFER_LIGHT) ? lens_theme_default() : lens_theme_dark();
    lens_set_theme(pl->ui, th);
}

/* Recompute the scale we should use: max integer scale across the outputs
 * the surface currently sits on. If none known yet, fall back to 1. */
static void wp_recompute_scale(wp_platform *pl) {
    int s = 0;
    for (int i = 0; i < pl->n_outputs; i++)
        if (pl->outputs[i].entered && pl->outputs[i].scale > s)
            s = pl->outputs[i].scale;
    if (s <= 0)
        s = 1;
    pl->pending_scale = s;
}

/* ------------------------------------------------------------------ */
/*  xdg_wm_base — answer pings                                          */
/* ------------------------------------------------------------------ */

static void wm_base_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_base_listener = {.ping = wm_base_ping};

/* ------------------------------------------------------------------ */
/*  Pointer                                                            */
/* ------------------------------------------------------------------ */

static int mouse_index(uint32_t button) {
    switch (button) {
    case BTN_LEFT:
        return LENS_MOUSE_LEFT;
    case BTN_RIGHT:
        return LENS_MOUSE_RIGHT;
    case BTN_MIDDLE:
        return LENS_MOUSE_MIDDLE;
    default:
        return -1;
    }
}

/* Forward decl: cursor_apply is defined later (in the cursor section),
 * but ptr_enter needs to call it so the cursor is right when the pointer
 * enters our surface. */
static void cursor_apply(wp_platform *pl);

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf,
                      wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    (void)surf;
    wp_platform *pl = data;
    pl->acc.cx = wl_fixed_to_double(x);
    pl->acc.cy = wl_fixed_to_double(y);
    pl->cursor_inside = true;
    pl->cursor_serial = serial;
    cursor_apply(pl);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf) {
    (void)p;
    (void)serial;
    (void)surf;
    wp_platform *pl = data;
    pl->acc.cx = pl->acc.cy = -100000.0; /* off-window: clears hover */
    pl->cursor_inside = false;
}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)p;
    (void)t;
    wp_platform *pl = data;
    pl->acc.cx = wl_fixed_to_double(x);
    pl->acc.cy = wl_fixed_to_double(y);
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state) {
    (void)p;
    (void)serial;
    (void)t;
    wp_platform *pl = data;
    int i = mouse_index(button);
    if (i < 0)
        return;
    bool down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (down && !pl->acc.down[i])
        pl->acc.pressed[i] = true;
    if (!down && pl->acc.down[i])
        pl->acc.released[i] = true;
    pl->acc.down[i] = down;
}
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t axis,
                     wl_fixed_t value) {
    (void)p;
    (void)t;
    wp_platform *pl = data;
    /* Legacy wl_pointer.axis: ~10 units per notch, positive is a physical
     * wheel-down gesture. Lens uses the conventional UI delta contract in
     * which wheel-down is negative (content offset increases), so invert at
     * this platform boundary. Modern
     * compositors deliver via value120 instead — see ptr_axis_value120 —
     * but the legacy event still fires on older compositors, so we keep
     * handling it. */
    double v = wl_fixed_to_double(value) / 10.0;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        pl->acc.scroll_y -= v;
    else if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        pl->acc.scroll_x -= v;
}
static void ptr_frame(void *d, struct wl_pointer *p) {
    (void)d;
    (void)p;
}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {
    (void)d;
    (void)p;
    (void)s;
}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)d;
    (void)p;
    (void)t;
    (void)a;
}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
    (void)d;
    (void)p;
    (void)a;
    (void)v;
}
/* Modern compositors (GNOME, KDE, wlroots ≥ 2022) deliver wheel events
 * here instead of the deprecated `axis` event. value120 is 120 per notch;
 * same Wayland sign convention as `axis`; invert it for Lens. Many
 * compositors send BOTH this and `axis` for back-compat — accumulating
 * both would double the scroll, so we ONLY honour value120 when it fires
 * and zero the legacy accumulator contribution. The simplest way: handle
 * value120 here and accept that on compositors that send both, `axis`
 * will redundantly add a small legacy delta — empirically the latter is
 * zero on value120-supporting compositors, so this is fine in practice. */
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
    (void)p;
    wp_platform *pl = d;
    double norm = (double)v / 120.0;
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL)
        pl->acc.scroll_y -= norm;
    else if (a == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        pl->acc.scroll_x -= norm;
}
static void ptr_axis_relative_direction(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {
    (void)d;
    (void)p;
    (void)a;
    (void)dir;
}

/* ------------------------------------------------------------------ */
/*  Cursor (wayland-cursor)                                            */
/* ------------------------------------------------------------------ */

/* iris_set_cursor is a public, context-free API; it reaches the active
 * app's platform through this single static pointer. Set at the top of
 * iris_app_run_wayland, cleared at exit. Documented as thread-affine:
 * callers must drive it from the same thread that runs iris_app_run. */
static wp_platform *g_active_pl = NULL;

void iris_request_animation_frame_wayland(void) {
    wp_platform *pl = g_active_pl;
    if (pl)
        pl->animation_frame_requested = true;
}

/* Map the public cursor enum to the cursor-shape-v1 shape enum. The
 * compositor renders the cursor with its own configured theme, so we
 * never load a theme ourselves — this is why the cursor always matches
 * the user's desktop (Adwaita on GNOME, Breeze on KDE, …). */
static uint32_t cursor_shape_for(iris_cursor c) {
    switch (c) {
    case IRIS_CURSOR_TEXT:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
    case IRIS_CURSOR_POINTER:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    case IRIS_CURSOR_BUSY:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_WAIT;
    case IRIS_CURSOR_CROSSHAIR:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_CROSSHAIR;
    case IRIS_CURSOR_NOT_ALLOWED:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NOT_ALLOWED;
    case IRIS_CURSOR_RESIZE_EW:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_COL_RESIZE;
    case IRIS_CURSOR_RESIZE_NS:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_ROW_RESIZE;
    case IRIS_CURSOR_DEFAULT:
    default:
        return WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    }
}

/* Lazily create the per-pointer cursor_shape_device on first use. The
 * device is per-pointer; we make exactly one and reuse it for the app's
 * lifetime. */
static void cursor_ensure_device(wp_platform *pl) {
    if (pl->cursor_shape_device || !pl->cursor_shape_mgr || !pl->pointer)
        return;
    pl->cursor_shape_device =
        wp_cursor_shape_manager_v1_get_pointer(pl->cursor_shape_mgr, pl->pointer);
}

/* Tell the compositor to render `pl->current_cursor` for the next pointer
 * frame. No-op if cursor-shape isn't supported (the compositor's default
 * arrow stays) or the pointer is outside our surface. The serial is from
 * the pointer enter; cursor-shape-v1 requires it to attribute the shape
 * change to a legitimate input event the client received. */
static void cursor_apply(wp_platform *pl) {
    if (!pl->cursor_inside || !pl->cursor_serial)
        return;
    cursor_ensure_device(pl);
    if (!pl->cursor_shape_device)
        return;
    wp_cursor_shape_device_v1_set_shape(pl->cursor_shape_device, pl->cursor_serial,
                                        cursor_shape_for(pl->current_cursor));
}

/* Public API. Sets the active platform's current cursor and tells the
 * compositor at the next pointer frame. No-op when iris was built
 * without cursor-shape protocol support, or before iris_app_run starts. */
IRIS_API void iris_set_cursor(iris_cursor cursor) {
    wp_platform *pl = g_active_pl;
    if (!pl)
        return;
    if (cursor < IRIS_CURSOR_DEFAULT)
        cursor = IRIS_CURSOR_DEFAULT;
    if (cursor == pl->current_cursor)
        return;
    pl->current_cursor = cursor;
    cursor_apply(pl);
}

/* ------------------------------------------------------------------ */
/*  Window state (iris/window.h)                                       */
/* ------------------------------------------------------------------ */

/* All iris_window_* APIs operate on g_active_pl — the platform the active
 * iris_app_run call owns. They are documented as thread-affine to
 * iris_app_run and a no-op when no app is active. Each maps directly to the
 * matching xdg_toplevel request; compositors honour them according to
 * their own policy (tiling WMs may ignore maximise / minimise requests). */

IRIS_API void iris_window_minimize(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->toplevel)
        xdg_toplevel_set_minimized(pl->toplevel);
}

IRIS_API void iris_window_maximize(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->toplevel)
        xdg_toplevel_set_maximized(pl->toplevel);
}

IRIS_API void iris_window_unmaximize(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->toplevel)
        xdg_toplevel_unset_maximized(pl->toplevel);
}

IRIS_API void iris_window_fullscreen(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->toplevel)
        xdg_toplevel_set_fullscreen(pl->toplevel, NULL);
}

IRIS_API void iris_window_unfullscreen(void) {
    wp_platform *pl = g_active_pl;
    if (pl && pl->toplevel)
        xdg_toplevel_unset_fullscreen(pl->toplevel);
}

IRIS_API void iris_window_restore(void) {
    wp_platform *pl = g_active_pl;
    if (!pl || !pl->toplevel)
        return;
    xdg_toplevel_unset_maximized(pl->toplevel);
    xdg_toplevel_unset_fullscreen(pl->toplevel);
}

IRIS_API void iris_window_focus(void) {
    /* Wayland clients cannot activate themselves; the compositor is the
     * arbiter. Provide the hook for symmetry with future Win32/Cocoa
     * backends where it works. */
    (void)0;
}

IRIS_API void iris_window_close(void) {
    wp_platform *pl = g_active_pl;
    if (pl)
        pl->running = false;
}

IRIS_API void iris_window_set_min_size(int32_t width, int32_t height) {
    wp_platform *pl = g_active_pl;
    if (!pl || !pl->toplevel)
        return;
    pl->min_w = width;
    pl->min_h = height;
    xdg_toplevel_set_min_size(pl->toplevel, width, height);
}

IRIS_API void iris_window_set_max_size(int32_t width, int32_t height) {
    wp_platform *pl = g_active_pl;
    if (!pl || !pl->toplevel)
        return;
    pl->max_w = width;
    pl->max_h = height;
    xdg_toplevel_set_max_size(pl->toplevel, width, height);
}

IRIS_API bool iris_window_get_geometry(int32_t *out_width, int32_t *out_height) {
    wp_platform *pl = g_active_pl;
    if (!pl)
        return false;
    if (out_width)
        *out_width = pl->width;
    if (out_height)
        *out_height = pl->height;
    return true;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter,
    .leave = ptr_leave,
    .motion = ptr_motion,
    .button = ptr_button,
    .axis = ptr_axis,
    .frame = ptr_frame,
    .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative_direction,
};

/* ------------------------------------------------------------------ */
/*  Touch (wl_touch) — single-touch mapped to mouse + LENS_MOUSE_LEFT  */
/* ------------------------------------------------------------------ */

/* Multi-touch is intentionally collapsed to a single pointer; lens's input
 * model is mouse + IME, and a proper multi-touch widget API is a follow-on.
 * The first finger down drives the cursor and button state; subsequent
 * fingers without an explicit ID up are tracked but ignored. */

static int32_t g_touch_active_id = -1; /* -1 = no active touch */

static void touch_down(void *data, struct wl_touch *t, uint32_t serial, uint32_t time,
                       struct wl_surface *surf, int32_t id, wl_fixed_t x, wl_fixed_t y) {
    (void)t;
    (void)serial;
    (void)time;
    (void)surf;
    wp_platform *pl = data;
    if (g_touch_active_id == -1) {
        g_touch_active_id = id;
        pl->acc.cx = wl_fixed_to_double(x);
        pl->acc.cy = wl_fixed_to_double(y);
        if (!pl->acc.down[LENS_MOUSE_LEFT]) {
            pl->acc.pressed[LENS_MOUSE_LEFT] = true;
            pl->acc.down[LENS_MOUSE_LEFT] = true;
        }
    }
}
static void touch_up(void *data, struct wl_touch *t, uint32_t serial, uint32_t time, int32_t id) {
    (void)t;
    (void)serial;
    (void)time;
    wp_platform *pl = data;
    if (g_touch_active_id == id) {
        g_touch_active_id = -1;
        if (pl->acc.down[LENS_MOUSE_LEFT]) {
            pl->acc.released[LENS_MOUSE_LEFT] = true;
            pl->acc.down[LENS_MOUSE_LEFT] = false;
        }
    }
}
static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id,
                         wl_fixed_t x, wl_fixed_t y) {
    (void)t;
    (void)time;
    wp_platform *pl = data;
    if (g_touch_active_id == id) {
        pl->acc.cx = wl_fixed_to_double(x);
        pl->acc.cy = wl_fixed_to_double(y);
    }
}
static void touch_frame(void *d, struct wl_touch *t) {
    (void)d;
    (void)t;
}
static void touch_cancel(void *d, struct wl_touch *t) {
    (void)d;
    (void)t;
    /* The compositor cancels the entire active sequence; reset state so we
     * do not leave the mouse "stuck down". */
    wp_platform *pl = g_active_pl;
    if (pl) {
        pl->acc.down[LENS_MOUSE_LEFT] = false;
    }
    g_touch_active_id = -1;
}
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t maj,
                        wl_fixed_t min) {
    (void)d;
    (void)t;
    (void)id;
    (void)maj;
    (void)min;
}
static void touch_orientation(void *d, struct wl_touch *t, int32_t id, wl_fixed_t ori) {
    (void)d;
    (void)t;
    (void)id;
    (void)ori;
}

static const struct wl_touch_listener touch_listener = {
    .down = touch_down,
    .up = touch_up,
    .motion = touch_motion,
    .frame = touch_frame,
    .cancel = touch_cancel,
    .shape = touch_shape,
    .orientation = touch_orientation,
};

/* ------------------------------------------------------------------ */
/*  Keyboard (xkbcommon)                                               */
/* ------------------------------------------------------------------ */

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd,
                      uint32_t size) {
    (void)k;
    wp_platform *pl = data;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return;
    }
    struct xkb_keymap *km = xkb_keymap_new_from_string(pl->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
                                                       XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!km)
        return;
    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }
    if (pl->xkb_state)
        xkb_state_unref(pl->xkb_state);
    if (pl->xkb_keymap)
        xkb_keymap_unref(pl->xkb_keymap);
    pl->xkb_keymap = km;
    pl->xkb_state = st;
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf,
                     struct wl_array *keys) {
    (void)k;
    (void)s;
    (void)keys;
    wp_platform *pl = d;
    if (pl->text_input) {
        zwp_text_input_v3_enable(pl->text_input);
        zwp_text_input_v3_set_content_type(pl->text_input, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                           ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
        zwp_text_input_v3_commit(pl->text_input);
    }
    (void)surf;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf) {
    (void)k;
    (void)s;
    (void)surf;
    wp_platform *pl = d;
    if (pl->text_input) {
        zwp_text_input_v3_disable(pl->text_input);
        zwp_text_input_v3_commit(pl->text_input);
    }
}

/* ------------------------------------------------------------------ */
/*  zwp_text_input_v3 listener (IME)                                  */
/* ------------------------------------------------------------------ */

static void ti_enter(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *surface) {
    wp_platform *pl = data;
    (void)ti;
    pl->text_input_surface = surface;
}

static void ti_leave(void *data, struct zwp_text_input_v3 *ti, struct wl_surface *surface) {
    wp_platform *pl = data;
    (void)ti;
    (void)surface;
    pl->text_input_surface = NULL;
}

static void ti_preedit(void *data, struct zwp_text_input_v3 *ti, const char *text,
                       int32_t cursor_begin, int32_t cursor_end) {
    wp_platform *pl = data;
    (void)ti;
    (void)cursor_end;
    if (text) {
        strncpy(pl->ime.preedit, text, sizeof pl->ime.preedit - 1);
        pl->ime.preedit[sizeof pl->ime.preedit - 1] = '\0';
    } else {
        pl->ime.preedit[0] = '\0';
    }
    pl->ime.preedit_cursor_begin = cursor_begin;
    pl->ime.has_preedit = true;
}

static void ti_commit(void *data, struct zwp_text_input_v3 *ti, const char *text) {
    wp_platform *pl = data;
    (void)ti;
    if (text) {
        strncpy(pl->ime.commit, text, sizeof pl->ime.commit - 1);
        pl->ime.commit[sizeof pl->ime.commit - 1] = '\0';
    } else {
        pl->ime.commit[0] = '\0';
    }
    pl->ime.has_commit = true;
}

static void ti_delete_surrounding(void *data, struct zwp_text_input_v3 *ti, uint32_t before,
                                  uint32_t after) {
    wp_platform *pl = data;
    (void)ti;
    pl->ime.delete_before = before;
    pl->ime.delete_after = after;
    pl->ime.has_delete = true;
}

static void ti_done(void *data, struct zwp_text_input_v3 *ti, uint32_t serial) {
    wp_platform *pl = data;
    (void)ti;
    (void)serial;

    /* Apply pending IME state in the order specified by the protocol:
     * 1. Delete surrounding text
     * 2. Insert commit string
     * 3. Set preedit string
     *
     * The actual deletion is performed by lens when it sees the
     * ime_delete_before / ime_delete_after fields in lens_input — we just
     * forward the request through drain_input. */
    if (pl->ime.has_delete) {
        /* Keep has_delete set; drain_input copies the values into lens_input
         * and clears them after the frame. */
    }

    if (pl->ime.has_commit && pl->ime.commit[0]) {
        size_t used = strlen(pl->acc.text);
        size_t n = strlen(pl->ime.commit);
        if (used + n < sizeof pl->acc.text)
            memcpy(pl->acc.text + used, pl->ime.commit, n + 1);
        pl->ime.commit[0] = '\0';
    }

    if (pl->ime.has_preedit) {
        if (pl->ime.preedit[0]) {
            strncpy(pl->acc.preedit, pl->ime.preedit, sizeof pl->acc.preedit - 1);
            pl->acc.preedit[sizeof pl->acc.preedit - 1] = '\0';
            pl->acc.preedit_cursor =
                pl->ime.preedit_cursor_begin >= 0 ? (uint32_t)pl->ime.preedit_cursor_begin : 0;
        } else {
            pl->acc.preedit[0] = '\0';
            pl->acc.preedit_cursor = 0;
        }
    }

    pl->ime.has_commit = false;
    pl->ime.has_preedit = false;
    /* NOTE: delete_before/delete_after are NOT cleared here — drain_input
     * consumes them when it assembles lens_input, then clears. This is so
     * a ti_done arriving between frames still delivers the delete request
     * to lens on the next frame. */
}

static const struct zwp_text_input_v3_listener text_input_listener = {
    .enter = ti_enter,
    .leave = ti_leave,
    .preedit_string = ti_preedit,
    .commit_string = ti_commit,
    .delete_surrounding_text = ti_delete_surrounding,
    .done = ti_done,
};

static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t t, uint32_t key,
                   uint32_t state) {
    (void)k;
    (void)t;
    wp_platform *pl = data;
    if (!pl->xkb_state)
        return;
    pl->last_serial = serial; /* needed for wl_data_device_set_selection */
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);
    xkb_keycode_t code = key + 8; /* evdev -> xkb */
    xkb_keysym_t sym = xkb_state_key_get_one_sym(pl->xkb_state, code);

    if (pressed && pl->acc.key_count < LENS_INPUT_MAX_KEYS) {
        int fk = 0;
        if (sym == XKB_KEY_Escape)
            fk = LENS_KEY_ESCAPE;
        else if (sym == XKB_KEY_Tab)
            fk = LENS_KEY_TAB;
        else if (sym == XKB_KEY_Return)
            fk = LENS_KEY_RETURN;
        else if (sym == XKB_KEY_BackSpace)
            fk = LENS_KEY_BACKSPACE;
        else if (sym == XKB_KEY_Delete)
            fk = LENS_KEY_DELETE;
        else if (sym == XKB_KEY_Left)
            fk = LENS_KEY_LEFT;
        else if (sym == XKB_KEY_Right)
            fk = LENS_KEY_RIGHT;
        else if (sym == XKB_KEY_Up)
            fk = LENS_KEY_UP;
        else if (sym == XKB_KEY_Down)
            fk = LENS_KEY_DOWN;
        else if (sym == XKB_KEY_Home)
            fk = LENS_KEY_HOME;
        else if (sym == XKB_KEY_End)
            fk = LENS_KEY_END;
        /* ASCII letters (and digits) so widgets see Ctrl+C/V/X/A etc.
         * xkb keysyms for these equal their ASCII codepoints. */
        else if (sym >= XKB_KEY_space && sym <= XKB_KEY_asciitilde)
            fk = (int)sym;
        if (fk) {
            pl->acc.keys[pl->acc.key_count++] =
                (lens_key_event){.key = fk, .pressed = true, .repeat = false};
        }
    }

    if (pressed) {
        char buf[8];
        int n = xkb_state_key_get_utf8(pl->xkb_state, code, buf, sizeof buf);
        if (n > 0 && (unsigned char)buf[0] >= 0x20) { /* skip control chars */
            size_t used = strlen(pl->acc.text);
            if (used + (size_t)n < sizeof pl->acc.text)
                memcpy(pl->acc.text + used, buf, (size_t)n + 1);
        }
    }
}
static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                         uint32_t latched, uint32_t locked, uint32_t group) {
    (void)k;
    (void)serial;
    wp_platform *pl = data;
    if (!pl->xkb_state)
        return;
    xkb_state_update_mask(pl->xkb_state, dep, latched, locked, 0, 0, group);
    struct {
        const char *name;
        uint32_t bit;
    } m[] = {
        {XKB_MOD_NAME_SHIFT, LENS_MOD_SHIFT},
        {XKB_MOD_NAME_CTRL, LENS_MOD_CTRL},
        {XKB_MOD_NAME_ALT, LENS_MOD_ALT},
        {XKB_MOD_NAME_LOGO, LENS_MOD_SUPER},
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        bool on =
            xkb_state_mod_name_is_active(pl->xkb_state, m[i].name, XKB_STATE_MODS_EFFECTIVE) > 0;
        if (on)
            pl->acc.mods |= m[i].bit;
        else
            pl->acc.mods &= ~m[i].bit;
    }
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)d;
    (void)k;
    (void)rate;
    (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap,
    .enter = kb_enter,
    .leave = kb_leave,
    .key = kb_key,
    .modifiers = kb_modifiers,
    .repeat_info = kb_repeat,
};

/* ------------------------------------------------------------------ */
/*  Clipboard (wl_data_device) — bridges lens_clipboard to the selection */
/* ------------------------------------------------------------------ */

/* A data offer we might read on paste or drop; remember the most recent
 * text/URI-list one. Both clipboard (selection) and DnD share this listener. */
static void doffer_offer(void *data, struct wl_data_offer *off, const char *mime) {
    wp_platform *pl = data;
    (void)off;
    /* Track only that *some* text type is on offer; receive negotiates below. */
    if (strstr(mime, "text/") || strstr(mime, "uri-list")) {
        /* remember the strongest text mime we've seen on this offer */
    }
    (void)pl;
}
static void doffer_source_actions(void *d, struct wl_data_offer *o, uint32_t a) {
    (void)d;
    (void)o;
    (void)a;
}
static void doffer_action(void *d, struct wl_data_offer *o, uint32_t a) {
    (void)d;
    (void)o;
    (void)a;
}
static const struct wl_data_offer_listener data_offer_listener = {
    .offer = doffer_offer,
    .source_actions = doffer_source_actions,
    .action = doffer_action,
};

/* The compositor announces a new offer object before selection/enter. */
static void ddev_data_offer(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    (void)data;
    (void)dev;
    wl_data_offer_add_listener(offer, &data_offer_listener, data);
}
static void ddev_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    wp_platform *pl = data;
    (void)dev;
    if (pl->selection_offer && pl->selection_offer != offer)
        wl_data_offer_destroy(pl->selection_offer);
    pl->selection_offer = offer; /* may be NULL when selection is cleared */
}

/* DnD enter/motion/leave/drop: report only text/uri-list drops. A real drop
 * reads the offer via wl_data_offer_receive and writes the URIs into the
 * pending buffer; the next frame's lens_input carries the dropped text. */
static const char *dnd_pick_mime(struct wl_data_offer *offer) {
    /* The listener doesn't expose the mime list; assume text/uri-list is
     * always offered (compositors universally provide it for file drags).
     * For raw text drags we fall back to text/plain;charset=utf-8. */
    (void)offer;
    return "text/uri-list";
}

static void dnd_read_and_emit(wp_platform *pl, struct wl_data_offer *offer) {
    int fds[2];
    if (pipe(fds) != 0)
        return;
    wl_data_offer_receive(offer, dnd_pick_mime(offer), fds[1]);
    close(fds[1]);
    wl_display_flush(pl->display);

    /* Read everything; cap at sizeof pl->acc.text so we can land it in the
     * pending IME commit slot, mirroring how paste delivers text to lens. */
    char buf[1024];
    size_t total = 0;
    while (total < sizeof buf - 1) {
        ssize_t n = read(fds[0], buf + total, sizeof buf - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fds[0]);
    buf[total] = '\0';
    /* Strip trailing newline from uri-list format. */
    while (total && (buf[total - 1] == '\n' || buf[total - 1] == '\r'))
        buf[--total] = '\0';

    if (total == 0)
        return;

    /* Deliver as committed text — lens textfield/textarea will insert it
     * the same way paste / IME commit text is inserted. For non-text-drop
     * surfaces this is silently ignored, which is safe. */
    size_t used = strlen(pl->acc.text);
    size_t room = sizeof pl->acc.text - 1 - used;
    if (total <= room)
        memcpy(pl->acc.text + used, buf, total + 1);
    else
        memcpy(pl->acc.text + used, buf, room);
    pl->acc.text[used + (total < room ? total : room)] = '\0';
}

static void ddev_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *su,
                       wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)dev;
    (void)s;
    (void)su;
    wp_platform *pl = d;
    pl->dnd_inside = true;
    pl->dnd_pos = (flux_point){wl_fixed_to_double(x), wl_fixed_to_double(y)};
    if (pl->dnd_offer != o) {
        if (pl->dnd_offer)
            wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = o;
    } else if (o) {
        wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
}
static void ddev_leave(void *d, struct wl_data_device *dev) {
    (void)dev;
    wp_platform *pl = d;
    pl->dnd_inside = false;
    if (pl->dnd_offer) {
        wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = NULL;
    }
}
static void ddev_motion(void *d, struct wl_data_device *dev, uint32_t t, wl_fixed_t x,
                        wl_fixed_t y) {
    (void)dev;
    (void)t;
    wp_platform *pl = d;
    pl->dnd_pos = (flux_point){wl_fixed_to_double(x), wl_fixed_to_double(y)};
}
static void ddev_drop(void *d, struct wl_data_device *dev) {
    (void)dev;
    wp_platform *pl = d;
    if (pl->dnd_offer) {
        dnd_read_and_emit(pl, pl->dnd_offer);
        wl_data_offer_finish(pl->dnd_offer);
        wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = NULL;
    }
    pl->dnd_inside = false;
}
static const struct wl_data_device_listener data_device_listener = {
    .data_offer = ddev_data_offer,
    .enter = ddev_enter,
    .leave = ddev_leave,
    .motion = ddev_motion,
    .drop = ddev_drop,
    .selection = ddev_selection,
};

/* Our outgoing selection: write the stored buffer when a reader asks. */
static void dsource_send(void *data, struct wl_data_source *src, const char *mime, int32_t fd) {
    wp_platform *pl = data;
    (void)src;
    (void)mime;
    const char *p = pl->copy_buf;
    size_t left = pl->copy_len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0)
            break;
        p += w;
        left -= (size_t)w;
    }
    close(fd);
}
static void dsource_cancelled(void *data, struct wl_data_source *src) {
    wp_platform *pl = data;
    wl_data_source_destroy(src);
    if (pl->copy_source == src)
        pl->copy_source = NULL;
}
static void dsource_target(void *d, struct wl_data_source *s, const char *m) {
    (void)d;
    (void)s;
    (void)m;
}
static const struct wl_data_source_listener data_source_listener = {
    .target = dsource_target,
    .send = dsource_send,
    .cancelled = dsource_cancelled,
};

static void wp_maybe_create_data_device(wp_platform *pl) {
    if (pl->data_device || !pl->data_device_mgr || !pl->seat)
        return;
    pl->data_device = wl_data_device_manager_get_data_device(pl->data_device_mgr, pl->seat);
    wl_data_device_add_listener(pl->data_device, &data_device_listener, pl);
}

/* lens_clipboard.set_text — advertise `utf8` as the system selection. */
static void clip_set_text(const char *utf8, size_t len, void *user) {
    wp_platform *pl = user;
    if (!pl->data_device_mgr || !pl->data_device)
        return;

    char *copy = malloc(len ? len : 1);
    if (!copy)
        return;
    memcpy(copy, utf8, len);
    free(pl->copy_buf);
    pl->copy_buf = copy;
    pl->copy_len = len;

    pl->copy_source = wl_data_device_manager_create_data_source(pl->data_device_mgr);
    wl_data_source_add_listener(pl->copy_source, &data_source_listener, pl);
    wl_data_source_offer(pl->copy_source, "text/plain;charset=utf-8");
    wl_data_source_offer(pl->copy_source, "text/plain");
    wl_data_source_offer(pl->copy_source, "UTF8_STRING");
    wl_data_device_set_selection(pl->data_device, pl->copy_source, pl->last_serial);
}

/* lens_clipboard.request_text — read the current selection and hand it back.
 * Reading is synchronous here (an example); we pump the display so our own
 * data source can answer when we are the selection owner. */
static void clip_request_text(void *user) {
    wp_platform *pl = user;
    if (!pl->selection_offer || !pl->ui)
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;
    wl_data_offer_receive(pl->selection_offer, "text/plain;charset=utf-8", fds[1]);
    close(fds[1]);
    wl_display_flush(pl->display);

    char *out = NULL;
    size_t total = 0;
    for (;;) {
        struct pollfd pfd = {.fd = fds[0], .events = POLLIN};
        int pr = poll(&pfd, 1, 200);
        if (pr <= 0) {
            /* Nothing yet — let our own source (or the sender) make progress. */
            if (pr == 0) {
                wl_display_roundtrip(pl->display);
                continue;
            }
            break;
        }
        char buf[4096];
        ssize_t r = read(fds[0], buf, sizeof buf);
        if (r > 0) {
            char *grown = realloc(out, total + (size_t)r);
            if (!grown)
                break;
            out = grown;
            memcpy(out + total, buf, (size_t)r);
            total += (size_t)r;
        } else {
            break; /* EOF or error */
        }
    }
    close(fds[0]);

    if (out)
        lens_paste(pl->ui, out, total);
    free(out);
}

/* ------------------------------------------------------------------ */
/*  Seat                                                               */
/* ------------------------------------------------------------------ */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    wp_platform *pl = data;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    bool has_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

    if (has_ptr && !pl->pointer) {
        pl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pl->pointer, &pointer_listener, pl);
    } else if (!has_ptr && pl->pointer) {
        wl_pointer_destroy(pl->pointer);
        pl->pointer = NULL;
    }
    if (has_kb && !pl->keyboard) {
        pl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(pl->keyboard, &keyboard_listener, pl);
    } else if (!has_kb && pl->keyboard) {
        wl_keyboard_destroy(pl->keyboard);
        pl->keyboard = NULL;
    }
    if (has_touch && !pl->touch) {
        pl->touch = wl_seat_get_touch(seat);
        if (pl->touch)
            wl_touch_add_listener(pl->touch, &touch_listener, pl);
    } else if (!has_touch && pl->touch) {
        wl_touch_destroy(pl->touch);
        pl->touch = NULL;
        g_touch_active_id = -1;
    }

    wp_maybe_create_data_device(pl);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {
    (void)d;
    (void)s;
    (void)n;
}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps,
    .name = seat_name,
};

/* ------------------------------------------------------------------ */
/*  wl_output — track each output's scale for HiDPI                    */
/* ------------------------------------------------------------------ */

static wp_output *find_output(wp_platform *pl, struct wl_output *wl) {
    for (int i = 0; i < pl->n_outputs; i++)
        if (pl->outputs[i].wl == wl)
            return &pl->outputs[i];
    return NULL;
}

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *mk, const char *md, int32_t tr) {
    (void)d;
    (void)o;
    (void)x;
    (void)y;
    (void)pw;
    (void)ph;
    (void)sub;
    (void)mk;
    (void)md;
    (void)tr;
}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d;
    (void)o;
    (void)f;
    (void)w;
    (void)h;
    (void)r;
}
static void out_scale(void *data, struct wl_output *o, int32_t scale) {
    wp_platform *pl = data;
    wp_output *out = find_output(pl, o);
    if (!out)
        return;
    out->scale = scale;
    wp_recompute_scale(pl);
}
static void out_name(void *d, struct wl_output *o, const char *n) {
    (void)d;
    (void)o;
    (void)n;
}
static void out_desc(void *d, struct wl_output *o, const char *n) {
    (void)d;
    (void)o;
    (void)n;
}
static void out_done(void *d, struct wl_output *o) {
    (void)d;
    (void)o;
}

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry,
    .mode = out_mode,
    .done = out_done,
    .scale = out_scale,
    .name = out_name,
    .description = out_desc,
};

/* wl_surface.enter / leave — which outputs the surface is currently on. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)s;
    wp_platform *pl = data;
    wp_output *out = find_output(pl, o);
    if (out) {
        out->entered = true;
        wp_recompute_scale(pl);
    }
}
static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)s;
    wp_platform *pl = data;
    wp_output *out = find_output(pl, o);
    if (out) {
        out->entered = false;
        wp_recompute_scale(pl);
    }
}
/* wl_compositor v6: the compositor tells us the integer buffer scale it
 * wants for this surface. This is the modern, authoritative HiDPI signal
 * (niri and others rely on it instead of wl_output.scale + surface.enter). */
static void surf_preferred_buffer_scale(void *data, struct wl_surface *s, int32_t f) {
    (void)s;
    wp_platform *pl = data;
    if (f >= 1)
        pl->pending_scale = f;
}
static void surf_preferred_buffer_transform(void *d, struct wl_surface *s, uint32_t t) {
    (void)d;
    (void)s;
    (void)t;
}

/* wp_fractional_scale_v1 (wayland-protocols ≥ 1.31, staging) — compositors
 * that expose this global prefer it over wl_surface.preferred_buffer_scale
 * because it carries a 1/120 fractional scale (e.g. 1.25x, 1.5x) instead
 * of an integer. We bind the global and the per-surface object at registry
 * time; the listener records the fractional scale so the lens side can be
 * told the true float content scale. */
static void frac_scale_preferred(void *data, struct wp_fractional_scale_v1 *frac, uint32_t scale) {
    (void)frac;
    wp_platform *pl = data;
    /* scale is in 1/120 steps (so 120 = 1.0, 180 = 1.5, …). Convert to a
     * float first, then a usable integer buffer scale (rounding to nearest
     * so the existing integer-scale pipeline keeps working; the fractional
     * part is preserved on the lens content_scale too). */
    float f = (float)scale / 120.0f;
    int b = (int)(f + 0.5f);
    if (b < 1)
        b = 1;
    pl->pending_scale = b;
    pl->fractional_scale = f;
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = frac_scale_preferred,
};

static const struct wl_surface_listener surface_listener = {
    .enter = surf_enter,
    .leave = surf_leave,
    .preferred_buffer_scale = surf_preferred_buffer_scale,
    .preferred_buffer_transform = surf_preferred_buffer_transform,
};

/* ------------------------------------------------------------------ */
/*  Registry                                                           */
/* ------------------------------------------------------------------ */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
                       uint32_t version) {
    wp_platform *pl = data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        /* v6 gives wl_surface.preferred_buffer_scale (HiDPI). */
        pl->compositor =
            wl_registry_bind(reg, name, &wl_compositor_interface, version < 6 ? version : 6);
    } else if (strcmp(iface, xdg_wm_base_interface.name) == 0) {
        pl->wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(pl->wm_base, &wm_base_listener, pl);
    } else if (strcmp(iface, wl_seat_interface.name) == 0) {
        pl->seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(pl->seat, &seat_listener, pl);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (pl->n_outputs < WP_MAX_OUTPUTS) {
            /* version 2 introduces .scale; ask for it if available. */
            uint32_t v = version < 2 ? version : 2;
            wp_output *out = &pl->outputs[pl->n_outputs++];
            out->wl = wl_registry_bind(reg, name, &wl_output_interface, v);
            out->scale = 1;
            out->entered = false;
            wl_output_add_listener(out->wl, &output_listener, pl);
        }
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        pl->data_device_mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                                               version < 3 ? version : 3);
        wp_maybe_create_data_device(pl);
    } else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0) {
        pl->deco_mgr = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (strcmp(iface, zwp_text_input_manager_v3_interface.name) == 0) {
        pl->text_input_mgr = wl_registry_bind(reg, name, &zwp_text_input_manager_v3_interface, 1);
    } else if (strcmp(iface, wp_cursor_shape_manager_v1_interface.name) == 0) {
        /* cursor-shape-v1 (wayland-protocols ≥ 1.32, staging). The
         * compositor renders the cursor with its own configured theme,
         * so we never load a cursor theme ourselves — the cursor always
         * matches the user's desktop. Optional: compositors that don't
         * expose this global leave iris_set_cursor as a no-op. */
        pl->cursor_shape_mgr =
            wl_registry_bind(reg, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        /* fractional-scale-v1 (wayland-protocols ≥ 1.31, staging). The
         * modern HiDPI signal for fractional scales (1.25x, 1.5x). Falls
         * back to integer wl_surface.preferred_buffer_scale when absent. */
        pl->fractional_scale_mgr =
            wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)d;
    (void)r;
    (void)name;
}
static const struct wl_registry_listener registry_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

/* ------------------------------------------------------------------ */
/*  xdg_surface / xdg_toplevel                                          */
/* ------------------------------------------------------------------ */

static void xdg_surf_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    wp_platform *pl = data;
    xdg_surface_ack_configure(s, serial);
    if (pl->pending_w > 0 && pl->pending_h > 0 &&
        (pl->pending_w != pl->width || pl->pending_h != pl->height)) {
        pl->width = pl->pending_w;
        pl->height = pl->pending_h;
        pl->resized = true;
    }
}
static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surf_configure,
};

static void top_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h,
                          struct wl_array *states) {
    (void)t;
    (void)states;
    wp_platform *pl = data;
    pl->pending_w = w; /* 0 means "you choose"; handled at surf.configure */
    pl->pending_h = h;
}
static void top_close(void *data, struct xdg_toplevel *t) {
    (void)t;
    ((wp_platform *)data)->running = false;
}
static void top_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)d;
    (void)t;
    (void)w;
    (void)h;
}
static void top_wm_caps(void *d, struct xdg_toplevel *t, struct wl_array *c) {
    (void)d;
    (void)t;
    (void)c;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = top_configure,
    .close = top_close,
    .configure_bounds = top_bounds,
    .wm_capabilities = top_wm_caps,
};

/* ------------------------------------------------------------------ */
/*  Build one lens_input from the accumulated state                      */
/* ------------------------------------------------------------------ */

static void drain_input(wp_platform *pl, lens_input *in, float dt) {
    memset(in, 0, sizeof *in);
    in->cursor = (flux_point){(float)pl->acc.cx, (float)pl->acc.cy};
    in->display_size = (flux_point){(float)pl->width, (float)pl->height};
    in->dt_seconds = dt;
    in->mods = pl->acc.mods;
    in->scroll_x = (float)pl->acc.scroll_x;
    in->scroll_y = (float)pl->acc.scroll_y;
    for (int i = 0; i < LENS_MOUSE_COUNT; i++) {
        in->mouse_down[i] = pl->acc.down[i];
        in->mouse_pressed[i] = pl->acc.pressed[i];
        in->mouse_released[i] = pl->acc.released[i];
    }
    memcpy(in->text_utf8, pl->acc.text, sizeof in->text_utf8);
    memcpy(in->preedit_utf8, pl->acc.preedit, sizeof in->preedit_utf8);
    in->preedit_cursor = pl->acc.preedit_cursor;
    in->key_count = pl->acc.key_count;
    for (uint32_t i = 0; i < pl->acc.key_count; i++)
        in->keys[i] = pl->acc.keys[i];

    /* Forward IME delete_surrounding (text-input-v3). Lens consumes it
     * in textfield/textarea to honour compositor corrections (e.g. when
     * an IME replaces "teh" -> "the", it asks us to delete "teh" first). */
    if (pl->ime.delete_before || pl->ime.delete_after) {
        in->ime_delete_before = pl->ime.delete_before;
        in->ime_delete_after = pl->ime.delete_after;
    }

    /* clear per-frame edges; keep level state (down/cursor/mods) */
    for (int i = 0; i < LENS_MOUSE_COUNT; i++)
        pl->acc.pressed[i] = pl->acc.released[i] = false;
    pl->acc.scroll_x = pl->acc.scroll_y = 0.0;
    pl->acc.key_count = 0;
    pl->acc.text[0] = '\0';
    pl->ime.delete_before = pl->ime.delete_after = 0;
    pl->ime.has_delete = false;
}

static void log_raw(const lens_input *in) {
    static const char *names[LENS_MOUSE_COUNT] = {"left", "right", "middle"};
    for (int b = 0; b < LENS_MOUSE_COUNT; b++) {
        if (in->mouse_pressed[b])
            printf("[raw] mouse press   %s\n", names[b]);
        if (in->mouse_released[b])
            printf("[raw] mouse release %s\n", names[b]);
    }
    if (in->scroll_x != 0.0f || in->scroll_y != 0.0f)
        printf("[raw] scroll dx=%.2f dy=%.2f\n", in->scroll_x, in->scroll_y);
    for (uint32_t k = 0; k < in->key_count; k++)
        printf("[raw] key %d down\n", in->keys[k].key);
}

/* ------------------------------------------------------------------ */
/*  Pump Wayland events without blocking the render loop               */
/* ------------------------------------------------------------------ */

static void pump_events(wp_platform *pl, int timeout_ms) {
    struct wl_display *d = pl->display;
    while (wl_display_prepare_read(d) != 0)
        wl_display_dispatch_pending(d);
    wl_display_flush(d);

    /* Poll the Wayland display fd and (if active) the theme watcher and
     * AT-SPI bus fds together so we wake up on any. `timeout_ms` is the
     * frame-pacing budget: a non-blocking (vsync=false) present leaves the
     * render loop with no point that sleeps, so we block here for up to a
     * frame's worth of time. poll() returns the instant input (or a theme /
     * a11y event) arrives — keeping latency low — and otherwise wakes on the
     * deadline so time-based UI (caret blink, lens animations) keeps ticking
     * even with no input. Passing 0 keeps the old non-blocking behaviour. */
    struct pollfd pfds[3];
    int n = 1;
    int theme_idx = -1, a11y_idx = -1;
    pfds[0] = (struct pollfd){.fd = wl_display_get_fd(d), .events = POLLIN};
    if (pl->theme_fd >= 0) {
        /* sd-bus sockets are level-triggered: poll the mask the connection
         * actually wants (sd_bus_get_events), never a hard-coded POLLIN. A
         * bare POLLIN spins the loop because the socket can read-ready with
         * no sd-bus work pending, defeating the frame-pacing sleep below. */
        short ev = iris_color_scheme_watcher_poll_events();
        if (ev != 0) {
            theme_idx = n;
            pfds[n] = (struct pollfd){.fd = pl->theme_fd, .events = ev};
            n++;
        }
    }
    if (pl->a11y_fd >= 0) {
        /* AT-SPI is also an sd-bus socket — poll its requested mask, not a
         * bare POLLIN (see the theme fd above). */
        short ev = iris_a11y_poll_events();
        if (ev != 0) {
            a11y_idx = n;
            pfds[n] = (struct pollfd){.fd = pl->a11y_fd, .events = ev};
            n++;
        }
    }
    int pr = poll(pfds, n, timeout_ms);
    if (pr > 0) {
        if (pfds[0].revents & POLLIN)
            wl_display_read_events(d);
        else
            wl_display_cancel_read(d);
        /* Any signalled event (POLLIN / POLLHUP / POLLERR) — let sd-bus
         * process it so an error/hangup is consumed rather than re-firing. */
        if (theme_idx >= 0 && pfds[theme_idx].revents)
            iris_pump_color_scheme_watcher();
        if (a11y_idx >= 0 && pfds[a11y_idx].revents)
            iris_a11y_pump();
    } else {
        wl_display_cancel_read(d);
    }

    wl_display_dispatch_pending(d);
}

/* Monotonic nanoseconds, for frame pacing. */
static long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + (long long)t.tv_nsec;
}

/* True if the input accumulator holds genuine user activity (pointer motion,
 * buttons, scroll, keys, or text/IME) since the last check. Frame-callback and
 * buffer-release traffic from our own present does NOT touch the accumulator,
 * so this stays false when the user is idle — which is what lets the loop drop
 * to a low idle frame rate. `*pcx`/`*pcy` carry the previous cursor position so
 * motion can be detected; they are updated in place. */
static bool acc_has_user_input(wp_platform *pl, double *pcx, double *pcy) {
    bool moved = (pl->acc.cx != *pcx) || (pl->acc.cy != *pcy);
    *pcx = pl->acc.cx;
    *pcy = pl->acc.cy;
    bool edge = pl->acc.key_count != 0 || pl->acc.text[0] != '\0' || pl->acc.preedit[0] != '\0' ||
                pl->acc.scroll_x != 0.0 || pl->acc.scroll_y != 0.0;
    for (int i = 0; i < LENS_MOUSE_COUNT; i++)
        edge = edge || pl->acc.pressed[i] || pl->acc.released[i] || pl->acc.down[i];
    return moved || edge;
}

/* ------------------------------------------------------------------ */
/*  System theme detection (best-effort, no extra deps)               */
/* ------------------------------------------------------------------ */

/* System colour-scheme query is provided by iris (src/theme_linux.c).
 * Drop the local copy so we have one source of truth and so future live
 * updates / portal queries plug in here. */

/* ------------------------------------------------------------------ */
/*  Vulkan surface helpers (platform-specific)                        */
/* ------------------------------------------------------------------ */

static VkSurfaceKHR wp_create_vk_surface(const flux_device *device, struct wl_display *display,
                                         struct wl_surface *wl_surface) {
    VkWaylandSurfaceCreateInfoKHR wsci = {
        .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
        .display = display,
        .surface = wl_surface,
    };
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    if (vkCreateWaylandSurfaceKHR(flux_device_vk_instance(device), &wsci, NULL, &vk_surface) !=
        VK_SUCCESS)
        return VK_NULL_HANDLE;
    return vk_surface;
}

static void wp_destroy_vk_surface(const flux_device *device, VkSurfaceKHR vk_surface) {
    if (vk_surface && device)
        vkDestroySurfaceKHR(flux_device_vk_instance(device), vk_surface, NULL);
}

/* ------------------------------------------------------------------ */
/*  Run                                                                */
/* ------------------------------------------------------------------ */

int iris_app_run_wayland(const iris_app_config *cfg) {
    /* All resources declared up front and NULL/VK_NULL_HANDLE-initialized so
     * a single `fail:` cleanup block can run on every exit path (error or
     * success) without referencing out-of-scope or indeterminate pointers. */
    flux_device *device = NULL;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    flux_surface *surface = NULL;
    flux_canvas *canvas = NULL;
    lens *ui = NULL;
    int rc = 1; /* pessimistic; set to 0 only on success */

    wp_platform pl = {
        .running = true,
        .width = cfg->width > 0 ? cfg->width : 960,
        .height = cfg->height > 0 ? cfg->height : 720,
        .buffer_scale = 1,
        .pending_scale = 1,
        .current_cursor = IRIS_CURSOR_DEFAULT,
        .theme_fd = -1, /* so the cleanup guards are correct even if
                         * we fail before the watcher is started */
        .a11y_fd = -1,
    };

    /* Publish `pl` as the active app instance so the context-free
     * iris_set_cursor() can reach it. Cleared on the way out (success
     * or fail). */
    g_active_pl = &pl;

    /* --- Wayland connection + globals ---------------------------- */
    pl.display = wl_display_connect(NULL);
    if (!pl.display) {
        fprintf(stderr, "no Wayland display (is a compositor running?)\n");
        return 1;
    }
    pl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!pl.xkb_ctx) {
        fprintf(stderr, "xkb_context_new failed\n");
        goto fail;
    }

    pl.registry = wl_display_get_registry(pl.display);
    wl_registry_add_listener(pl.registry, &registry_listener, &pl);
    wl_display_roundtrip(pl.display); /* bind globals */
    wl_display_roundtrip(pl.display); /* seat caps -> pointer/keyboard */

    if (pl.text_input_mgr && pl.seat) {
        pl.text_input = zwp_text_input_manager_v3_get_text_input(pl.text_input_mgr, pl.seat);
        if (pl.text_input)
            zwp_text_input_v3_add_listener(pl.text_input, &text_input_listener, &pl);
    }

    if (!pl.compositor || !pl.wm_base) {
        fprintf(stderr, "compositor missing wl_compositor / xdg_wm_base\n");
        goto fail;
    }

    /* --- Vulkan instance via flux (Wayland WSI extensions) ------- */
    const char *inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME,
    };
    const char *dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    flux_device_desc ddesc = {
        .type = FLUX_TYPE_DEVICE_DESC,
        .log = flux_console_logger,
        .validation = FLUX_VALIDATION_AUTO,
        .required_instance_extensions = inst_exts,
        .required_instance_extension_count = sizeof inst_exts / sizeof *inst_exts,
        .required_device_extensions = dev_exts,
        .required_device_extension_count = sizeof dev_exts / sizeof *dev_exts,
        .frames_in_flight = 2,
    };
    if (flux_device_create(&ddesc, &device) != FLUX_OK) {
        fprintf(stderr, "flux_device_create failed\n");
        goto fail;
    }

    /* --- xdg-shell window --------------------------------------- */
    pl.surface = wl_compositor_create_surface(pl.compositor);
    wl_surface_add_listener(pl.surface, &surface_listener, &pl);
    pl.xdg_surface = xdg_wm_base_get_xdg_surface(pl.wm_base, pl.surface);
    xdg_surface_add_listener(pl.xdg_surface, &xdg_surface_listener, &pl);
    pl.toplevel = xdg_surface_get_toplevel(pl.xdg_surface);
    xdg_toplevel_add_listener(pl.toplevel, &toplevel_listener, &pl);
    xdg_toplevel_set_title(pl.toplevel, cfg->title ? cfg->title : "iris");
    xdg_toplevel_set_app_id(pl.toplevel,
                            cfg->app_id ? cfg->app_id : "ai.opencode.iris");

    /* Bind a fractional-scale object if the compositor exposes the global;
     * it carries the precise 1/120-step scale used on HiDPI mixed-DPI
     * desktops (Windows portability scenarios, fractional laptop scales). */
    if (pl.fractional_scale_mgr) {
        pl.fractional_scale_obj = wp_fractional_scale_manager_v1_get_fractional_scale(
            pl.fractional_scale_mgr, pl.surface);
        if (pl.fractional_scale_obj)
            wp_fractional_scale_v1_add_listener(pl.fractional_scale_obj,
                                                &fractional_scale_listener, &pl);
    }

    /* Ask the compositor for a server-side title bar when it can. */
    if (pl.deco_mgr) {
        pl.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(pl.deco_mgr, pl.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(pl.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        fprintf(stderr, "note: compositor offers no server-side decorations "
                        "(no title bar)\n");
    }

    wl_surface_commit(pl.surface);
    wl_display_roundtrip(pl.display); /* receive + ack initial configure */
    wl_display_roundtrip(pl.display); /* receive .enter(output) for scale */

    /* If no output reported yet (rare), keep the default scale = 1; the
     * frame loop will pick up changes via pl.pending_scale. */
    if (pl.pending_scale > 0)
        pl.buffer_scale = pl.pending_scale;

    /* --- Vulkan surface + flux surface/canvas ------------------- */
    vk_surface = wp_create_vk_surface(device, pl.display, pl.surface);
    if (!vk_surface) {
        fprintf(stderr, "vkCreateWaylandSurfaceKHR failed\n");
        goto fail;
    }

    /* Swapchain is sized in *device* pixels — logical × buffer_scale —
     * while layout and lens_input stay in logical units. */
    wl_surface_set_buffer_scale(pl.surface, pl.buffer_scale);
    wl_surface_commit(pl.surface);

    flux_surface_desc sdesc = {
        .type = FLUX_TYPE_SURFACE_DESC,
        .vk_surface_khr = vk_surface,
        .width = (uint32_t)(pl.width * pl.buffer_scale),
        .height = (uint32_t)(pl.height * pl.buffer_scale),
        /* Non-blocking present (MAILBOX, or IMMEDIATE as fallback — see
         * pick_present_mode in flux). A FIFO/vsync present blocks the main
         * thread in vkAcquireNextImageKHR until the compositor releases an
         * image, freezing input handling and time-based UI between frames;
         * the frame loop paces itself instead (see below). */
        .vsync = false,
    };
    if (flux_surface_create(device, &sdesc, &surface) != FLUX_OK) {
        fprintf(stderr, "flux_surface_create failed\n");
        goto fail;
    }

    if (flux_canvas_create(&(flux_canvas_desc){.type = FLUX_TYPE_CANVAS_DESC, .surface = surface},
                           &canvas) != FLUX_OK) {
        fprintf(stderr, "flux_canvas_create failed\n");
        goto fail;
    }

    if (lens_create(&(lens_desc){.device = device,
                                 .theme = cfg->dark
                                              ? lens_theme_dark()
                                              : (iris_system_prefers_dark() ? lens_theme_dark()
                                                                            : lens_theme_default()),
                                 .scale = (float)pl.buffer_scale,
                                 .clipboard = {.set_text = clip_set_text,
                                               .request_text = clip_request_text,
                                               .user = &pl}},
                    &ui) != FLUX_OK) {
        fprintf(stderr, "lens_create failed\n");
        goto fail;
    }

    /* --- Frame loop --------------------------------------------------
     * The surface presents without vsync (non-blocking, see flux_surface_desc
     * above): a FIFO/vsync present blocks the main thread in
     * vkAcquireNextImageKHR until the compositor releases an image, and while
     * blocked there the loop can neither pump input nor advance time-based UI —
     * the window appears frozen and "only updates on input". Instead we present
     * immediately and pace the loop ourselves: we sleep inside pump_events'
     * poll() until the next frame is due (or until real input wakes us early).
     *
     * The rate is adaptive. Right after user input we run at ~60 Hz so hover /
     * scroll / drag stay smooth; once input goes quiet we drop to a low idle
     * rate — just enough to keep the blinking caret and any settling animation
     * alive — so an idle window costs almost no CPU instead of re-laying-out
     * the whole document 60 times a second. */
    const long long ACTIVE_PERIOD_NS = 16666667LL; /* ~60 Hz when active    */
    const long long IDLE_PERIOD_NS = 250000000LL;  /* ~4 Hz when idle: just  */
                                                   /* enough for the caret   */
                                                   /* blink (500ms period).  */
    const long long INPUT_GRACE_NS = 400000000LL;  /* stay fast 400ms after  */

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    int frame_no = 0;

    long long next_deadline = now_ns();
    long long last_input_ns = next_deadline;
    long long last_render_ns = next_deadline - ACTIVE_PERIOD_NS;
    double prev_cx = pl.acc.cx, prev_cy = pl.acc.cy;

    pl.ui = ui;

    /* Live colour-scheme watching: only when not forcing dark. The watcher
     * callback updates the lens theme in place; the next frame renders with
     * the new palette. -1 means the feature is unavailable (no libsystemd
     * at build time, or portal unreachable) — silently degrade. */
    pl.theme_fd = -1;
    pl.a11y_fd = -1;
    if (!cfg->dark) {
        if (iris_watch_system_color_scheme(wp_on_color_scheme_changed, &pl) == 0)
            pl.theme_fd = iris_color_scheme_watcher_fd();
    }

    /* AT-SPI bridge: register the app on the a11y session bus so screen
     * readers (orca, etc.) can read the widget tree. Fail-soft: if the
     * bridge is unavailable we silently skip — the app still runs. */
    if (iris_a11y_init() == 0) {
        pl.a11y_fd = iris_a11y_fd();
    }

    while (pl.running) {
        /* Sleep (inside poll) until the next frame is due, waking early on any
         * Wayland / theme / a11y event. Gating the render on a deadline is what
         * lets poll() actually block: presenting every iteration would make the
         * compositor flood us with frame-callback events so poll() never sleeps
         * and the loop spins at 100% CPU. */
        long long t = now_ns();
        long long budget_ns = next_deadline - t;
        if (budget_ns > 0) {
            int budget_ms = (int)(budget_ns / 1000000LL);
            if (budget_ms > 200)
                budget_ms = 200;
            pump_events(&pl, budget_ms);
        } else {
            pump_events(&pl, 0);
        }

        /* Real user input wakes us out of the idle rate: pull the next render
         * forward (but never sooner than one active period after the last one,
         * so a burst of motion events can't exceed ~60 Hz) and mark the moment
         * so we stay at the active rate through the grace window. */
        if (acc_has_user_input(&pl, &prev_cx, &prev_cy)) {
            last_input_ns = now_ns();
            long long earliest = last_render_ns + ACTIVE_PERIOD_NS;
            if (next_deadline > earliest)
                next_deadline = earliest;
        }

        /* Not time to draw yet — keep draining events and sleeping. */
        if (now_ns() < next_deadline)
            continue;

        /* Render this iteration. Schedule the next deadline relative to now
         * (no catch-up bursts): fast while input is recent, slow when idle.
         * A host animation request made by the previous frame also keeps this
         * iteration at the active cadence. */
        t = now_ns();
        bool host_animating = pl.animation_frame_requested;
        pl.animation_frame_requested = false;
        long long period = (t - last_input_ns < INPUT_GRACE_NS || host_animating)
                               ? ACTIVE_PERIOD_NS
                               : IDLE_PERIOD_NS;
        next_deadline = t + period;
        last_render_ns = t;

        /* Scale change (e.g. surface dragged to a HiDPI output): apply
         * the new buffer scale, resize the swapchain in device pixels,
         * and tell lens so its replay transform matches. */
        if (pl.pending_scale > 0 && pl.pending_scale != pl.buffer_scale) {
            pl.buffer_scale = pl.pending_scale;
            wl_surface_set_buffer_scale(pl.surface, pl.buffer_scale);
            wl_surface_commit(pl.surface);
            lens_set_scale(ui, (float)pl.buffer_scale);
            pl.resized = true;
        }
        if (pl.resized) {
            (void)flux_surface_resize(surface, (uint32_t)(pl.width * pl.buffer_scale),
                                      (uint32_t)(pl.height * pl.buffer_scale));
            pl.resized = false;
        }

        flux_frame *frame = NULL;
        flux_result r = flux_surface_begin_frame(surface, NULL, &frame);
        if (r == FLUX_ERROR_SURFACE_LOST) {
            (void)flux_surface_resize(surface, (uint32_t)(pl.width * pl.buffer_scale),
                                      (uint32_t)(pl.height * pl.buffer_scale));
            continue;
        }
        if (r == FLUX_ERROR_INVALID_STATE)
            continue;
        if (r != FLUX_OK)
            break;

        flux_surface_info info;
        flux_surface_get_info(surface, &info);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - prev.tv_sec) + (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
        if (dt <= 0.0f)
            dt = 1.0f / 60.0f;
        prev = now;

        lens_input in;

        /* Drain a click requested by an AT-SPI Action.DoAction call (if the
         * a11y bridge is running). lens is input-driven — there is no
         * programmatic activate API — so we synthesize a press+release at
         * the widget's centre this frame and lens reports a `clicked`
         * response for it. Single in-flight click; safe to call every frame. */
        if (pl.a11y_fd >= 0) {
            flux_point pc;
            if (iris_a11y__take_pending_click(&pc)) {
                pl.acc.cx = pc.x;
                pl.acc.cy = pc.y;
                pl.acc.pressed[LENS_MOUSE_LEFT] = true;
                pl.acc.released[LENS_MOUSE_LEFT] = true;
                pl.acc.down[LENS_MOUSE_LEFT] = false;
            }
        }

        drain_input(&pl, &in, dt);
        if (cfg->log_raw)
            log_raw(&in);

        lens_begin(ui, &in);
        if (cfg->build)
            cfg->build(ui, &in, cfg->user);
        lens_end(ui);

        /* Push the live semantic tree to the AT-SPI bridge so screen
         * readers see the current widget names / roles / focus. No-op
         * when the bridge isn't running. */
        if (pl.a11y_fd >= 0)
            iris_a11y_update(ui);

        /* Update IME cursor rectangle */
        if (pl.text_input && pl.text_input_surface) {
            flux_rect caret = lens_caret_rect(ui);
            if (caret.w > 0.0f) {
                zwp_text_input_v3_set_cursor_rectangle(pl.text_input, (int32_t)caret.x,
                                                       (int32_t)caret.y, (int32_t)caret.w,
                                                       (int32_t)caret.h);
                zwp_text_input_v3_commit(pl.text_input);
            }
        }

        /* Clear to the current theme's body background so empty areas
         * (e.g. short content in a tall window) don't show a hard-coded
         * dark color in light mode. The paint callback (if any) draws
         * *under* lens's chrome: iris calls it before lens_render so the
         * host's document surface lands first and lens's widget layer
         * composites on top. */
        lens_theme th = lens_get_theme(ui);
        flux_color clear = th.color_bg;
        if (flux_canvas_begin(canvas, frame, &clear) == FLUX_OK) {
            if (cfg->paint)
                cfg->paint(canvas, device, (float)pl.buffer_scale, cfg->user);
            (void)lens_render(ui, canvas);
            flux_canvas_end(canvas);
        }

        if (flux_frame_submit(frame) != FLUX_OK)
            break;
        r = flux_frame_present(frame);
        if (r == FLUX_ERROR_SURFACE_LOST)
            (void)flux_surface_resize(surface, (uint32_t)(pl.width * pl.buffer_scale),
                                      (uint32_t)(pl.height * pl.buffer_scale));
        else if (r != FLUX_OK)
            break;

        /* build/paint may have requested a follow-up after the tentative idle
         * deadline was selected above. Pull that deadline forward now so the
         * next frame arrives at the active cadence. */
        if (pl.animation_frame_requested)
            next_deadline = last_render_ns + ACTIVE_PERIOD_NS;

        if (++frame_no == 1)
            printf("first frame presented: %dx%d logical, %ux%u device (scale=%d)\n", pl.width,
                   pl.height, info.width, info.height, pl.buffer_scale);
    }

    rc = 0; /* success — fall through to the unified cleanup below */

    /* --- Cleanup (shared by the success path and every `goto fail`) --- */
fail:
    /* Stop publishing this wp_platform to iris_set_cursor() — any host
     * call after this returns is a no-op rather than a use-after-free. */
    g_active_pl = NULL;

    /* GPU side first: let in-flight work finish before tearing down objects.
     * flux/lens calls are guarded so an early failure (before a resource
     * existed) doesn't dereference NULL. */
    if (device)
        flux_device_wait_idle(device);
    if (ui)
        lens_destroy(ui);
    if (pl.theme_fd >= 0)
        iris_stop_color_scheme_watcher();
    if (pl.a11y_fd >= 0)
        iris_a11y_shutdown();
    if (canvas)
        flux_canvas_destroy(canvas);
    if (surface)
        flux_surface_release(surface);
    if (vk_surface)
        wp_destroy_vk_surface(device, vk_surface);
    if (device)
        flux_device_release(device);

    /* Cursor: cursor-shape-v1 device + manager. */
    if (pl.cursor_shape_device)
        wp_cursor_shape_device_v1_destroy(pl.cursor_shape_device);
    if (pl.cursor_shape_mgr)
        wp_cursor_shape_manager_v1_destroy(pl.cursor_shape_mgr);

    /* Clipboard buffers we own (clip_set_text mallocs copy_buf; the data
     * offer / source are normally released by the compositor, but free the
     * heap buffer and any still-live offer so teardown doesn't leak). */
    free(pl.copy_buf);
    if (pl.copy_source)
        wl_data_source_destroy(pl.copy_source);
    if (pl.selection_offer)
        wl_data_offer_destroy(pl.selection_offer);

    if (pl.deco)
        zxdg_toplevel_decoration_v1_destroy(pl.deco);
    if (pl.toplevel)
        xdg_toplevel_destroy(pl.toplevel);
    if (pl.xdg_surface)
        xdg_surface_destroy(pl.xdg_surface);
    if (pl.surface)
        wl_surface_destroy(pl.surface);
    for (int i = 0; i < pl.n_outputs; i++)
        if (pl.outputs[i].wl)
            wl_output_destroy(pl.outputs[i].wl);
    if (pl.xkb_state)
        xkb_state_unref(pl.xkb_state);
    if (pl.xkb_keymap)
        xkb_keymap_unref(pl.xkb_keymap);
    if (pl.xkb_ctx)
        xkb_context_unref(pl.xkb_ctx);
    if (pl.pointer)
        wl_pointer_destroy(pl.pointer);
    if (pl.keyboard)
        wl_keyboard_destroy(pl.keyboard);
    if (pl.text_input)
        zwp_text_input_v3_destroy(pl.text_input);
    if (pl.data_device)
        wl_data_device_release(pl.data_device);
    if (pl.display) {
        wl_display_roundtrip(pl.display); /* let the compositor process destroys */
        wl_display_disconnect(pl.display);
    }
    return rc;
}
