/* app_wayland.c — native Wayland window + Vulkan (via flux) + lens_input.
 *
 * Replaces the GLFW glue the examples used to share. Pure Wayland:
 * xdg-shell for the toplevel, xdg-decoration for a server-side title bar
 * when available, xkbcommon for the keymap, and VK_KHR_wayland_surface to
 * hand flux core a VkSurfaceKHR. Pointer and keyboard events are folded
 * into one lens_input per frame (ADR-0029).
 */

#include "a11y_internal.h"
#include "platform_input.h"
#include "platform_internal.h"
#include "platform_text.h"
#include "platform_wakeup.h"
#include "tablet_wayland.h"
#include "theme_watch_internal.h"

#include <iris/a11y.h>
#include <iris/cursor.h>
#include <iris/theme.h>

#include <flux/flux.h>
#include <flux/vulkan.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_wayland.h>

#include "primary-selection-unstable-v1-client-protocol.h"
#include "tablet-unstable-v2-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include <linux/input-event-codes.h> /* BTN_LEFT / BTN_RIGHT / BTN_MIDDLE — Linux-only, this file only */

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
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
    char text[256];                 /* committed text this frame; sized
                                       like lens_input.text_utf8       */
    char preedit[LENS_PREEDIT_MAX]; /* IME preedit string             */
    uint32_t preedit_cursor;        /* caret byte offset in preedit   */
    uint32_t preedit_sel_lo;        /* active clause, byte range      */
    uint32_t preedit_sel_hi;
    lens_key_event keys[LENS_INPUT_MAX_KEYS];
    uint32_t key_count;
} wp_accum;

/* The staging buffers feed lens_input by whole-buffer memcpy in
 * drain_input; pin the sizes so a lens-side change fails at compile time
 * instead of over-reading the accumulator. */
static_assert(sizeof((wp_accum *)0)->text == sizeof((lens_input *)0)->text_utf8,
              "wp_accum.text must match lens_input.text_utf8");
static_assert(sizeof((wp_accum *)0)->preedit == sizeof((lens_input *)0)->preedit_utf8,
              "wp_accum.preedit must match lens_input.preedit_utf8");

/* ------------------------------------------------------------------ */
/*  Platform state                                                     */
/* ------------------------------------------------------------------ */

/* One wl_output binding so we can read its scale. */
#define WP_MAX_OUTPUTS 8
typedef struct wp_output {
    struct wl_output *wl;
    uint32_t name; /* registry global name (for global_remove) */
    int scale;     /* output integer scale (1, 2, 3 …) */
    bool entered;  /* surface currently on this output */
} wp_output;

/* MIME types advertised by a data/primary-selection offer. Offers announce
 * their types via offer events BEFORE the selection/enter event binds them,
 * so types accumulate into a `pending` slot first and move into the matching
 * bound slot when the offer is bound. This is what real MIME negotiation
 * (paste / drop) keys off. The `offer` member is an identity cookie only —
 * it holds a wl_data_offer or a zwp_primary_selection_offer_v1 and is only
 * ever compared, never dereferenced through this struct. */
struct wp_offer_mimes {
    struct wl_data_offer *offer;
    bool uri_list;   /* text/uri-list              */
    bool text_utf8;  /* text/plain;charset=utf-8   */
    bool text_plain; /* text/plain                 */
};

