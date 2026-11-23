/*
 * window/host.h — the UI-host seam: how a UI layer plugs into the window frame loop.
 *
 * window owns the window, the event pump, the gfx device/surface/canvas and the
 * frame cadence. It does not know what draws the UI. A UI layer (ui through
 * `app`, or anything else) implements `optics_window_ui_host` and hands it to
 * optics_window_run via `optics_window_opts.ui`; window calls it at fixed points of the
 * frame and never includes a UI header.
 *
 * Per frame, on the optics_window_run thread:
 *
 *     native events → optics_window_event → on_event → fold → optics_window_input
 *       → ui.frame(in, &report)          build + layout + interaction
 *       → pacer decides paint / skip / quiesce from `report`
 *       → ui.snapshot_create             immutable visuals for this build
 *       → ui.snapshot_submit(canvas)     may run on the presentation thread
 *       → ui.snapshot_activate           the frame reached the display
 *
 * Threading: every callback runs on the optics_window_run thread except
 * `snapshot_submit`, `snapshot_retain` and `snapshot_release`, which may run on
 * the presentation thread (ADR-0124) and must only touch the snapshot.
 *
 * Every function pointer is optional; a null entry is a no-op with a neutral
 * result. `optics_window_opts.ui == nullptr` is a window with host canvas content
 * only.
 */
#ifndef OPTICS_WINDOW_HOST_H
#define OPTICS_WINDOW_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <optics/window/a11y.h>
#include <optics/window/a11y_prefs.h>
#include <optics/window/cursor.h>
#include <optics/window/event.h>
#include <optics/window/loop.h>
#include <optics/window/theme.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Folded input                                                       */
/* ================================================================== */

#define OPTICS_WINDOW_INPUT_MAX_KEYS 16
#define OPTICS_WINDOW_TEXT_MAX 256
#define OPTICS_WINDOW_PREEDIT_MAX 256

typedef struct optics_window_key_event {
    int key;      /* OPTICS_WINDOW_KEY_* or unshifted printable ASCII */
    bool pressed; /* down edge if true, up edge if false     */
    bool repeat;  /* synthesised auto-repeat press           */
} optics_window_key_event;

/* Every event that survived `on_event` since the previous frame, folded into
 * one value. Level state (cursor, buttons held, mods) persists across frames;
 * edges, scroll, text and keys are this frame's only. */
typedef struct optics_window_input {
    optics_gfx_point cursor; /* logical surface-local pixels */
    bool mouse_down[OPTICS_WINDOW_BUTTON_COUNT];
    bool mouse_pressed[OPTICS_WINDOW_BUTTON_COUNT];
    bool mouse_released[OPTICS_WINDOW_BUTTON_COUNT];
    float scroll_x, scroll_y;               /* wheel steps (notches)          */
    float scroll_pixels_x, scroll_pixels_y; /* continuous (touchpad) distance */

    uint32_t mods;                          /* OPTICS_WINDOW_MOD_* level state  */
    char text_utf8[OPTICS_WINDOW_TEXT_MAX]; /* committed text this frame */
    optics_window_key_event keys[OPTICS_WINDOW_INPUT_MAX_KEYS];
    uint32_t key_count;

    optics_gfx_point display_size; /* logical surface extent           */
    float dt_seconds;              /* simulation step for this frame   */

    char preedit_utf8[OPTICS_WINDOW_PREEDIT_MAX]; /* IME composition in progress */
    uint32_t preedit_cursor;                      /* caret byte offset           */
    uint32_t preedit_sel_lo;                      /* active clause, byte range   */
    uint32_t preedit_sel_hi;
    uint32_t ime_delete_before; /* IME delete_surrounding, bytes */
    uint32_t ime_delete_after;

    bool pen_active;    /* tool in proximity and down    */
    float pen_pressure; /* normalised 0..1               */
    bool pen_eraser;
} optics_window_input;

/* ================================================================== */
/*  Frame report                                                       */
/* ================================================================== */

/* The focused text widget, for the platform IME. `utf8 == nullptr` when no
 * text widget is focused. The buffer is borrowed until the next frame. */
