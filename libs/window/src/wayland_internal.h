/* wayland_internal.h — state shared by the Wayland backend's translation units.
 *
 * app_wayland.c owns the window, the seat and the frame loop. wayland_data.c
 * owns data transfer: clipboard, primary selection and drag-and-drop. Both
 * work on the one wp_platform declared here. Nothing outside the Wayland
 * backend includes this header. */
#ifndef OPTICS_WINDOW_WAYLAND_INTERNAL_H
#define OPTICS_WINDOW_WAYLAND_INTERNAL_H

#include "a11y_internal.h"
#include "a11y_prefs_internal.h"
#include "platform_input.h"
#include "platform_internal.h"
#include "platform_text.h"
#include "platform_wakeup.h"
#include "tablet_wayland.h"
#include "theme_watch_internal.h"

#include <optics/window/a11y.h>
#include <optics/window/cursor.h>
#include <optics/window/dnd.h>
#include <optics/window/state.h>
#include <optics/window/theme.h>

#include <optics/gfx.h>
#include <optics/gfx/vulkan.h>

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
#include "display_pacer.h"
#include "fd_observe.h"
#include "fractional-scale-v1-client-protocol.h"
#include "frame_mailbox.h"
#include "presentation-time-client-protocol.h"
#include "ui_host.h"
#include "xdg-activation-v1-client-protocol.h"
#include <linux/input-event-codes.h> /* BTN_LEFT / BTN_RIGHT / BTN_MIDDLE — Linux-only, this file only */
#include <sched.h>
#include <semaphore.h>
#include <sys/resource.h>

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
/*  Accumulated input, drained into one optics_window_input per frame             */
/* ------------------------------------------------------------------ */

typedef struct wp_accum {
    double cx, cy;                            /* latest cursor, surface-local   */
    bool down[OPTICS_WINDOW_BUTTON_COUNT];    /* held state (persists)          */
    bool pressed[OPTICS_WINDOW_BUTTON_COUNT]; /* edges this frame               */
    bool released[OPTICS_WINDOW_BUTTON_COUNT];
    double scroll_x, scroll_y;               /* wheel steps (notches) this frame */
    double scroll_px_x, scroll_px_y;         /* continuous (touchpad) distance,
                                                logical px (see ADR-0036)      */
    uint32_t axis_source;                    /* this event group's axis source
                                                 (WL_POINTER_AXIS_SOURCE_*),
                                                 scoped by wl_pointer.frame     */
    uint32_t mods;                           /* level state (persists)         */
    char text[256];                          /* committed text this frame; sized
                                                like optics_window_input.text_utf8       */
    char preedit[OPTICS_WINDOW_PREEDIT_MAX]; /* IME preedit string             */
    uint32_t preedit_cursor;                 /* caret byte offset in preedit   */
    uint32_t preedit_sel_lo;                 /* active clause, byte range      */
    uint32_t preedit_sel_hi;
    optics_window_key_event keys[OPTICS_WINDOW_INPUT_MAX_KEYS];
    uint32_t key_count;
} wp_accum;

/* The staging buffers feed optics_window_input by whole-buffer memcpy in
 * drain_input; pin the sizes so a ui-side change fails at compile time
 * instead of over-reading the accumulator. */
static_assert(sizeof((wp_accum *)0)->text == sizeof((optics_window_input *)0)->text_utf8,
              "wp_accum.text must match optics_window_input.text_utf8");
static_assert(sizeof((wp_accum *)0)->preedit == sizeof((optics_window_input *)0)->preedit_utf8,
              "wp_accum.preedit must match optics_window_input.preedit_utf8");

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
    bool uri_list;           /* text/uri-list                */
    bool text_utf8;          /* text/plain;charset=utf-8     */
    bool text_plain;         /* text/plain                   */
    bool gnome_copied_files; /* x-special/gnome-copied-files */
};

