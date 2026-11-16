/*
 * iris/host.h — the UI-host seam: how a UI layer plugs into the iris frame loop.
 *
 * iris owns the window, the event pump, the flux device/surface/canvas and the
 * frame cadence. It does not know what draws the UI. A UI layer (lens through
 * `aperture`, or anything else) implements `iris_ui_host` and hands it to
 * iris_app_run via `iris_app_opts.ui`; iris calls it at fixed points of the
 * frame and never includes a UI header.
 *
 * Per frame, on the iris_app_run thread:
 *
 *     native events → iris_event → on_event → fold → iris_input
 *       → ui.frame(in, &report)          build + layout + interaction
 *       → pacer decides paint / skip / quiesce from `report`
 *       → ui.snapshot_create             immutable visuals for this build
 *       → ui.snapshot_submit(canvas)     may run on the presentation thread
 *       → ui.snapshot_activate           the frame reached the display
 *
 * Threading: every callback runs on the iris_app_run thread except
 * `snapshot_submit`, `snapshot_retain` and `snapshot_release`, which may run on
 * the presentation thread (ADR-0124) and must only touch the snapshot.
 *
 * Every function pointer is optional; a null entry is a no-op with a neutral
 * result. `iris_app_opts.ui == nullptr` is a window with host canvas content
 * only.
 */
#ifndef IRIS_HOST_H
#define IRIS_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <flux/canvas.h>
#include <flux/core.h>
#include <flux/math.h>
#include <iris/a11y.h>
#include <iris/a11y_prefs.h>
#include <iris/app.h>
#include <iris/cursor.h>
#include <iris/event.h>
#include <iris/theme.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Folded input                                                       */
/* ================================================================== */

#define IRIS_INPUT_MAX_KEYS 16
#define IRIS_TEXT_MAX 256
#define IRIS_PREEDIT_MAX 256

typedef struct iris_key_event {
    int key;      /* IRIS_KEY_* or unshifted printable ASCII */
    bool pressed; /* down edge if true, up edge if false     */
    bool repeat;  /* synthesised auto-repeat press           */
} iris_key_event;

/* Every event that survived `on_event` since the previous frame, folded into
 * one value. Level state (cursor, buttons held, mods) persists across frames;
 * edges, scroll, text and keys are this frame's only. */
typedef struct iris_input {
    flux_point cursor; /* logical surface-local pixels */
    bool mouse_down[IRIS_BUTTON_COUNT];
    bool mouse_pressed[IRIS_BUTTON_COUNT];
    bool mouse_released[IRIS_BUTTON_COUNT];
    float scroll_x, scroll_y;               /* wheel steps (notches)          */
    float scroll_pixels_x, scroll_pixels_y; /* continuous (touchpad) distance */

    uint32_t mods;                 /* IRIS_MOD_* level state  */
    char text_utf8[IRIS_TEXT_MAX]; /* committed text this frame */
    iris_key_event keys[IRIS_INPUT_MAX_KEYS];
    uint32_t key_count;

    flux_point display_size; /* logical surface extent           */
    float dt_seconds;        /* simulation step for this frame   */

    char preedit_utf8[IRIS_PREEDIT_MAX]; /* IME composition in progress */
    uint32_t preedit_cursor;             /* caret byte offset           */
    uint32_t preedit_sel_lo;             /* active clause, byte range   */
    uint32_t preedit_sel_hi;
    uint32_t ime_delete_before; /* IME delete_surrounding, bytes */
    uint32_t ime_delete_after;

    bool pen_active;    /* tool in proximity and down    */
    float pen_pressure; /* normalised 0..1               */
    bool pen_eraser;
} iris_input;

/* ================================================================== */
/*  Frame report                                                       */
/* ================================================================== */

/* The focused text widget, for the platform IME. `utf8 == nullptr` when no
 * text widget is focused. The buffer is borrowed until the next frame. */