typedef struct optics_window_text_context {
    const char *utf8;
    uint32_t len;
    uint32_t cursor; /* caret byte offset in utf8                    */
    bool multiline;
    optics_gfx_rect caret; /* logical px; w == 0 when there is no caret    */
} optics_window_text_context;

/* What the UI layer tells the frame loop after building one frame. */
typedef struct optics_window_ui_frame {
    bool damaged;                /* visuals differ from the last activated snapshot  */
    bool animating;              /* a time-based value is still in flight            */
    bool caret_ticking;          /* a caret blink clock needs the idle tick          */
    optics_window_cursor cursor; /* semantic cursor under the pointer; DEFAULT = none */
    optics_window_text_context text;
    optics_gfx_color clear_color; /* surface background for this frame             */
} optics_window_ui_frame;

/* ================================================================== */
/*  Platform services offered to the UI layer                          */
/* ================================================================== */

/* Clipboard and drag source, bound to the active window. Paste is
 * asynchronous: `clipboard_request_text` is answered later through
 * `optics_window_ui_host.paste`, never inside the request. */
typedef struct optics_window_ui_services {
    void (*clipboard_set_text)(const char *utf8, size_t len, void *user);
    void (*clipboard_request_text)(void *user);
    int (*start_drag)(const char *text, size_t len, uint32_t actions, void *user);
    void *user;
} optics_window_ui_services;

typedef struct optics_window_ui_start_info {
    optics_gfx_device *device; /* borrowed for the whole run                 */
    float scale;               /* device-pixel ratio at startup              */
    bool dark;                 /* resolved colour scheme (forced or system)  */
    optics_window_ui_services services;
} optics_window_ui_start_info;

/* ================================================================== */
/*  The UI host                                                        */
/* ================================================================== */

/* Immutable visuals of one built frame; owned and defined by the UI layer. */
typedef struct optics_window_ui_snapshot optics_window_ui_snapshot;

#define OPTICS_WINDOW_DAMAGE_RECTS_CAPACITY 64

struct optics_window_ui_host {
    void *user;

    /* Lifecycle. `start` runs once the device, surface and canvas exist and
     * before the first frame; false aborts the run. `stop` runs after the
     * frame loop while every window object is still alive. `destroy` runs after
     * the device has gone idle and must release everything the UI layer owns. */
    bool (*start)(void *user, const optics_window_ui_start_info *info);
    void (*stop)(void *user);
    void (*destroy)(void *user);

    /* Build one frame from `in` and report what the loop needs to know. */
    void (*frame)(void *user, const optics_window_input *in, optics_window_ui_frame *out);

    /* Environment changes, delivered between frames. */
    void (*set_scale)(void *user, float scale);
    void (*set_color_scheme)(void *user, optics_window_color_scheme scheme);
    void (*set_a11y_prefs)(void *user, const optics_window_a11y_prefs *prefs);

    /* Asynchronous deliveries. */
    void (*paste)(void *user, const char *utf8, size_t len);
    void (*drop)(void *user, const char *utf8, size_t len, optics_gfx_point pos);

    /* The loop is about to block with nothing pending: trim caches. */
    void (*quiesce)(void *user);

    /* Logical-pixel damage of the frame just built. Returns the number of
     * rectangles written; 0 means "everything". */
    uint32_t (*damage_rects)(void *user, optics_gfx_rect *rects, uint32_t capacity);

    /* Presentation. */
    optics_gfx_result (*snapshot_create)(void *user, optics_window_ui_snapshot **out);
    optics_gfx_result (*snapshot_submit)(const optics_window_ui_snapshot *snapshot,
                                         optics_gfx_canvas *canvas);
    optics_gfx_result (*snapshot_activate)(void *user, optics_window_ui_snapshot *snapshot);
    optics_window_ui_snapshot *(*snapshot_retain)(optics_window_ui_snapshot *snapshot);
    void (*snapshot_release)(optics_window_ui_snapshot *snapshot);

    /* Accessibility: enumerate the semantic tree of the activated snapshot,
     * and activate a node on behalf of an assistive client. */
    void (*a11y_walk)(void *user, optics_window_a11y_visit_fn visit, void *visit_user);
    void (*a11y_activate)(void *user, optics_window_node_id id);
};

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_HOST_H */