typedef struct wp_platform {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    uint32_t subcompositor_name;
    struct xdg_wm_base *wm_base;
    struct xdg_activation_v1 *activation;
    uint32_t activation_name;
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
    uint32_t last_serial;     /* most recent input serial (set_selection) */
    bool dnd_inside;          /* drag currently over our surface           */
    optics_gfx_point dnd_pos; /* last reported drag position (surface px)  */

    /* Drag-and-drop source state (ADR-0086) */
    struct wl_data_source *drag_source;
    optics_window_dnd_source drag_info;
    char *drag_text_buf;
    size_t drag_text_len;
    bool drag_active;

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
     * `cursor_shape_mgr` stays NULL and optics_window_set_cursor is a no-op. */
    struct wp_cursor_shape_manager_v1 *cursor_shape_mgr;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    /* Cursor-shape device for the tablet tool in proximity over our surface
     * (NULL otherwise), and the proximity_in serial set_shape requires. */
    struct wp_cursor_shape_device_v1 *tablet_cursor_device;
    uint32_t tablet_cursor_serial;
    optics_window_cursor host_cursor; /* last explicit optics_window_set_cursor (DEFAULT = none) */
    optics_window_cursor effective_cursor; /* what the compositor is asked to show       */
    uint32_t cursor_serial;                /* most recent enter/motion serial */
    bool cursor_inside;                    /* pointer currently inside our surface */
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
    int rep_ui_key;       /* its resolved ui key id (0 = unmappable) */
    bool rep_active;      /* timer armed */

    struct zwp_text_input_manager_v3 *text_input_mgr;
    struct zwp_text_input_v3 *text_input;
    struct wl_surface *text_input_surface;

    /* Per-widget IM ownership: the text input is enabled exactly while the
     * window has keyboard focus AND a ui text widget is focused
     * (re-evaluated once per frame after optics_ui_end — see im_frame_update).
     * `im_surr` is the last surrounding text reported to the compositor, so
     * set_surrounding_text goes out only on content/cursor changes. */
    bool kb_focused;
    bool im_active;
    char *im_surr;
    size_t im_surr_len;
    uint32_t im_surr_cursor;

    /* Pending IME state (double-buffered by text-input-v3 done event).
     * commit is sized like optics_window_input.text_utf8: a full-sentence IME
     * conversion must survive staging before the boundary-aware append
     * clips it into the per-frame accumulator. */
    struct {
        char commit[256];
        char preedit[OPTICS_WINDOW_PREEDIT_MAX];
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
    bool resized;                    /* size or scale changed -> resize swap  */
    bool animation_frame_requested;  /* host asked for active-rate follow-up */
    bool record_static;              /* host declared this frame's canvas content static */
    bool frame_skip_render;          /* host asked to skip rendering but keep the active cadence */
    const optics_window_ui_host *ui; /* live UI host; null before start / after teardown */
    optics_window_ui_frame ui_frame; /* report of the most recent UI frame   */

    /* Live colour-scheme watching + accessibility bridge: optional, fail-soft. */
    bool theme_watching;
    bool a11y_prefs_watching;
    bool a11y_running;
    int a11y_fd;

    /* Cross-thread wakeup seam (platform_wakeup.h): an eventfd in the
     * pump_events poll set. The colour-scheme watcher's pump thread posts
     * through it; the loop drains the queue on this thread. */
    int wakeup_fd;

    /* --- Display pacing (ADR-0122) ---------------------------------------
     * Presentation is paced by the display, not by a client timer. Two
     * compositor signals drive the loop:
     *
     *   wl_surface.frame        — the grant to draw the next frame. Requested
     *                             before each commit; the callback is what the
     *                             frame loop waits for instead of sleeping on
     *                             a hardcoded 60 Hz period.
     *   wp_presentation_feedback— the compositor's own report of when the
     *                             frame reached the display (latency / dropped
     *                             frames) and the real refresh interval, which
     *                             is how 120/144 Hz is learned rather than
     *                             assumed.
     *
     * `refresh_ns` stays 0 until one of those two sources reports a value;
     * the pacer then falls back to its "refresh rate not learned yet" period
     * rather than a hardcoded 60 Hz target. */
    struct wp_presentation *presentation;
    struct wl_callback *frame_cb; /* outstanding wl_surface.frame, or NULL  */
    bool frame_cb_pending;        /* requested, not yet fired              */
    bool frame_cb_fired;          /* fired this iteration                  */
    int64_t frame_cb_time_ns;     /* compositor timestamp of that callback */
    int64_t refresh_ns;           /* learned display period; 0 = unknown   */

    /* Decoupled dual-thread presentation engine (ADR-0124) */
    optics_window_frame_mailbox mailbox;
    pthread_t present_thread;
    sem_t present_sem;
    _Atomic(bool) presenter_running;
    bool has_presenter_thread;

    wp_accum acc;
} wp_platform;

/* Monotonic nanoseconds: frame pacing and read deadlines. */
static inline long long now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000000000LL + (long long)t.tv_nsec;
}

/* The platform published for the context-free public APIs (window/window.h,
 * window/cursor.h, window/dnd.h); null outside optics_window_run. Defined in
 * app_wayland.c. */
extern wp_platform *g_active_pl;

/* ---- app_wayland.c ------------------------------------------------ */

/* Route one drag-and-drop position through the event seam and, if it
 * survives, record it. Returns false when the host dropped the event. */
bool wp_emit_dnd(wp_platform *pl, double x, double y, bool active);

/* ---- wayland_data.c ----------------------------------------------- */

/* Create the seat's data devices once both the manager and the seat exist. */
void wp_maybe_create_data_device(wp_platform *pl);
void wp_maybe_create_primsel_device(wp_platform *pl);

/* Paste the primary selection (middle click) through the async read path. */
void wp_primsel_paste(wp_platform *pl);

/* optics_window_ui_services: clipboard and drag source for the UI layer. `user` is the
 * wp_platform. A text request is answered later through optics_window_ui_host.paste. */
void wp_clip_set_text(const char *utf8, size_t len, void *user);
void wp_clip_request_text(void *user);
int wp_start_drag(const char *text, size_t len, uint32_t actions, void *user);

#endif /* OPTICS_WINDOW_WAYLAND_INTERNAL_H */