typedef struct iris_text_context {
    const char *utf8;
    uint32_t len;
    uint32_t cursor; /* caret byte offset in utf8                    */
    bool multiline;
    flux_rect caret; /* logical px; w == 0 when there is no caret    */
} iris_text_context;

/* What the UI layer tells the frame loop after building one frame. */
typedef struct iris_ui_frame {
    bool damaged;       /* visuals differ from the last activated snapshot  */
    bool animating;     /* a time-based value is still in flight            */
    bool caret_ticking; /* a caret blink clock needs the idle tick          */
    iris_cursor cursor; /* semantic cursor under the pointer; DEFAULT = none */
    iris_text_context text;
    flux_color clear_color; /* surface background for this frame             */
} iris_ui_frame;

/* ================================================================== */
/*  Platform services offered to the UI layer                          */
/* ================================================================== */

/* Clipboard and drag source, bound to the active window. Paste is
 * asynchronous: `clipboard_request_text` is answered later through
 * `iris_ui_host.paste`, never inside the request. */
typedef struct iris_ui_services {
    void (*clipboard_set_text)(const char *utf8, size_t len, void *user);
    void (*clipboard_request_text)(void *user);
    int (*start_drag)(const char *text, size_t len, uint32_t actions, void *user);
    void *user;
} iris_ui_services;

typedef struct iris_ui_start_info {
    flux_device *device; /* borrowed for the whole run                 */
    float scale;         /* device-pixel ratio at startup              */
    bool dark;           /* resolved colour scheme (forced or system)  */
    iris_ui_services services;
} iris_ui_start_info;

/* ================================================================== */
/*  The UI host                                                        */
/* ================================================================== */

/* Immutable visuals of one built frame; owned and defined by the UI layer. */
typedef struct iris_ui_snapshot iris_ui_snapshot;

#define IRIS_DAMAGE_RECTS_CAPACITY 64

struct iris_ui_host {
    void *user;

    /* Lifecycle. `start` runs once the device, surface and canvas exist and
     * before the first frame; false aborts the run. `stop` runs after the
     * frame loop while every iris object is still alive. `destroy` runs after
     * the device has gone idle and must release everything the UI layer owns. */
    bool (*start)(void *user, const iris_ui_start_info *info);
    void (*stop)(void *user);
    void (*destroy)(void *user);

    /* Build one frame from `in` and report what the loop needs to know. */
    void (*frame)(void *user, const iris_input *in, iris_ui_frame *out);

    /* Environment changes, delivered between frames. */
    void (*set_scale)(void *user, float scale);
    void (*set_color_scheme)(void *user, iris_color_scheme scheme);
    void (*set_a11y_prefs)(void *user, const iris_a11y_prefs *prefs);

    /* Asynchronous deliveries. */
    void (*paste)(void *user, const char *utf8, size_t len);
    void (*drop)(void *user, const char *utf8, size_t len, flux_point pos);

    /* The loop is about to block with nothing pending: trim caches. */
    void (*quiesce)(void *user);

    /* Logical-pixel damage of the frame just built. Returns the number of
     * rectangles written; 0 means "everything". */
    uint32_t (*damage_rects)(void *user, flux_rect *rects, uint32_t capacity);

    /* Presentation. */
    flux_result (*snapshot_create)(void *user, iris_ui_snapshot **out);
    flux_result (*snapshot_submit)(const iris_ui_snapshot *snapshot, flux_canvas *canvas);
    flux_result (*snapshot_activate)(void *user, iris_ui_snapshot *snapshot);
    iris_ui_snapshot *(*snapshot_retain)(iris_ui_snapshot *snapshot);
    void (*snapshot_release)(iris_ui_snapshot *snapshot);

    /* Accessibility: enumerate the semantic tree of the activated snapshot,
     * and activate a node on behalf of an assistive client. */
    void (*a11y_walk)(void *user, iris_a11y_visit_fn visit, void *visit_user);
    void (*a11y_activate)(void *user, iris_node_id id);
};

#ifdef __cplusplus
}
#endif

#endif /* IRIS_HOST_H */