typedef struct wp_platform {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    uint32_t seat_name; /* registry global name (for global_remove) */
    struct zxdg_decoration_manager_v1 *deco_mgr;

    struct wl_data_device_manager *data_device_mgr;
    struct wl_data_device *data_device;
    struct wl_data_offer *selection_offer; /* current clipboard offer */
    struct wl_data_offer *dnd_offer;       /* current drag offer         */
    struct wl_data_offer *dnd_job_offer;   /* offer owned by an in-flight
                                              async drop read (item: the
                                              read runs on a helper thread;
                                              finish+destroy on delivery)  */
    struct wl_data_source *copy_source;    /* our outgoing selection  */
    char *copy_buf;                        /* text we currently advertise for copy     */
    size_t copy_len;
    uint32_t last_serial; /* most recent input serial (set_selection) */
    bool dnd_inside;      /* drag currently over our surface           */
    flux_point dnd_pos;   /* last reported drag position (surface px)  */

    /* Primary selection (zwp_primary_selection_unstable_v1): the X11-style
     * middle-click clipboard. clip_set_text mirrors the copy onto it; a
     * middle-button press requests its text and pastes it (async, like the
     * clipboard paste path). */
    struct zwp_primary_selection_device_manager_v1 *primsel_mgr;
    struct zwp_primary_selection_device_v1 *primsel_device;
    struct zwp_primary_selection_offer_v1 *primsel_offer;
    struct zwp_primary_selection_source_v1 *primsel_source;
    struct wp_offer_mimes primsel_pending_mimes, primsel_offer_mimes;

    struct wp_offer_mimes pending_offer_mimes, selection_offer_mimes, dnd_offer_mimes;

    /* Pointer + keyboard + touch (touch is optional). */
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_touch *touch;

    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *deco;

    /* xdg-foreign-unstable-v2: exports a stable handle naming this window,
     * passed to portal dialogs (file_dialog_portal.c) as parent_window so
     * the picker stays modal to us. */
    struct zxdg_exporter_v2 *foreign_exporter;
    struct zxdg_exported_v2 *foreign_exported;
    char foreign_handle[80]; /* empty until the compositor answers */

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
    iris_cursor host_cursor;      /* last explicit iris_set_cursor (DEFAULT = none) */
    iris_cursor effective_cursor; /* what the compositor is asked to show       */
    uint32_t cursor_serial;       /* most recent enter/motion serial */
    bool cursor_inside;           /* pointer currently inside our surface */
    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;

    /* Dead-key composition (xkbcommon-compose): fed each non-repeat key
     * press; a completed sequence replaces the key's own UTF-8 text. NULL
     * when no compose table exists for the locale (dead keys then fall
     * through as ordinary keysyms). */
    struct xkb_compose_table *compose_table;
    struct xkb_compose_state *compose_state;

    /* Client-side key repeat, driven by the compositor's repeat_info: a
     * timerfd in pump_events' poll set fires after `repeat_delay` ms and
     * then every 1000/`repeat_rate` ms while `rep_key` is held. rate 0 (or
     * no repeat_info ever received, e.g. wl_keyboard < v4) disables repeat.
     * Cancelled on key release, kb_leave, a new key press, or a modifier
     * change. */
    int repeat_fd;
    int32_t repeat_rate;  /* chars/sec, 0 = disabled */
    int32_t repeat_delay; /* ms before the first repeat */
    uint32_t rep_key;     /* evdev keycode being repeated */
    int rep_lens_key;     /* its resolved lens key id (0 = unmappable) */
    bool rep_active;      /* timer armed */

    struct zwp_text_input_manager_v3 *text_input_mgr;
    struct zwp_text_input_v3 *text_input;
    struct wl_surface *text_input_surface;

    /* Per-widget IM ownership: the text input is enabled exactly while the
     * window has keyboard focus AND a lens text widget is focused
     * (re-evaluated once per frame after lens_end — see im_frame_update).
     * `im_surr` is the last surrounding text reported to the compositor, so
     * set_surrounding_text goes out only on content/cursor changes. */
    bool kb_focused;
    bool im_active;
    char *im_surr;
    size_t im_surr_len;
    uint32_t im_surr_cursor;

    /* Pending IME state (double-buffered by text-input-v3 done event).
     * commit is sized like lens_input.text_utf8: a full-sentence IME
     * conversion must survive staging before the boundary-aware append
     * clips it into the per-frame accumulator. */
    struct {
        char commit[256];
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
    int buffer_scale;  /* max scale of entered outputs; ≥1     */
    int pending_scale; /* recomputed from enter/leave + globals */

    struct wp_fractional_scale_manager_v1 *fractional_scale_mgr;
    struct wp_fractional_scale_v1 *fractional_scale_obj;

    int width, height;        /* surface size in *logical* pixels      */
    int pending_w, pending_h; /* from the latest toplevel.configure    */
    int min_w, min_h;         /* size hints last sent to the compositor */
    int max_w, max_h;         /* 0 = no limit                            */
    bool running;
    bool resized;                   /* size or scale changed -> resize swap  */
    bool animation_frame_requested; /* host asked for active-rate follow-up */
    bool paint_static;              /* host declared this frame's canvas content static */
    lens *ui;                       /* so output/scale callbacks can update  */

    /* Live colour-scheme watching + AT-SPI bridge: optional, fail-soft. */
    bool theme_watching;
    int a11y_fd;

    /* Cross-thread wakeup seam (platform_wakeup.h): an eventfd in the
     * pump_events poll set. The colour-scheme watcher's pump thread posts
     * through it; the loop drains the queue on this thread. */
    int wakeup_fd;

    wp_accum acc;
} wp_platform;

/* Theme watcher callback: invoked on the iris main thread (delivered
 * through the wakeup seam) when the portal emits a SettingChanged for
 * org.freedesktop.appearance.color-scheme. */
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

/* evdev button code (linux/input-event-codes.h) → lens mouse index, via
 * the shared iris_pointer_button layer (platform_input.h) so the
 * Linux-only header never leaks past this file. Unknown buttons (side /
 * extra) return -1 and are ignored. */
static int mouse_index(uint32_t button) {
    iris_pointer_button b;
    switch (button) {
    case BTN_LEFT:
        b = IRIS_POINTER_BUTTON_LEFT;
        break;
    case BTN_RIGHT:
        b = IRIS_POINTER_BUTTON_RIGHT;
        break;
    case BTN_MIDDLE:
        b = IRIS_POINTER_BUTTON_MIDDLE;
        break;
    default:
        b = IRIS_POINTER_BUTTON_UNKNOWN;
        break;
    }
    return iris_pointer_button_to_lens(b);
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
/* Forward decl: defined in the primary-selection section, but ptr_button
 * needs it for middle-click paste. */
static void primsel_paste(wp_platform *pl);

static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state) {
    (void)p;
    (void)t;
    wp_platform *pl = data;
    int i = mouse_index(button);
    if (i < 0)
        return;
    /* Keep the serial fresh for pointer-initiated selection operations
     * (middle-click paste below, clipboard ops driven by mouse chords). */
    pl->last_serial = serial;
    bool down = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    if (down && !pl->acc.down[i])
        pl->acc.pressed[i] = true;
    if (!down && pl->acc.down[i])
        pl->acc.released[i] = true;
    pl->acc.down[i] = down;
    if (down && button == BTN_MIDDLE)
        primsel_paste(pl);
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

void iris_paint_mark_static_wayland(void) {
    wp_platform *pl = g_active_pl;
    if (pl)
        pl->paint_static = true;
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

/* Tell the compositor to render `pl->effective_cursor` for the next pointer
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
                                        cursor_shape_for(pl->effective_cursor));
}

/* Effective cursor: an explicit host iris_set_cursor wins; while it is
 * DEFAULT, follow the semantic hint of the lens widget under the pointer
 * (I-beam over text fields, hand over clickable elements, …). Same policy
 * as the Win32/Cocoa backends, evaluated once per frame from the run loop
 * and after every iris_set_cursor. Returns true when the effective cursor
 * changed. */
static bool cursor_follow_hint(wp_platform *pl) {
    iris_cursor eff = pl->host_cursor;
    if (eff == IRIS_CURSOR_DEFAULT && pl->ui) {
        switch (lens_get_cursor_hint(pl->ui)) {
        case LENS_CURSOR_POINTER:
            eff = IRIS_CURSOR_POINTER;
            break;
        case LENS_CURSOR_TEXT:
            eff = IRIS_CURSOR_TEXT;
            break;
        case LENS_CURSOR_RESIZE_EW:
            eff = IRIS_CURSOR_RESIZE_EW;
            break;
        case LENS_CURSOR_RESIZE_NS:
            eff = IRIS_CURSOR_RESIZE_NS;
            break;
        case LENS_CURSOR_DEFAULT:
        default:
            break;
        }
    }
    if (eff == pl->effective_cursor)
        return false;
    pl->effective_cursor = eff;
    return true;
}

/* Public API. Pins a host cursor until reset to IRIS_CURSOR_DEFAULT, which
 * hands control back to the per-frame lens widget hint. No-op when iris was
 * built without cursor-shape protocol support, or before iris_app_run
 * starts. */
IRIS_API void iris_set_cursor(iris_cursor cursor) {
    wp_platform *pl = g_active_pl;
    if (!pl)
        return;
    if (cursor < IRIS_CURSOR_DEFAULT)
        cursor = IRIS_CURSOR_DEFAULT;
    if (cursor == pl->host_cursor)
        return;
    pl->host_cursor = cursor;
    if (cursor_follow_hint(pl))
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
static void touch_motion(void *data, struct wl_touch *t, uint32_t time, int32_t id, wl_fixed_t x,
                         wl_fixed_t y) {
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
static void touch_shape(void *d, struct wl_touch *t, int32_t id, wl_fixed_t maj, wl_fixed_t min) {
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

/* -- Client-side key repeat ----------------------------------------
 * Wayland delivers no auto-repeat events; the compositor only tells us
 * the user's preference (wl_keyboard.repeat_info: rate in chars/sec, delay
 * in ms). We implement repeat ourselves: while a key is held, a timerfd in
 * pump_events' poll set fires after `delay` ms and then every 1000/rate ms,
 * and each expiry re-emits the key as lens_key_event{pressed=true,
 * repeat=true} plus its UTF-8 text (composed sequences are a per-press
 * affair, so repeats bypass compose and emit the key's own utf8). The
 * repeat follows the newest pressed key and is cancelled by its release,
 * by kb_leave (focus loss), by any other key press, or by a modifier
 * change — matching the X11/desktop behaviour users expect. */

static void wp_repeat_cancel(wp_platform *pl) {
    if (!pl->rep_active)
        return;
    pl->rep_active = false;
    if (pl->repeat_fd >= 0) {
        const struct itimerspec disarm = {0};
        (void)timerfd_settime(pl->repeat_fd, 0, &disarm, NULL);
    }
}

/* (Re)arm the repeat timer for pl->rep_key from the compositor's
 * rate/delay. No-op when repeat is disabled (rate 0, or no repeat_info
 * ever received). */
static void wp_repeat_arm(wp_platform *pl) {
    if (pl->repeat_fd < 0 || pl->repeat_rate <= 0) {
        pl->rep_active = false;
        return;
    }
    long long interval_ns = 1000000000LL / pl->repeat_rate;
    long long delay_ns = (long long)(pl->repeat_delay > 0 ? pl->repeat_delay : 0) * 1000000LL;
    if (delay_ns <= 0)
        delay_ns = interval_ns; /* delay 0: first repeat after one interval */
    struct itimerspec its = {
        .it_value = {.tv_sec = delay_ns / 1000000000LL, .tv_nsec = delay_ns % 1000000000LL},
        .it_interval = {.tv_sec = interval_ns / 1000000000LL,
                        .tv_nsec = interval_ns % 1000000000LL},
    };
    pl->rep_active = timerfd_settime(pl->repeat_fd, 0, &its, NULL) == 0;
}

/* The plain-text emission of a key press: boundary-aware append of the
 * key's xkb UTF-8 string into the per-frame accumulator, skipping control
 * characters. Shared by the compose fallback in kb_key and by repeats. */
static void kb_emit_text(wp_platform *pl, xkb_keycode_t code) {
    char buf[8];
    int n = xkb_state_key_get_utf8(pl->xkb_state, code, buf, sizeof buf);
    if (n > 0 && (unsigned char)buf[0] >= 0x20)
        iris_utf8_append(pl->acc.text, sizeof pl->acc.text, buf, (size_t)n);
}

/* One repeat tick: the key event (repeat=true) plus its text. Repeats
 * deliberately bypass the compose state — a composed sequence completes on
 * the original press; the held key repeats its own character. */
static void repeat_emit_one(wp_platform *pl) {
    if (pl->rep_lens_key && pl->acc.key_count < LENS_INPUT_MAX_KEYS)
        pl->acc.keys[pl->acc.key_count++] =
            (lens_key_event){.key = pl->rep_lens_key, .pressed = true, .repeat = true};
    kb_emit_text(pl, pl->rep_key + 8); /* evdev -> xkb */
}

/* -- IM session state ----------------------------------------------
 * The text-input-v3 object is enabled exactly while the window has
 * keyboard focus AND a lens text widget is focused, re-evaluated once per
 * frame after lens_end (im_frame_update). kb_enter/kb_leave only track the
 * window-level focus; the per-frame evaluation owns enable/disable. */

/* Drop every piece of pending composition state: the double-buffered IME
 * batch and the accumulator's preedit. Called when the IM session ends
 * (kb_leave, ti_leave, per-widget blur) so an abandoned composition never
 * renders after focus loss. */
static void im_clear_pending(wp_platform *pl) {
    memset(&pl->ime, 0, sizeof pl->ime);
    pl->acc.preedit[0] = '\0';
    pl->acc.preedit_cursor = 0;
    pl->acc.preedit_sel_lo = pl->acc.preedit_sel_hi = 0;
}

/* End the IM session: disable + commit, drop pending composition state,
 * and reset the surrounding-text memento (the compositor forgets it on
 * disable, so the next enable must re-report even for identical text). */
static void im_deactivate(wp_platform *pl) {
    if (pl->text_input) {
        zwp_text_input_v3_disable(pl->text_input);
        zwp_text_input_v3_commit(pl->text_input);
    }
    pl->im_active = false;
    im_clear_pending(pl);
    free(pl->im_surr);
    pl->im_surr = NULL;
    pl->im_surr_len = 0;
    pl->im_surr_cursor = 0;
}

/* Report the widget's text + caret as the IME surrounding text, but only
 * when it changed since the last report (the memento compare is the shared
 * platform_text helper). Returns true when set_surrounding_text was sent;
 * the caller then folds the commit into its own (enable transition or
 * steady-state update). */
static bool im_report_surrounding(wp_platform *pl, const lens_text_context *ctx) {
    if (!iris_text_memento_update(&pl->im_surr, &pl->im_surr_len, &pl->im_surr_cursor, ctx->utf8,
                                  ctx->len, ctx->cursor))
        return false;
    zwp_text_input_v3_set_surrounding_text(pl->text_input, ctx->utf8, (int32_t)ctx->cursor,
                                           (int32_t)ctx->cursor);
    return true;
}

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
    /* The held key's meaning may have changed under it. */
    wp_repeat_cancel(pl);
}
/* Window-level keyboard focus. kb_enter deliberately does NOT enable the
 * text input: the wl_keyboard enter arrives before any frame has rendered,
 * so which lens text widget (if any) holds focus is unknowable here —
 * im_frame_update's per-frame evaluation is the single owner of
 * enable/disable and picks the state up on the next frame. kb_leave ends
 * the IM session immediately (and cancels key repeat) rather than waiting
 * for a frame, so an abandoned composition can never linger. */
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf,
                     struct wl_array *keys) {
    (void)k;
    (void)s;
    (void)keys;
    (void)surf;
    wp_platform *pl = d;
    pl->kb_focused = true;
}
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *surf) {
    (void)k;
    (void)s;
    (void)surf;
    wp_platform *pl = d;
    pl->kb_focused = false;
    wp_repeat_cancel(pl);
    if (pl->im_active)
        im_deactivate(pl);
    else
        im_clear_pending(pl);
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
    /* The compositor's text-input focus left our surface: drop any pending
     * composition so an abandoned preedit never renders afterwards. */
    im_clear_pending(pl);
}

/* Clamp a text-input-v3 byte offset (-1 = none) into [0, len]. */
static uint32_t clamp_preedit_off(int32_t off, uint32_t len) {
    if (off < 0)
        return 0;
    return (uint32_t)off > len ? len : (uint32_t)off;
}

static void ti_preedit(void *data, struct zwp_text_input_v3 *ti, const char *text,
                       int32_t cursor_begin, int32_t cursor_end) {
    wp_platform *pl = data;
    (void)ti;
    /* boundary-aware copy: a raw byte cap could split a multi-byte sequence
     * at the buffer edge and hand lens invalid UTF-8. */
    iris_utf8_copy(pl->ime.preedit, sizeof pl->ime.preedit, text);
    pl->ime.preedit_cursor_begin = cursor_begin;
    pl->ime.preedit_cursor_end = cursor_end; /* active clause (lens underline) */
    pl->ime.has_preedit = true;
}

static void ti_commit(void *data, struct zwp_text_input_v3 *ti, const char *text) {
    wp_platform *pl = data;
    (void)ti;
    iris_utf8_copy(pl->ime.commit, sizeof pl->ime.commit, text);
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
        /* Boundary-aware append: an over-long commit is clipped on a
         * code-point boundary instead of being dropped wholesale (old
         * behaviour) or split mid-sequence. */
        iris_utf8_append(pl->acc.text, sizeof pl->acc.text, pl->ime.commit, strlen(pl->ime.commit));
        pl->ime.commit[0] = '\0';
    }

    if (pl->ime.has_preedit) {
        if (pl->ime.preedit[0]) {
            iris_utf8_copy(pl->acc.preedit, sizeof pl->acc.preedit, pl->ime.preedit);
            uint32_t plen = (uint32_t)strlen(pl->acc.preedit);
            /* Caret + active clause are byte offsets into the ORIGINAL
             * preedit; clamp both to the truncated copy (a negative offset
             * means "no caret"/"no clause" and collapses to the caret).
             * IMs signal "no active clause" with begin == end == caret,
             * which yields lo == hi and lens renders no clause underline. */
            uint32_t lo = clamp_preedit_off(pl->ime.preedit_cursor_begin, plen);
            uint32_t hi = pl->ime.preedit_cursor_end >= 0
                              ? clamp_preedit_off(pl->ime.preedit_cursor_end, plen)
                              : lo;
            if (hi < lo)
                hi = lo;
            pl->acc.preedit_cursor = lo;
            pl->acc.preedit_sel_lo = lo;
            pl->acc.preedit_sel_hi = hi;
        } else {
            pl->acc.preedit[0] = '\0';
            pl->acc.preedit_cursor = 0;
            pl->acc.preedit_sel_lo = pl->acc.preedit_sel_hi = 0;
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

    /* Keyboard contract (platform_internal.h): both edges are reported, and
     * letters/digits are normalised to the UNSHIFTED key ('a', not 'A';
     * '1', not '!') so Ctrl/Shift chords line up across platforms — shift
     * state travels in lens_input.mods only. Deriving the key id from the
     * level-0 keysym of the key's active layout gives exactly that, and —
     * just as important — press and release always agree on the id even if
     * the modifier state changed in between. */
    xkb_layout_index_t layout = xkb_state_key_get_layout(pl->xkb_state, code);
    const xkb_keysym_t *level0 = NULL;
    int n_level0 = xkb_keymap_key_get_syms_by_level(pl->xkb_keymap, code, layout, 0, &level0);
    xkb_keysym_t sym = (n_level0 > 0) ? level0[0] : xkb_state_key_get_one_sym(pl->xkb_state, code);

    int fk = 0;
    if (pl->acc.key_count < LENS_INPUT_MAX_KEYS) {
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
         * xkb keysyms for these equal their ASCII codepoints; the level-0
         * keysym is the unshifted one ('a', never 'A'). */
        else if (sym >= XKB_KEY_space && sym <= XKB_KEY_asciitilde)
            fk = (int)sym;
        if (fk) {
            /* Physical press/release edges carry repeat=false; repeats are
             * synthesised by the timerfd path (repeat_emit_one) and carry
             * repeat=true. Lens treats a repeat like any other press. */
            pl->acc.keys[pl->acc.key_count++] =
                (lens_key_event){.key = fk, .pressed = pressed, .repeat = false};
        }
    }

    /* Key repeat follows the newest press and dies on its release. The
     * timer arms even for unmappable keys (fk == 0): they can still repeat
     * their text (e.g. a non-ASCII printable). */
    if (pressed) {
        pl->rep_key = key;
        pl->rep_lens_key = fk;
        wp_repeat_arm(pl);
    } else if (pl->rep_active && pl->rep_key == key) {
        wp_repeat_cancel(pl);
    }

    if (pressed) {
        /* Dead-key composition: feed the press's own keysym (shift-aware,
         * unlike the level-0 id above) to the compose state. While a
         * sequence is composing the press produces no text; a completed
         * sequence emits the composed UTF-8 (possibly several characters);
         * a cancelled or non-sequence key falls back to the plain xkb
         * text. Repeats never reach here — repeat_emit_one bypasses
         * compose on purpose. */
        if (pl->compose_state) {
            xkb_keysym_t csym = xkb_state_key_get_one_sym(pl->xkb_state, code);
            if (xkb_compose_state_feed(pl->compose_state, csym) == XKB_COMPOSE_FEED_ACCEPTED) {
                switch (xkb_compose_state_get_status(pl->compose_state)) {
                case XKB_COMPOSE_COMPOSING:
                    return; /* mid-sequence: swallow this press's text */
                case XKB_COMPOSE_COMPOSED: {
                    char buf[64];
                    int n = xkb_compose_state_get_utf8(pl->compose_state, buf, sizeof buf);
                    xkb_compose_state_reset(pl->compose_state);
                    if (n > 0)
                        iris_utf8_append(pl->acc.text, sizeof pl->acc.text, buf, (size_t)n);
                    return;
                }
                case XKB_COMPOSE_CANCELLED:
                    xkb_compose_state_reset(pl->compose_state);
                    break; /* fall through to the plain text */
                case XKB_COMPOSE_NOTHING:
                default:
                    break;
                }
            }
        }
        kb_emit_text(pl, code);
    }
}
static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                         uint32_t latched, uint32_t locked, uint32_t group) {
    (void)k;
    (void)serial;
    wp_platform *pl = data;
    if (!pl->xkb_state)
        return;
    /* A modifier change ends any in-flight repeat: the held key's meaning
     * (and text) just changed under it. */
    wp_repeat_cancel(pl);
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
/* The compositor's repeat preference (wl_keyboard ≥ v4). rate 0 disables
 * repeat entirely; a change re-arms an in-flight repeat with the new
 * timing. Negative values are protocol-hostile; clamp them. */
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)k;
    wp_platform *pl = d;
    pl->repeat_rate = rate > 0 ? rate : 0;
    pl->repeat_delay = delay > 0 ? delay : 0;
    if (!pl->rep_active)
        return;
    if (pl->repeat_rate == 0)
        wp_repeat_cancel(pl);
    else
        wp_repeat_arm(pl);
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

/* Offers announce their MIME types via offer events before the
 * selection/enter event binds them; accumulate into the pending slot and
 * move the record when the offer is bound (ddev_selection / ddev_enter). */
static struct wp_offer_mimes *offer_mime_slot(wp_platform *pl, struct wl_data_offer *off) {
    if (pl->pending_offer_mimes.offer == off)
        return &pl->pending_offer_mimes;
    if (pl->selection_offer_mimes.offer == off)
        return &pl->selection_offer_mimes;
    if (pl->dnd_offer_mimes.offer == off)
        return &pl->dnd_offer_mimes;
    return NULL;
}

static void doffer_offer(void *data, struct wl_data_offer *off, const char *mime) {
    wp_platform *pl = data;
    struct wp_offer_mimes *slot = offer_mime_slot(pl, off);
    if (!slot)
        return;
    if (strcmp(mime, "text/uri-list") == 0)
        slot->uri_list = true;
    else if (strcmp(mime, "text/plain;charset=utf-8") == 0)
        slot->text_utf8 = true;
    else if (strcmp(mime, "text/plain") == 0)
        slot->text_plain = true;
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
    wp_platform *pl = data;
    (void)dev;
    /* A previous offer that was announced but never bound (no selection /
     * enter referenced it) is ours to destroy, or it leaks. */
    if (pl->pending_offer_mimes.offer && pl->pending_offer_mimes.offer != offer)
        wl_data_offer_destroy(pl->pending_offer_mimes.offer);
    pl->pending_offer_mimes = (struct wp_offer_mimes){
        .offer = offer, .uri_list = false, .text_utf8 = false, .text_plain = false};
    wl_data_offer_add_listener(offer, &data_offer_listener, pl);
}
static void ddev_selection(void *data, struct wl_data_device *dev, struct wl_data_offer *offer) {
    wp_platform *pl = data;
    (void)dev;
    if (pl->selection_offer && pl->selection_offer != offer)
        wl_data_offer_destroy(pl->selection_offer);
    pl->selection_offer = offer; /* may be NULL when selection is cleared */
    pl->selection_offer_mimes = (struct wp_offer_mimes){.offer = offer};
    if (offer && pl->pending_offer_mimes.offer == offer) {
        pl->selection_offer_mimes = pl->pending_offer_mimes;
        pl->pending_offer_mimes = (struct wp_offer_mimes){0};
    }
}

/* Strongest readable text MIME on the offer, or NULL when it carries no
 * text we can use (image-only drags, …). uri-list is preferred for file
 * drags; utf-8 text beats legacy text/plain. */
static const char *offer_pick_mime(const struct wp_offer_mimes *m) {
    if (m->uri_list)
        return "text/uri-list";
    if (m->text_utf8)
        return "text/plain;charset=utf-8";
    if (m->text_plain)
        return "text/plain";
    return NULL;
}

/* Read a received offer fd with a hard overall deadline. Returns a malloc'd
 * buffer (caller frees) and its length via *out_len; NULL on error/timeout.
 * Runs on the async reader thread for every path (paste, primary paste,
 * drop) — never on the UI thread (cross-platform.md invariant 4). */
#define WP_DND_READ_TIMEOUT_MS 2000
#define WP_DROP_MAX (1u << 20) /* 1 MiB — matches lens's paste staging cap */
static long long now_ns(void); /* defined with the frame-pacing helpers */

static char *read_offer_fd(int fd, int timeout_ms, size_t *out_len) {
    *out_len = 0;
    char *out = NULL;
    size_t total = 0;
    long long deadline = now_ns() + (long long)timeout_ms * 1000000LL;
    for (;;) {
        long long remain_ms = (deadline - now_ns()) / 1000000LL;
        if (remain_ms <= 0)
            break; /* deadline: a hostile or stuck source cannot hang the UI */
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, (int)(remain_ms > 100 ? 100 : remain_ms));
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue; /* re-check the deadline */
        char chunk[16384];
        ssize_t r = read(fd, chunk, sizeof chunk);
        if (r == 0)
            break; /* EOF */
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (total + (size_t)r > WP_DROP_MAX) {
            fprintf(stderr, "iris: drop/paste payload exceeds %u bytes; truncated\n",
                    (unsigned)WP_DROP_MAX);
            size_t room = WP_DROP_MAX - total;
            if (room) {
                char *grown = realloc(out, WP_DROP_MAX);
                if (!grown) {
                    free(out);
                    return NULL;
                }
                out = grown;
                memcpy(out + total, chunk, room);
                total = WP_DROP_MAX;
            }
            break;
        }
        char *grown = realloc(out, total + (size_t)r);
        if (!grown) {
            free(out);
            return NULL;
        }
        out = grown;
        memcpy(out + total, chunk, (size_t)r);
        total += (size_t)r;
    }
    *out_len = total;
    return out;
}

/* ------------------------------------------------------------------ */
/*  Async offer reads (clipboard paste / primary paste / DND drop)       */
/* ------------------------------------------------------------------ */

/* cross-platform.md invariant 4: the UI thread never blocks reading a
 * selection or drop payload — a stuck or malicious source must not hang
 * the app. Every read (clipboard paste, middle-click primary paste, DND
 * drop) runs on a detached helper thread with a hard deadline, and
 * completion is posted through iris_post_to_main_thread — which also makes
 * the self-paste case correct: the main thread keeps dispatching, so our
 * own data source can answer. */
#define WP_PASTE_TIMEOUT_MS 5000

typedef struct wp_paste_job {
    wp_platform *pl;
    int fd;
    struct wl_data_offer *offer; /* DND drop only: finish+destroy on delivery */
    uint32_t timeout_ms;
    bool strip_crlf; /* strip trailing \\r\\n (uri-list drops) */
} wp_paste_job;

typedef struct wp_paste_result {
    wp_platform *pl;
    char *text; /* NULL when the read failed or timed out */
    size_t len;
    struct wl_data_offer *offer;
} wp_paste_result;

/* Runs on the iris main thread via iris_post_to_main_thread. pl->ui is
 * checked (not just pl): teardown clears it before draining the queue. */
static void paste_deliver(void *user) {
    wp_paste_result *res = user;
    /* A drop's offer stays alive until the read completes so we can tell
     * the source the transfer finished (protocol: finish before destroy).
     * The slot check guards against teardown having destroyed it already. */
    if (res->offer && res->pl->dnd_job_offer == res->offer) {
        wl_data_offer_finish(res->offer);
        wl_data_offer_destroy(res->offer);
        res->pl->dnd_job_offer = NULL;
    }
    if (res->pl->ui && res->text && res->len)
        lens_paste(res->pl->ui, res->text, res->len);
    free(res->text);
    free(res);
}

static void *paste_reader_main(void *arg) {
    wp_paste_job *job = arg;
    size_t len = 0;
    char *text = read_offer_fd(job->fd, (int)job->timeout_ms, &len);
    close(job->fd);
    if (text && job->strip_crlf) {
        /* uri-list payloads are CRLF-terminated; the terminator is not
         * part of the dropped text. */
        while (len && (text[len - 1] == '\n' || text[len - 1] == '\r'))
            text[--len] = '\0';
    }

    wp_paste_result *res = malloc(sizeof *res);
    if (res) {
        res->pl = job->pl;
        res->text = text;
        res->len = len;
        res->offer = job->offer;
        /* -1: the loop is gone (window closed mid-read) — drop. A DND
         * offer in flight is then finished/destroyed by teardown (it owns
         * the dnd_job_offer slot). */
        if (iris_post_to_main_thread(paste_deliver, res) != 0) {
            free(text);
            free(res);
        }
    } else {
        free(text);
    }
    free(job);
    return NULL;
}

/* Spawn the detached reader for an offer pipe's read end. Returns false on
 * failure (fd closed by caller path). */
static bool start_offer_read(wp_platform *pl, int fd, struct wl_data_offer *offer,
                             uint32_t timeout_ms, bool strip_crlf) {
    wp_paste_job *job = malloc(sizeof *job);
    if (!job)
        return false;
    job->pl = pl;
    job->fd = fd;
    job->offer = offer;
    job->timeout_ms = timeout_ms;
    job->strip_crlf = strip_crlf;
    pthread_t th;
    if (pthread_create(&th, NULL, paste_reader_main, job) != 0) {
        free(job);
        return false;
    }
    pthread_detach(th);
    return true;
}

static void ddev_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *su,
                       wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) {
    (void)dev;
    (void)su;
    wp_platform *pl = d;
    pl->dnd_inside = true;
    pl->dnd_pos = (flux_point){wl_fixed_to_double(x), wl_fixed_to_double(y)};
    if (pl->dnd_offer != o) {
        if (pl->dnd_offer)
            wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = o;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){.offer = o};
        if (o && pl->pending_offer_mimes.offer == o) {
            pl->dnd_offer_mimes = pl->pending_offer_mimes;
            pl->pending_offer_mimes = (struct wp_offer_mimes){0};
        }
    }
    if (o) {
        /* Real MIME negotiation: accept only when the drag carries a text
         * type we can read; otherwise leave the offer un-accepted and the
         * compositor shows the not-allowed cursor. */
        const char *mime = offer_pick_mime(&pl->dnd_offer_mimes);
        if (mime) {
            wl_data_offer_accept(o, s, mime);
            wl_data_offer_set_actions(o, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                      WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
        }
    }
}
static void ddev_leave(void *d, struct wl_data_device *dev) {
    (void)dev;
    wp_platform *pl = d;
    pl->dnd_inside = false;
    if (pl->dnd_offer) {
        wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = NULL;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){0};
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
        /* The read runs off the UI thread (invariant 4): hand the offer to
         * the async job slot; delivery (paste + finish + destroy) happens
         * in paste_deliver on the main thread when the read completes. */
        const char *mime = offer_pick_mime(&pl->dnd_offer_mimes);
        bool started = false;
        if (mime && pl->ui && !pl->dnd_job_offer) {
            int fds[2];
            if (pipe(fds) == 0) {
                wl_data_offer_receive(pl->dnd_offer, mime, fds[1]);
                close(fds[1]);
                wl_display_flush(pl->display);
                started = start_offer_read(pl, fds[0], pl->dnd_offer, WP_DND_READ_TIMEOUT_MS,
                                           true /* uri-list CRLF strip */);
                if (!started)
                    close(fds[0]);
                else
                    pl->dnd_job_offer = pl->dnd_offer;
            }
        }
        if (!started) {
            /* No usable payload or no reader: close the DND session now. */
            wl_data_offer_finish(pl->dnd_offer);
            wl_data_offer_destroy(pl->dnd_offer);
        }
        pl->dnd_offer = NULL;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){0};
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

/* ------------------------------------------------------------------ */
/*  Primary selection (zwp_primary_selection_unstable_v1)                */
/* ------------------------------------------------------------------ */

/* The X11-style middle-click clipboard, mirrored from clip_set_text and
 * read on middle-button press (primsel_paste, called from ptr_button).
 * Same offer/MIME machinery as the clipboard, with its own pending/bound
 * slots (primary offers are a different object type, tracked through the
 * shared wp_offer_mimes identity cookie). */

static struct wp_offer_mimes *primsel_mime_slot(wp_platform *pl,
                                                struct zwp_primary_selection_offer_v1 *off) {
    if ((struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer == off)
        return &pl->primsel_pending_mimes;
    if ((struct zwp_primary_selection_offer_v1 *)pl->primsel_offer_mimes.offer == off)
        return &pl->primsel_offer_mimes;
    return NULL;
}

static void poffer_offer(void *data, struct zwp_primary_selection_offer_v1 *off, const char *mime) {
    wp_platform *pl = data;
    struct wp_offer_mimes *slot = primsel_mime_slot(pl, off);
    if (!slot)
        return;
    if (strcmp(mime, "text/plain;charset=utf-8") == 0)
        slot->text_utf8 = true;
    else if (strcmp(mime, "text/plain") == 0)
        slot->text_plain = true;
}
static const struct zwp_primary_selection_offer_v1_listener primary_offer_listener = {
    .offer = poffer_offer,
};

static void pdev_data_offer(void *data, struct zwp_primary_selection_device_v1 *dev,
                            struct zwp_primary_selection_offer_v1 *offer) {
    wp_platform *pl = data;
    (void)dev;
    /* A previous offer that was announced but never bound is ours to
     * destroy, or it leaks. */
    if (pl->primsel_pending_mimes.offer &&
        (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer != offer)
        zwp_primary_selection_offer_v1_destroy(
            (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer);
    pl->primsel_pending_mimes = (struct wp_offer_mimes){.offer = (struct wl_data_offer *)offer};
    zwp_primary_selection_offer_v1_add_listener(offer, &primary_offer_listener, pl);
}
static void pdev_selection(void *data, struct zwp_primary_selection_device_v1 *dev,
                           struct zwp_primary_selection_offer_v1 *offer) {
    wp_platform *pl = data;
    (void)dev;
    if (pl->primsel_offer && pl->primsel_offer != offer)
        zwp_primary_selection_offer_v1_destroy(pl->primsel_offer);
    pl->primsel_offer = offer; /* may be NULL when the selection is cleared */
    pl->primsel_offer_mimes = (struct wp_offer_mimes){.offer = (struct wl_data_offer *)offer};
    if (offer &&
        (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer == offer) {
        pl->primsel_offer_mimes = pl->primsel_pending_mimes;
        pl->primsel_pending_mimes = (struct wp_offer_mimes){0};
    }
}
static const struct zwp_primary_selection_device_v1_listener primary_device_listener = {
    .data_offer = pdev_data_offer,
    .selection = pdev_selection,
};

/* Our outgoing primary selection: serves the same buffer as the clipboard
 * source (clip_set_text mirrors every copy onto both). */
static void psource_send(void *data, struct zwp_primary_selection_source_v1 *src, const char *mime,
                         int32_t fd) {
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
static void psource_cancelled(void *data, struct zwp_primary_selection_source_v1 *src) {
    wp_platform *pl = data;
    zwp_primary_selection_source_v1_destroy(src);
    if (pl->primsel_source == src)
        pl->primsel_source = NULL;
}
static const struct zwp_primary_selection_source_v1_listener primary_source_listener = {
    .send = psource_send,
    .cancelled = psource_cancelled,
};

static void wp_maybe_create_primsel_device(wp_platform *pl) {
    if (pl->primsel_device || !pl->primsel_mgr || !pl->seat)
        return;
    pl->primsel_device =
        zwp_primary_selection_device_manager_v1_get_device(pl->primsel_mgr, pl->seat);
    zwp_primary_selection_device_v1_add_listener(pl->primsel_device, &primary_device_listener, pl);
}

/* Middle-click paste: request the primary selection's text and deliver it
 * through the same async paste channel as the clipboard (lens_paste routes
 * it to the focused text widget, so a middle click anywhere pastes into
 * whatever holds focus — the standard desktop behaviour). */
static void primsel_paste(wp_platform *pl) {
    if (!pl->primsel_offer || !pl->ui)
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;
    /* Text family only (no uri-list): utf-8 beats legacy text/plain; fall
     * back to utf-8 when the offer predates our MIME tracking. */
    const struct wp_offer_mimes *m = &pl->primsel_offer_mimes;
    const char *mime = ((struct zwp_primary_selection_offer_v1 *)m->offer == pl->primsel_offer &&
                        m->text_plain && !m->text_utf8)
                           ? "text/plain"
                           : "text/plain;charset=utf-8";
    zwp_primary_selection_offer_v1_receive(pl->primsel_offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(pl->display);

    if (!start_offer_read(pl, fds[0], NULL, WP_PASTE_TIMEOUT_MS, false))
        close(fds[0]);
}

/* lens_clipboard.set_text — advertise `utf8` as the system selection, and
 * mirror it onto the primary selection (the X11-style middle-click
 * clipboard) so explicit copies are also middle-pasteable. */
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

    if (pl->primsel_mgr && pl->primsel_device) {
        pl->primsel_source = zwp_primary_selection_device_manager_v1_create_source(pl->primsel_mgr);
        zwp_primary_selection_source_v1_add_listener(pl->primsel_source, &primary_source_listener,
                                                     pl);
        zwp_primary_selection_source_v1_offer(pl->primsel_source, "text/plain;charset=utf-8");
        zwp_primary_selection_source_v1_offer(pl->primsel_source, "text/plain");
        zwp_primary_selection_device_v1_set_selection(pl->primsel_device, pl->primsel_source,
                                                      pl->last_serial);
    }
}

/* lens_clipboard.request_text — the lens contract is asynchronous
 * (cross-platform.md invariant 4): the answer MUST arrive later via
 * lens_paste, never synchronously inside the request. The pipe is drained
 * on a detached helper thread with a hard deadline (start_offer_read), and
 * completion is posted through iris_post_to_main_thread — which also makes
 * the self-paste case correct: the main thread keeps dispatching, so our
 * own data source can answer. */
static void clip_request_text(void *user) {
    wp_platform *pl = user;
    if (!pl->selection_offer || !pl->ui)
        return;

    int fds[2];
    if (pipe(fds) != 0)
        return;
    /* Negotiate against the offer's advertised types; fall back to the
     * UTF-8 type when the offer predates our MIME tracking (harmless: the
     * source simply refuses a type it does not have). */
    const struct wp_offer_mimes *m = &pl->selection_offer_mimes;
    const char *mime = (m->offer == pl->selection_offer && m->text_plain && !m->text_utf8)
                           ? "text/plain"
                           : "text/plain;charset=utf-8";
    wl_data_offer_receive(pl->selection_offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(pl->display);

    if (!start_offer_read(pl, fds[0], NULL, WP_PASTE_TIMEOUT_MS, false))
        close(fds[0]);
}

/* ------------------------------------------------------------------ */
/*  Seat                                                               */
/* ------------------------------------------------------------------ */

/* seat/pointer/keyboard/touch gained a _release request in version 3 (the
 * destructor for objects created from a seat); data_device in version 2.
 * Use _release when the bound version supports it — plain _destroy races
 * the compositor's own cleanup on capability loss / hot-unplug. */
static void wp_pointer_release(struct wl_pointer *p) {
    if (wl_pointer_get_version(p) >= WL_POINTER_RELEASE_SINCE_VERSION)
        wl_pointer_release(p);
    else
        wl_pointer_destroy(p);
}
static void wp_keyboard_release(struct wl_keyboard *k) {
    if (wl_keyboard_get_version(k) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
        wl_keyboard_release(k);
    else
        wl_keyboard_destroy(k);
}
static void wp_touch_release(struct wl_touch *t) {
    if (wl_touch_get_version(t) >= WL_TOUCH_RELEASE_SINCE_VERSION)
        wl_touch_release(t);
    else
        wl_touch_destroy(t);
}
static void wp_seat_release(struct wl_seat *s) {
    if (wl_seat_get_version(s) >= WL_SEAT_RELEASE_SINCE_VERSION)
        wl_seat_release(s);
    else
        wl_seat_destroy(s);
}
static void wp_data_device_release(struct wl_data_device *d) {
    if (wl_data_device_get_version(d) >= WL_DATA_DEVICE_RELEASE_SINCE_VERSION)
        wl_data_device_release(d);
    else
        wl_data_device_destroy(d);
}

/* The text input is per-seat; create it lazily so a seat arriving after
 * startup (or returning after a hot-unplug) gets one too. */
static void wp_maybe_create_text_input(wp_platform *pl) {
    if (pl->text_input || !pl->text_input_mgr || !pl->seat)
        return;
    pl->text_input = zwp_text_input_manager_v3_get_text_input(pl->text_input_mgr, pl->seat);
    if (pl->text_input)
        zwp_text_input_v3_add_listener(pl->text_input, &text_input_listener, pl);
}

/* Tablet bridge (tablet_wayland.c): pen events land in the same
 * accumulator the mouse uses — motion → cursor, tip → LEFT, barrel →
 * RIGHT/MIDDLE — so every existing widget works with a pen unchanged. */
static void tablet_motion(void *user, double x, double y) {
    wp_platform *pl = user;
    pl->acc.cx = x;
    pl->acc.cy = y;
}
static void tablet_button(void *user, int i, bool down) {
    wp_platform *pl = user;
    if (down && !pl->acc.down[i])
        pl->acc.pressed[i] = true;
    if (!down && pl->acc.down[i])
        pl->acc.released[i] = true;
    pl->acc.down[i] = down;
}
static void tablet_serial(void *user, uint32_t serial) {
    wp_platform *pl = user;
    pl->last_serial = serial;
}
static const iris_tablet_host tablet_host_bridge = {
    /* The platform is a stack object inside iris_app_run_wayland; the
     * registry (and thus tablet globals) can only arrive during that
     * call, where this pointer is set before dispatch starts. */
    .user = NULL,
    .motion = tablet_motion,
    .button = tablet_button,
    .serial = tablet_serial,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    wp_platform *pl = data;
    bool has_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
    bool has_touch = caps & WL_SEAT_CAPABILITY_TOUCH;

    if (has_ptr && !pl->pointer) {
        pl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(pl->pointer, &pointer_listener, pl);
    } else if (!has_ptr && pl->pointer) {
        /* The cursor-shape device is per-pointer: it dies with it. */
        if (pl->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(pl->cursor_shape_device);
            pl->cursor_shape_device = NULL;
        }
        pl->cursor_inside = false;
        wp_pointer_release(pl->pointer);
        pl->pointer = NULL;
    }
    if (has_kb && !pl->keyboard) {
        pl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(pl->keyboard, &keyboard_listener, pl);
    } else if (!has_kb && pl->keyboard) {
        /* Keyboard gone: end the IM session and any key repeat first, so
         * no composition or held key outlives the device. */
        wp_repeat_cancel(pl);
        if (pl->im_active)
            im_deactivate(pl);
        else
            im_clear_pending(pl);
        pl->kb_focused = false;
        wp_keyboard_release(pl->keyboard);
        pl->keyboard = NULL;
    }
    if (has_touch && !pl->touch) {
        pl->touch = wl_seat_get_touch(seat);
        if (pl->touch)
            wl_touch_add_listener(pl->touch, &touch_listener, pl);
    } else if (!has_touch && pl->touch) {
        wp_touch_release(pl->touch);
        pl->touch = NULL;
        g_touch_active_id = -1;
    }

    wp_maybe_create_data_device(pl);
    wp_maybe_create_primsel_device(pl);
    wp_maybe_create_text_input(pl);
}

/* Destroy every object created from pl->seat (capability objects, the data
 * and primary-selection devices, the text input) and then the seat itself.
 * Used when the seat's registry global goes away (hot-unplug). In-flight
 * async drop reads keep their offer via the dnd_job_offer slot, finished
 * on delivery or at app teardown. */
static void seat_teardown(wp_platform *pl) {
    wp_repeat_cancel(pl);
    if (pl->im_active)
        im_deactivate(pl);
    else
        im_clear_pending(pl);
    pl->kb_focused = false;

    if (pl->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(pl->cursor_shape_device);
        pl->cursor_shape_device = NULL;
    }
    pl->cursor_inside = false;
    if (pl->pointer) {
        wp_pointer_release(pl->pointer);
        pl->pointer = NULL;
    }
    if (pl->keyboard) {
        wp_keyboard_release(pl->keyboard);
        pl->keyboard = NULL;
    }
    if (pl->touch) {
        wp_touch_release(pl->touch);
        pl->touch = NULL;
        g_touch_active_id = -1;
    }
    if (pl->text_input) {
        zwp_text_input_v3_destroy(pl->text_input);
        pl->text_input = NULL;
        pl->text_input_surface = NULL;
    }
    if (pl->selection_offer) {
        wl_data_offer_destroy(pl->selection_offer);
        pl->selection_offer = NULL;
        pl->selection_offer_mimes = (struct wp_offer_mimes){0};
    }
    if (pl->dnd_offer) {
        wl_data_offer_destroy(pl->dnd_offer);
        pl->dnd_offer = NULL;
        pl->dnd_offer_mimes = (struct wp_offer_mimes){0};
    }
    if (pl->pending_offer_mimes.offer) {
        wl_data_offer_destroy(pl->pending_offer_mimes.offer);
        pl->pending_offer_mimes = (struct wp_offer_mimes){0};
    }
    if (pl->copy_source) {
        wl_data_source_destroy(pl->copy_source);
        pl->copy_source = NULL;
    }
    if (pl->data_device) {
        wp_data_device_release(pl->data_device);
        pl->data_device = NULL;
    }
    if (pl->primsel_offer) {
        zwp_primary_selection_offer_v1_destroy(pl->primsel_offer);
        pl->primsel_offer = NULL;
        pl->primsel_offer_mimes = (struct wp_offer_mimes){0};
    }
    if (pl->primsel_pending_mimes.offer) {
        zwp_primary_selection_offer_v1_destroy(
            (struct zwp_primary_selection_offer_v1 *)pl->primsel_pending_mimes.offer);
        pl->primsel_pending_mimes = (struct wp_offer_mimes){0};
    }
    if (pl->primsel_source) {
        zwp_primary_selection_source_v1_destroy(pl->primsel_source);
        pl->primsel_source = NULL;
    }
    if (pl->primsel_device) {
        zwp_primary_selection_device_v1_destroy(pl->primsel_device);
        pl->primsel_device = NULL;
    }
    if (pl->seat) {
        wp_seat_release(pl->seat);
        pl->seat = NULL;
    }
    pl->seat_name = 0;
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
 * time; the listener folds the fraction into the integer buffer scale. */
static void frac_scale_preferred(void *data, struct wp_fractional_scale_v1 *frac, uint32_t scale) {
    (void)frac;
    wp_platform *pl = data;
    /* scale is in 1/120 steps (so 120 = 1.0, 180 = 1.5, …). Round to the
     * nearest integer buffer scale so the existing integer-scale pipeline
     * keeps working — lens is told the integer buffer scale only, and true
     * fractional content scaling (1.25x etc. into lens) is a follow-on. */
    float f = (float)scale / 120.0f;
    int b = (int)(f + 0.5f);
    if (b < 1)
        b = 1;
    pl->pending_scale = b;
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
        pl->seat_name = name;
        wl_seat_add_listener(pl->seat, &seat_listener, pl);
        iris_wayland__tablet_attach_seat(pl->seat);
    } else if (strcmp(iface, zwp_tablet_manager_v2_interface.name) == 0) {
        /* Pen input (see tablet_wayland.c); no tablet → inert no-ops. */
        iris_wayland__tablet_bind_manager(reg, name, version, &tablet_host_bridge);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (pl->n_outputs < WP_MAX_OUTPUTS) {
            /* version 2 introduces .scale; ask for it if available. */
            uint32_t v = version < 2 ? version : 2;
            wp_output *out = &pl->outputs[pl->n_outputs++];
            out->wl = wl_registry_bind(reg, name, &wl_output_interface, v);
            out->name = name;
            out->scale = 1;
            out->entered = false;
            wl_output_add_listener(out->wl, &output_listener, pl);
        }
    } else if (strcmp(iface, wl_data_device_manager_interface.name) == 0) {
        pl->data_device_mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                                               version < 3 ? version : 3);
        wp_maybe_create_data_device(pl);
    } else if (strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name) == 0) {
        /* primary-selection-unstable-v1: the X11-style middle-click
         * clipboard (mirror of clip_set_text; read on middle press). */
        pl->primsel_mgr =
            wl_registry_bind(reg, name, &zwp_primary_selection_device_manager_v1_interface, 1);
        wp_maybe_create_primsel_device(pl);
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
    } else if (strcmp(iface, zxdg_exporter_v2_interface.name) == 0) {
        /* xdg-foreign-unstable-v2: exports a window handle for portal
         * dialogs (file_dialog_portal.c passes it as parent_window). */
        pl->foreign_exporter = wl_registry_bind(reg, name, &zxdg_exporter_v2_interface, 1);
    }
}

/* Global removal: destroy the proxies we bound for the departing global.
 * Outputs are hot-pluggable, so the slot is recycled (swap with the last)
 * for the array to keep accepting new outputs past WP_MAX_OUTPUTS
 * bind/unbind cycles; the seat tears down every object created from it. */
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)r;
    wp_platform *pl = d;
    for (int i = 0; i < pl->n_outputs; i++) {
        if (pl->outputs[i].name == name) {
            bool was_entered = pl->outputs[i].entered;
            wl_output_destroy(pl->outputs[i].wl);
            pl->outputs[i] = pl->outputs[--pl->n_outputs];
            if (was_entered)
                wp_recompute_scale(pl);
            return;
        }
    }
    if (pl->seat && name == pl->seat_name)
        seat_teardown(pl);
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

/* xdg-foreign-unstable-v2: the compositor answers an export with a handle
 * string ("<uuid>"), which portal dialogs use as parent_window so the
 * picker stays modal to our window. */
static void foreign_exported_handle(void *data, struct zxdg_exported_v2 *e, const char *handle) {
    wp_platform *pl = data;
    (void)e;
    snprintf(pl->foreign_handle, sizeof pl->foreign_handle, "%s", handle);
}
static const struct zxdg_exported_v2_listener foreign_exported_listener = {
    .handle = foreign_exported_handle,
};

/* Internal seam for file_dialog_portal.c (hidden visibility): while a
 * modal portal dialog waits for its D-Bus answer, the wait loop must keep
 * dispatching our display or xdg_wm_base pings go unanswered and the
 * compositor marks the window unresponsive. NULL when no app is running. */
struct wl_display *iris_wayland__display(void) {
    return g_active_pl ? g_active_pl->display : NULL;
}

/* The exported xdg-foreign handle for the active window ("wayland:<handle>"
 * is composed by the caller), or NULL when unavailable. */
const char *iris_wayland__foreign_handle(void) {
    return (g_active_pl && g_active_pl->foreign_handle[0]) ? g_active_pl->foreign_handle : NULL;
}

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
    in->preedit_sel_lo = pl->acc.preedit_sel_lo;
    in->preedit_sel_hi = pl->acc.preedit_sel_hi;
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
    iris_wayland__tablet_fill_input(in);

    /* clear per-frame edges; keep level state (down/cursor/mods) */
    for (int i = 0; i < LENS_MOUSE_COUNT; i++)
        pl->acc.pressed[i] = pl->acc.released[i] = false;
    pl->acc.scroll_x = pl->acc.scroll_y = 0.0;
    pl->acc.key_count = 0;
    pl->acc.text[0] = '\0';
    pl->ime.delete_before = pl->ime.delete_after = 0;
    pl->ime.has_delete = false;
}

/* Diagnostics go to stderr, never stdout: a consumer's stdout may be an
 * IPC wire (see flux_console_logger). */
static void log_raw(const lens_input *in) {
    static const char *names[LENS_MOUSE_COUNT] = {"left", "right", "middle"};
    for (int b = 0; b < LENS_MOUSE_COUNT; b++) {
        if (in->mouse_pressed[b])
            fprintf(stderr, "[raw] mouse press   %s\n", names[b]);
        if (in->mouse_released[b])
            fprintf(stderr, "[raw] mouse release %s\n", names[b]);
    }
    if (in->scroll_x != 0.0f || in->scroll_y != 0.0f)
        fprintf(stderr, "[raw] scroll dx=%.2f dy=%.2f\n", in->scroll_x, in->scroll_y);
    for (uint32_t k = 0; k < in->key_count; k++)
        fprintf(stderr, "[raw] key %d %s%s\n", in->keys[k].key,
                in->keys[k].pressed ? "down" : "up  ", in->keys[k].repeat ? " (repeat)" : "");
}

/* ------------------------------------------------------------------ */
/*  Pump Wayland events without blocking the render loop               */
/* ------------------------------------------------------------------ */

/* Kick for the cross-thread wakeup seam (platform_wakeup.h): called from
 * any thread; writes to the eventfd in pump_events' poll set so the loop
 * wakes and drains the callback queue on this (the main) thread. Never
 * blocks: the eventfd is non-blocking, and an EAGAIN just means a kick is
 * already pending. */
static void wp_wakeup_kick(void *user) {
    wp_platform *pl = user;
    uint64_t one = 1;
    (void)!write(pl->wakeup_fd, &one, sizeof one);
}

static bool pump_events(wp_platform *pl, int timeout_ms) {
    struct wl_display *d = pl->display;
    while (wl_display_prepare_read(d) != 0)
        wl_display_dispatch_pending(d);
    wl_display_flush(d);

    /* Poll the Wayland display fd, the wakeup eventfd, the key-repeat
     * timerfd, and (if active) the AT-SPI bus fd together so we wake up on
     * any. `timeout_ms` is the frame-pacing budget: a non-blocking
     * (vsync=false) present leaves the render loop with no point that
     * sleeps, so we block here for up to a frame's worth of time. poll()
     * returns the instant input (or a wakeup / a11y event) arrives —
     * keeping latency low — and otherwise wakes on the deadline so
     * time-based UI (caret blink, lens animations) keeps ticking even with
     * no input. Passing 0 keeps the old non-blocking behaviour. Returns
     * true when any fd was signalled (as opposed to a timeout), so the
     * frame loop can tell an event wake from a deadline expiry. */
    struct pollfd pfds[4];
    int n = 1;
    int wakeup_idx = -1, a11y_idx = -1, repeat_idx = -1;
    pfds[0] = (struct pollfd){.fd = wl_display_get_fd(d), .events = POLLIN};
    if (pl->wakeup_fd >= 0) {
        wakeup_idx = n;
        pfds[n] = (struct pollfd){.fd = pl->wakeup_fd, .events = POLLIN};
        n++;
    }
    if (pl->repeat_fd >= 0) {
        repeat_idx = n;
        pfds[n] = (struct pollfd){.fd = pl->repeat_fd, .events = POLLIN};
        n++;
    }
    if (pl->a11y_fd >= 0) {
        /* AT-SPI is an sd-bus socket — poll its requested mask, not a
         * bare POLLIN: the socket is level-triggered and can read-ready at
         * the kernel with no sd-bus work pending, which would spin the
         * loop and defeat the frame-pacing sleep above. */
        short ev = iris_a11y__poll_events();
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
        /* Cross-thread wakeup: a subsystem (colour-scheme watcher) posted
         * a callback from its own thread. Drain the eventfd counter, then
         * run the queued callbacks on this thread. */
        if (wakeup_idx >= 0 && pfds[wakeup_idx].revents & POLLIN) {
            uint64_t count;
            while (read(pl->wakeup_fd, &count, sizeof count) == sizeof count) {
            }
            iris_platform_wakeup_drain();
        }
        /* Key repeat tick. Each expiry is one repeated key, capped so a
         * long scheduling stall can't flood the frame with a huge backlog
         * (the coalescing bound desktop toolkits apply too). */
        if (repeat_idx >= 0 && pfds[repeat_idx].revents & POLLIN) {
            uint64_t expirations = 0;
            if (read(pl->repeat_fd, &expirations, sizeof expirations) ==
                    (ssize_t)sizeof expirations &&
                pl->rep_active && pl->xkb_state) {
                if (expirations > 8)
                    expirations = 8;
                for (uint64_t i = 0; i < expirations; i++)
                    repeat_emit_one(pl);
            }
        }
        /* Any signalled event (POLLIN / POLLHUP / POLLERR) — let sd-bus
         * process it so an error/hangup is consumed rather than re-firing. */
        if (a11y_idx >= 0 && pfds[a11y_idx].revents)
            iris_a11y__pump();
    } else {
        wl_display_cancel_read(d);
    }

    wl_display_dispatch_pending(d);
    return pr > 0;
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
/*  Per-frame IM evaluation (text-input-v3)                              */
/* ------------------------------------------------------------------ */

/* Once per frame after lens_end: keep the text input enabled exactly while
 * the window has keyboard focus AND a lens text widget is focused, report
 * the widget's surrounding text when it changes, and position the IME
 * candidate window at the caret.
 *
 * Why per-frame: the wl_keyboard enter arrives before any frame has
 * rendered, so at enter time we cannot know whether a text widget holds
 * lens focus; and lens-side focus changes (Tab traversal, click) never
 * produce Wayland events at all. lens_text_context_get() is the one
 * authoritative signal, refreshed by the focused text widget every frame,
 * so this evaluation owns enable/disable outright — kb_enter/kb_leave only
 * track the window-level focus that gates it. Transitions alone issue
 * protocol requests; steady state sends nothing (surrounding text goes out
 * only on content/cursor change, via the memento compare). */
static void im_frame_update(wp_platform *pl) {
    if (!pl->text_input || !pl->ui)
        return;

    lens_text_context ctx = lens_text_context_get(pl->ui);
    bool want = pl->kb_focused && ctx.utf8 != NULL;
    if (want != pl->im_active) {
        if (want) {
            zwp_text_input_v3_enable(pl->text_input);
            zwp_text_input_v3_set_content_type(pl->text_input,
                                               ctx.multiline
                                                   ? ZWP_TEXT_INPUT_V3_CONTENT_HINT_MULTILINE
                                                   : ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                               ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
            im_report_surrounding(pl, &ctx); /* fresh session: always reports */
            zwp_text_input_v3_commit(pl->text_input);
            pl->im_active = true;
        } else {
            im_deactivate(pl);
        }
    } else if (want) {
        /* Steady state: report surrounding text only when it changed. */
        if (im_report_surrounding(pl, &ctx))
            zwp_text_input_v3_commit(pl->text_input);
    }

    /* Candidate window follows the caret. */
    if (pl->im_active && pl->text_input_surface) {
        flux_rect caret = lens_caret_rect(pl->ui);
        if (caret.w > 0.0f) {
            zwp_text_input_v3_set_cursor_rectangle(pl->text_input, (int32_t)caret.x,
                                                   (int32_t)caret.y, (int32_t)caret.w,
                                                   (int32_t)caret.h);
            zwp_text_input_v3_commit(pl->text_input);
        }
    }
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
    bool host_started = false;
    int rc = 1; /* pessimistic; set to 0 only on success */

    wp_platform pl = {
        .running = true,
        .width = cfg->width > 0 ? cfg->width : 960,
        .height = cfg->height > 0 ? cfg->height : 720,
        .buffer_scale = 1,
        .pending_scale = 1,
        .host_cursor = IRIS_CURSOR_DEFAULT,
        .effective_cursor = IRIS_CURSOR_DEFAULT,
        .a11y_fd = -1, /* so the cleanup guards are correct even if
                        * we fail before the bridge is started */
        .wakeup_fd = -1,
        .repeat_fd = -1,
    };

    /* Publish `pl` as the active app instance so the context-free
     * iris_set_cursor() can reach it. Cleared on the way out (success
     * or fail). */
    g_active_pl = &pl;
    /* Tablet bridge needs the (stack) platform for its host callbacks. */
    *(wp_platform **)&tablet_host_bridge.user = &pl;

    /* --- Wayland connection + globals ---------------------------- */
    pl.display = wl_display_connect(NULL);
    if (!pl.display) {
        fprintf(stderr, "no Wayland display (is a compositor running?)\n");
        return 1;
    }

    /* Cross-thread wakeup seam: an eventfd in pump_events' poll set, plus
     * a kick that writes to it. Must exist before the theme watcher starts
     * (its pump thread posts through this seam). Created only after the
     * early return above so that path cannot leak the fd or leave a kick
     * registered against stack memory. */
    pl.wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (pl.wakeup_fd >= 0)
        iris_platform_wakeup_set_kick(wp_wakeup_kick, &pl);

    pl.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!pl.xkb_ctx) {
        fprintf(stderr, "xkb_context_new failed\n");
        goto fail;
    }

    /* Dead-key compose table for the session locale. xkbcommon idiom: the
     * process locale comes from setlocale(LC_CTYPE, ""); XKB_DEFAULT_LOCALE
     * overrides it (test harnesses and tools set it to force a table). No
     * table for the locale → compose stays NULL and dead keys fall through
     * as ordinary keysyms. */
    setlocale(LC_CTYPE, "");
    const char *locale = getenv("XKB_DEFAULT_LOCALE");
    if (!locale || !locale[0])
        locale = setlocale(LC_CTYPE, NULL);
    if (locale && locale[0]) {
        pl.compose_table =
            xkb_compose_table_new_from_locale(pl.xkb_ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (pl.compose_table)
            pl.compose_state = xkb_compose_state_new(pl.compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    }

    /* Key-repeat timer: armed per held key from the compositor's
     * repeat_info; pump_events polls it alongside the display fd. */
    pl.repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

    pl.registry = wl_display_get_registry(pl.display);
    wl_registry_add_listener(pl.registry, &registry_listener, &pl);
    wl_display_roundtrip(pl.display); /* bind globals */
    wl_display_roundtrip(pl.display); /* seat caps -> pointer/keyboard */

    wp_maybe_create_text_input(&pl);

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
    xdg_toplevel_set_app_id(pl.toplevel, cfg->app_id ? cfg->app_id : "ai.opencode.iris");

    /* Bind a fractional-scale object if the compositor exposes the global;
     * it carries the precise 1/120-step scale used on HiDPI mixed-DPI
     * desktops (Windows portability scenarios, fractional laptop scales). */
    if (pl.fractional_scale_mgr) {
        pl.fractional_scale_obj = wp_fractional_scale_manager_v1_get_fractional_scale(
            pl.fractional_scale_mgr, pl.surface);
        if (pl.fractional_scale_obj)
            wp_fractional_scale_v1_add_listener(pl.fractional_scale_obj, &fractional_scale_listener,
                                                &pl);
    }

    /* Ask the compositor for a server-side title bar when it can. */
    if (pl.deco_mgr) {
        pl.deco = zxdg_decoration_manager_v1_get_toplevel_decoration(pl.deco_mgr, pl.toplevel);
        zxdg_toplevel_decoration_v1_set_mode(pl.deco, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    } else {
        fprintf(stderr, "note: compositor offers no server-side decorations "
                        "(no title bar)\n");
    }

    /* Export a window handle for portal dialogs (parent_window). The handle
     * event arrives on the initial roundtrips below. */
    if (pl.foreign_exporter) {
        pl.foreign_exported = zxdg_exporter_v2_export_toplevel(pl.foreign_exporter, pl.surface);
        if (pl.foreign_exported)
            zxdg_exported_v2_add_listener(pl.foreign_exported, &foreign_exported_listener, &pl);
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

    if (cfg->start && !cfg->start(ui, device, cfg->user)) {
        fprintf(stderr, "iris host start callback failed\n");
        goto fail;
    }
    host_started = true;

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
     * scroll / drag stay smooth. When nothing is happening we go further than
     * a low idle rate: lens_frame_needs_repaint() tells us whether the frame
     * just built would change a single pixel, and if not we skip the whole
     * acquire/paint/present cycle; and when nothing time-driven is pending
     * (no animation, no focused caret) we stop scheduling frames entirely and
     * sleep in poll() until the next input / wakeup / a11y event. A focused
     * text field keeps a low-frequency deadline alive for the caret blink.
     *
     * Hosts with a paint callback (cfg->paint) opt out of all of this: their
     * content is opaque to lens, so they keep the old always-render pacing
     * (~60 Hz after input, ~4 Hz idle). */
    const long long ACTIVE_PERIOD_NS = 16666667LL; /* ~60 Hz when active    */
    const long long IDLE_PERIOD_NS = 250000000LL;  /* ~4 Hz when idle: just  */
                                                   /* enough for the caret   */
                                                   /* blink (500ms period).  */
    const long long INPUT_GRACE_NS = 400000000LL;  /* stay fast 400ms after  */

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    int frame_no = 0;

    long long next_deadline = now_ns();
    bool frame_scheduled = true; /* the first frame must always paint */
    /* Sticky until a frame containing the latest lens build is actually
     * presented. Layout/draw hashes compare consecutive builds; without this
     * host-side latch an acquire timeout could let the next identical build
     * look clean and permanently skip content that never reached screen. */
    bool surface_needs_paint = true;
    long long last_input_ns = next_deadline;
    long long last_render_ns = next_deadline - ACTIVE_PERIOD_NS;
    double prev_cx = pl.acc.cx, prev_cy = pl.acc.cy;

    pl.ui = ui;

    /* Live colour-scheme watching: only when not forcing dark. The watcher
     * pumps the portal on its own thread and posts through the wakeup seam;
     * the callback runs on this thread (via iris_platform_wakeup_drain in
     * pump_events) and updates the lens theme in place — the next frame
     * renders with the new palette. -1 means the feature is unavailable
     * (no libsystemd at build time, or portal unreachable) — silently
     * degrade to the startup-only query. The backend uses its reserved
     * internal slot (theme_watch_internal.h), never the host's public
     * iris_color_scheme_watch registration. */
    pl.theme_watching = false;
    pl.a11y_fd = -1;
    if (!cfg->dark)
        pl.theme_watching = (iris_theme__watch_backend(wp_on_color_scheme_changed, &pl) == 0);

    /* AT-SPI bridge: register the app on the a11y session bus so screen
     * readers (orca, etc.) can read the widget tree. Fail-soft: if the
     * bridge is unavailable we silently skip — the app still runs. Its
     * bus fd joins the pump_events poll set (internal integration point,
     * src/a11y_internal.h), so AT-SPI method calls are answered on this
     * thread. */
    if (iris_a11y_init() == 0)
        pl.a11y_fd = iris_a11y__fd();

    while (pl.running) {
        /* Sleep (inside poll) until the next frame is due, waking early on any
         * Wayland / wakeup / a11y event. Gating the render on a deadline is what
         * lets poll() actually block: presenting every iteration would make the
         * compositor flood us with frame-callback events so poll() never sleeps
         * and the loop spins at 100% CPU. With no frame scheduled (fully idle:
         * no animation, no focused caret) there is no deadline at all — poll
         * blocks until an event; the 200ms cap only keeps the sd-bus fd's
         * poll mask fresh. */
        long long t = now_ns();
        long long budget_ns = frame_scheduled ? next_deadline - t : 0;
        bool woke_on_event;
        if (!frame_scheduled) {
            woke_on_event = pump_events(&pl, 200);
        } else if (budget_ns > 0) {
            int budget_ms = (int)(budget_ns / 1000000LL);
            if (budget_ms > 200)
                budget_ms = 200;
            woke_on_event = pump_events(&pl, budget_ms);
        } else {
            woke_on_event = pump_events(&pl, 0);
        }

        /* Real user input wakes us out of the idle rate: pull the next render
         * forward (but never sooner than one active period after the last one,
         * so a burst of motion events can't exceed ~60 Hz) and mark the moment
         * so we stay at the active rate through the grace window. */
        if (acc_has_user_input(&pl, &prev_cx, &prev_cy)) {
            last_input_ns = now_ns();
            long long earliest = last_render_ns + ACTIVE_PERIOD_NS;
            if (!frame_scheduled || next_deadline > earliest)
                next_deadline = earliest;
            frame_scheduled = true;
        } else if (woke_on_event && !frame_scheduled) {
            /* A non-input event (wakeup-posted callback such as a theme
             * change, a11y, configure/scale) while fully idle: run one
             * frame so lens sees the new state; whether it paints is
             * decided by the repaint query below. */
            next_deadline = now_ns();
            frame_scheduled = true;
        }

        /* Not time to draw yet — keep draining events and sleeping. */
        if (!frame_scheduled || now_ns() < next_deadline)
            continue;

        /* Render this iteration. A tentative deadline is scheduled up front
         * (no catch-up bursts) so the error `continue`s in the present path
         * still retry; it is refined — or dropped entirely — after lens_end,
         * once the repaint query and animation state are known. A host
         * animation request made by the previous frame also keeps this
         * iteration at the active cadence. */
        t = now_ns();
        bool host_animating = pl.animation_frame_requested;
        pl.animation_frame_requested = false;
        long long period = (t - last_input_ns < INPUT_GRACE_NS || host_animating) ? ACTIVE_PERIOD_NS
                                                                                  : IDLE_PERIOD_NS;
        next_deadline = t + period;
        frame_scheduled = true;
        /* last_render_ns anchors the "earliest active frame after input"
         * throttle. It is refreshed only when a frame is actually rendered
         * (below, after a successful present) so a run of skipped frames
         * does not silently march the anchor forward. */
        long long render_anchor_ns = t;

        /* Scale change (e.g. surface dragged to a HiDPI output): apply
         * the new buffer scale, resize the swapchain in device pixels,
         * and tell lens so its replay transform matches. The first frame
         * after a resize/scale must always paint (the swapchain contents
         * were discarded), so remember it for the skip decision below. */
        bool resized_this_frame = false;
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
            resized_this_frame = true;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        float dt = (float)(now.tv_sec - prev.tv_sec) + (float)(now.tv_nsec - prev.tv_nsec) * 1e-9f;
        if (dt <= 0.0f)
            dt = 1.0f / 60.0f;
        prev = now;

        lens_input in;

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

        /* Cursor: an explicit host iris_set_cursor pins the cursor; while
         * it is DEFAULT, follow the semantic hint of the lens widget under
         * the pointer (I-beam over text fields, hand over clickable
         * elements, …) — same policy as the Win32/Cocoa backends. */
        if (cursor_follow_hint(&pl))
            cursor_apply(&pl);

        /* IM: per-widget enable/disable, surrounding-text reports, and the
         * IME candidate-window rectangle (owns every text-input-v3 request;
         * see im_frame_update). */
        im_frame_update(&pl);

        /* Static-frame skip: when lens reports no damage, the host has no
         * paint callback, no host animation is in flight, and no resize /
         * scale just happened, the frame just built is pixel-identical to
         * what is on screen — skip the whole begin_frame → canvas → present
         * cycle (no swapchain image acquired, nothing committed).
         *
         * Hosts with a paint callback can opt into the same skip per frame
         * via iris_paint_mark_static(): their content is opaque to lens, so
         * only they know whether it moved. The declaration is consumed here
         * and must be re-issued every frame, so a stale flag can never skip
         * a frame the host wanted painted; lens chrome damage, host
         * animation, and resizes always force a paint regardless. */
        /* A static declaration is the host's final word for the frame's
         * canvas content — but per the iris_paint_mark_static contract, a
         * following iris_request_animation_frame outranks it: a host that
         * asks for another frame is announcing its content is about to
         * change, so the skip must never eat the request. The request is
         * sampled and cleared above (host_animating) exactly like win32
         * and cocoa; the scheduling branch below re-arms the active
         * cadence off the same flag. It only covers the host's own
         * pixels: lens chrome damage still forces a paint, or a hover
         * highlight would freeze mid-transition while the host scene is
         * static. Resizes and the not-yet-presented latch force a paint
         * too. */
        bool chrome_damaged = lens_frame_needs_repaint(ui);
        bool host_canvas_static = cfg->paint != NULL && pl.paint_static && !host_animating &&
                                  !resized_this_frame && !surface_needs_paint && !chrome_damaged;
        pl.paint_static = false;
        bool must_paint =
            !host_canvas_static && (cfg->paint != NULL || chrome_damaged || host_animating ||
                                    resized_this_frame || surface_needs_paint);
        if (must_paint) {
            surface_needs_paint = true;
            flux_frame *frame = NULL;
            flux_result r = flux_surface_begin_frame(surface, NULL, &frame);
            if (r == FLUX_ERROR_SURFACE_LOST) {
                (void)flux_surface_resize(surface, (uint32_t)(pl.width * pl.buffer_scale),
                                          (uint32_t)(pl.height * pl.buffer_scale));
                continue;
            }
            if (r == FLUX_ERROR_INVALID_STATE)
                continue;
            /* Acquire timeout (display asleep or surface occluded): not an
             * error — no swapchain image was consumed, so skip this frame and
             * retry on the next deadline instead of exiting the loop. */
            if (r == FLUX_ERROR_TIMEOUT)
                continue;
            if (r != FLUX_OK)
                break;

            flux_surface_info info;
            flux_surface_get_info(surface, &info);

            /* Clear to the current theme's body background so empty areas
             * (e.g. short content in a tall window) don't show a hard-coded
             * dark color in light mode. The paint callback (if any) draws
             * *under* lens's chrome: iris calls it before lens_render so the
             * host's document surface lands first and lens's widget layer
             * composites on top. */
            lens_theme th = lens_get_theme(ui);
            flux_color clear = th.color_bg;
            bool drew = false;
            if (flux_canvas_begin(canvas, frame, &clear) == FLUX_OK) {
                if (cfg->paint)
                    cfg->paint(canvas, device, (float)pl.buffer_scale, cfg->user);
                drew = lens_render(ui, canvas) == FLUX_OK;
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
            else if (drew) {
                surface_needs_paint = false;
                last_render_ns = render_anchor_ns;
            }

            if (++frame_no == 1)
                fprintf(stderr, "first frame presented: %dx%d logical, %ux%u device (scale=%d)\n",
                        pl.width, pl.height, info.width, info.height, pl.buffer_scale);
        }

        /* Refine the tentative deadline now that the repaint query and the
         * post-build animation state are known. Stay at the active rate
         * while input is warm or an eased value / host animation is still
         * in flight; keep a low cadence for the caret blink while a text
         * field is focused; otherwise stop scheduling frames entirely —
         * the loop sleeps in poll() until the next event. Hosts with a
         * paint callback keep the always-render pacing unless they declared
         * this frame static: a static declaration is the host's promise
         * that nothing moved, so unscheduling is exactly as safe as for a
         * host without canvas content. */
        if (cfg->paint && !host_canvas_static) {
            if (pl.animation_frame_requested)
                next_deadline = last_render_ns + ACTIVE_PERIOD_NS;
            frame_scheduled = true;
        } else if (cfg->paint) {
            /* Static-declaring host: keep the low idle tick so build/paint
             * keep running (~4 Hz) and the host can observe state changes
             * and resume animating on their own; only the GPU work skips.
             * The request flag was already cleared into host_animating
             * above, so a host that asked for another frame during build
             * never reaches this skip branch. */
            next_deadline = t + IDLE_PERIOD_NS;
            frame_scheduled = true;
        } else if (t - last_input_ns < INPUT_GRACE_NS || pl.animation_frame_requested ||
                   lens_anim_pending(ui)) {
            next_deadline = t + ACTIVE_PERIOD_NS;
            frame_scheduled = true;
        } else if (lens_caret_rect(ui).w > 0.0f) {
            next_deadline = t + IDLE_PERIOD_NS;
            frame_scheduled = true;
        } else {
            frame_scheduled = false;
            /* Fully idle (no caret, no animation, no scheduled paint):
             * the text engine's high-water scratch from any earlier
             * one-off huge paste can go home (ADR-0072 item 5). Cheap
             * and null-safe; the next shape reallocates what it needs.
             * Skipped on the caret tick so an active text field never
             * pays a re-shape of its own content. */
            lens_text_compact(ui);
        }
    }

    rc = 0; /* success — fall through to the unified cleanup below */

    /* --- Cleanup (shared by the success path and every `goto fail`) --- */
fail:
    /* Let the host release every resource created from iris's borrowed
     * device before that device, the lens context, or the canvas disappears.
     * Keep the active platform published during the callback so thread-affine
     * iris helpers remain valid through the host's teardown. */
    if (host_started && cfg->stop)
        cfg->stop(ui, device, cfg->user);

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
    pl.ui = NULL; /* after this, queued main-thread callbacks must not touch lens */
    if (pl.theme_watching)
        iris_theme__unwatch_backend();
    if (pl.a11y_fd >= 0)
        iris_a11y_shutdown();
    /* Wakeup seam teardown: unregister the kick FIRST so a detached
     * subsystem thread (async paste, theme watcher) posting late fails
     * cleanly instead of queueing a job nothing would drain; then drain
     * whatever was legitimately queued (deliveries see pl.ui == NULL and
     * no-op), and close the eventfd. */
    if (pl.wakeup_fd >= 0) {
        iris_platform_wakeup_set_kick(NULL, NULL);
        iris_platform_wakeup_drain();
        close(pl.wakeup_fd);
        pl.wakeup_fd = -1;
    }
    /* Key-repeat timer: disarmed by the loop exit already (no key events
     * arrive anymore); just close the fd. */
    if (pl.repeat_fd >= 0) {
        close(pl.repeat_fd);
        pl.repeat_fd = -1;
    }
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
     * heap buffer and any still-live offer so teardown doesn't leak). A
     * drop read still in flight owns its offer via dnd_job_offer (the
     * helper thread only reads the pipe fd; finishing here is safe — the
     * late delivery will find the slot NULL and skip the offer). */
    free(pl.copy_buf);
    free(pl.im_surr);
    if (pl.dnd_job_offer) {
        wl_data_offer_finish(pl.dnd_job_offer);
        wl_data_offer_destroy(pl.dnd_job_offer);
        pl.dnd_job_offer = NULL;
    }
    if (pl.copy_source)
        wl_data_source_destroy(pl.copy_source);
    if (pl.selection_offer)
        wl_data_offer_destroy(pl.selection_offer);
    if (pl.dnd_offer && pl.dnd_offer != pl.selection_offer)
        wl_data_offer_destroy(pl.dnd_offer);
    if (pl.pending_offer_mimes.offer && pl.pending_offer_mimes.offer != pl.selection_offer &&
        pl.pending_offer_mimes.offer != pl.dnd_offer)
        wl_data_offer_destroy(pl.pending_offer_mimes.offer);

    /* Primary selection: our source (if still live), the current offer,
     * any unbound pending offer, the per-seat device, and the manager. */
    if (pl.primsel_source)
        zwp_primary_selection_source_v1_destroy(pl.primsel_source);
    if (pl.primsel_offer)
        zwp_primary_selection_offer_v1_destroy(pl.primsel_offer);
    if (pl.primsel_pending_mimes.offer)
        zwp_primary_selection_offer_v1_destroy(
            (struct zwp_primary_selection_offer_v1 *)pl.primsel_pending_mimes.offer);
    if (pl.primsel_device)
        zwp_primary_selection_device_v1_destroy(pl.primsel_device);
    if (pl.primsel_mgr)
        zwp_primary_selection_device_manager_v1_destroy(pl.primsel_mgr);

    if (pl.foreign_exported)
        zxdg_exported_v2_destroy(pl.foreign_exported);
    if (pl.foreign_exporter)
        zxdg_exporter_v2_destroy(pl.foreign_exporter);

    if (pl.deco)
        zxdg_toplevel_decoration_v1_destroy(pl.deco);
    if (pl.toplevel)
        xdg_toplevel_destroy(pl.toplevel);
    if (pl.xdg_surface)
        xdg_surface_destroy(pl.xdg_surface);
    if (pl.fractional_scale_obj)
        wp_fractional_scale_v1_destroy(pl.fractional_scale_obj);
    if (pl.surface)
        wl_surface_destroy(pl.surface);
    for (int i = 0; i < pl.n_outputs; i++)
        if (pl.outputs[i].wl)
            wl_output_destroy(pl.outputs[i].wl);
    if (pl.xkb_state)
        xkb_state_unref(pl.xkb_state);
    if (pl.xkb_keymap)
        xkb_keymap_unref(pl.xkb_keymap);
    if (pl.compose_state)
        xkb_compose_state_unref(pl.compose_state);
    if (pl.compose_table)
        xkb_compose_table_unref(pl.compose_table);
    if (pl.xkb_ctx)
        xkb_context_unref(pl.xkb_ctx);
    if (pl.pointer)
        wp_pointer_release(pl.pointer);
    if (pl.keyboard)
        wp_keyboard_release(pl.keyboard);
    if (pl.touch)
        wp_touch_release(pl.touch);
    if (pl.text_input)
        zwp_text_input_v3_destroy(pl.text_input);
    if (pl.data_device)
        wp_data_device_release(pl.data_device);
    if (pl.text_input_mgr)
        zwp_text_input_manager_v3_destroy(pl.text_input_mgr);
    if (pl.fractional_scale_mgr)
        wp_fractional_scale_manager_v1_destroy(pl.fractional_scale_mgr);
    if (pl.deco_mgr)
        zxdg_decoration_manager_v1_destroy(pl.deco_mgr);
    if (pl.data_device_mgr)
        wl_data_device_manager_destroy(pl.data_device_mgr);
    if (pl.seat)
        wp_seat_release(pl.seat);
    if (pl.wm_base)
        xdg_wm_base_destroy(pl.wm_base);
    if (pl.compositor)
        wl_compositor_destroy(pl.compositor);
    if (pl.registry)
        wl_registry_destroy(pl.registry);
    if (pl.display) {
        wl_display_roundtrip(pl.display); /* let the compositor process destroys */
        wl_display_disconnect(pl.display);
    }
    return rc;
}
