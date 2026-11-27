/*
 * window/event.h — the reified, platform-neutral input event (ADR-0107).
 *
 * window owns the platform event loop and is the sole event pump of the
 * stack (ADR-0043, ADR-0084). Native events — Wayland wl_pointer /
 * wl_keyboard / text-input-v3, Win32 WM_*, Cocoa NSEvent — are reified
 * into one `optics_window_event` per native event, in arrival order. The host may
 * observe, drop, or rewrite each event at the single pre-fold seam
 * (`optics_window_event_fn` in window/app.h) before window folds the survivors into the
 * frame's `optics_window_input` (window/host.h).
 *
 * The pipeline is deliberately one-directional:
 *
 *     native → [reify] → optics_window_event → [intercept: on_event] → [accumulate]
 *            → [fold into optics_window_input] → optics_window_ui_host.frame(in)
 *
 * Rules this type upholds (ADR-0107):
 *   - Platform-neutral. No native handles, pointers, or compositor
 *     serials. Platform-specific capability travels through the capability
 *     APIs (window/capability.h, ADR-0099), never through this struct.
 *   - One vocabulary. Key ids, modifier masks and pointer buttons are the
 *     OPTICS_WINDOW_KEY_* / OPTICS_WINDOW_MOD_* / OPTICS_WINDOW_BUTTON_* values defined below;
 * events and the folded optics_window_input share them.
 *   - Intrinsically ordered. `sequence` is a monotonic counter window stamps
 *     at emit time; it never wraps within an app run.
 *
 * Lifetime: an `optics_window_event` and any string it points at are valid only for
 * the duration of one `on_event` callback. Hosts that need to retain data
 * must copy it. Pointers are NULL / zero for event kinds without that
 * payload.
 */
#ifndef OPTICS_WINDOW_EVENT_H
#define OPTICS_WINDOW_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Input vocabulary                                                   */
/* ================================================================== */

typedef enum optics_window_button : uint32_t {
    OPTICS_WINDOW_BUTTON_LEFT = 0,
    OPTICS_WINDOW_BUTTON_RIGHT = 1,
    OPTICS_WINDOW_BUTTON_MIDDLE = 2,
} optics_window_button;
#define OPTICS_WINDOW_BUTTON_COUNT 3

/* Key ids. Printable ASCII (0x20-0x7E) travels as its unshifted codepoint
 * ('a', never 'A'); the values below name the non-printing keys. Non-ASCII
 * printable input never produces a key event — it arrives as committed text. */
#define OPTICS_WINDOW_KEY_ESCAPE 256
#define OPTICS_WINDOW_KEY_TAB 258
#define OPTICS_WINDOW_KEY_RETURN 259
#define OPTICS_WINDOW_KEY_BACKSPACE 260
#define OPTICS_WINDOW_KEY_DELETE 261
#define OPTICS_WINDOW_KEY_LEFT 262
#define OPTICS_WINDOW_KEY_RIGHT 263
#define OPTICS_WINDOW_KEY_HOME 264
#define OPTICS_WINDOW_KEY_END 265
#define OPTICS_WINDOW_KEY_UP 266
#define OPTICS_WINDOW_KEY_DOWN 267
#define OPTICS_WINDOW_KEY_F1 280
#define OPTICS_WINDOW_KEY_F2 281
#define OPTICS_WINDOW_KEY_F3 282
#define OPTICS_WINDOW_KEY_F4 283
#define OPTICS_WINDOW_KEY_F5 284
#define OPTICS_WINDOW_KEY_F6 285
#define OPTICS_WINDOW_KEY_F7 286
#define OPTICS_WINDOW_KEY_F8 287
#define OPTICS_WINDOW_KEY_F9 288
#define OPTICS_WINDOW_KEY_F10 289
#define OPTICS_WINDOW_KEY_F11 290
#define OPTICS_WINDOW_KEY_F12 291

/* Modifier level-state bits. */
#define OPTICS_WINDOW_MOD_SHIFT (1u << 0)
#define OPTICS_WINDOW_MOD_CTRL (1u << 1)
#define OPTICS_WINDOW_MOD_ALT (1u << 2)
#define OPTICS_WINDOW_MOD_SUPER (1u << 3)

/* ================================================================== */
/*  Event kind                                                         */
/* ================================================================== */

/* Fixed underlying type (C23): the wire identity of an event is a
 * uint32_t, stable across the stack and across platforms. */
typedef enum optics_window_event_type : uint32_t {
    OPTICS_WINDOW_EVENT_NONE = 0,

    OPTICS_WINDOW_EVENT_POINTER_ENTER,  /* payload: pointer                       */
    OPTICS_WINDOW_EVENT_POINTER_LEAVE,  /* payload: pointer (off-screen sentinel) */
    OPTICS_WINDOW_EVENT_POINTER_MOVE,   /* payload: pointer                       */
    OPTICS_WINDOW_EVENT_POINTER_BUTTON, /* payload: button                        */
    OPTICS_WINDOW_EVENT_SCROLL,         /* payload: scroll                        */

    OPTICS_WINDOW_EVENT_KEY,         /* payload: keyboard                      */
    OPTICS_WINDOW_EVENT_MODIFIERS,   /* payload: modifiers (level state)       */
    OPTICS_WINDOW_EVENT_TEXT_COMMIT, /* payload: text (IME / layout commit)    */
    OPTICS_WINDOW_EVENT_PREEDIT,     /* payload: preedit                       */
    OPTICS_WINDOW_EVENT_IME_DELETE,  /* payload: ime_delete (delete_surrounding)*/

    OPTICS_WINDOW_EVENT_TABLET, /* payload: tablet (pen: pressure, tilt)  */
    OPTICS_WINDOW_EVENT_DND,    /* payload: dnd (drag-and-drop)           */

    OPTICS_WINDOW_EVENT_RESIZE, /* payload: resize                        */
    OPTICS_WINDOW_EVENT_SCALE,  /* payload: scale                         */
    OPTICS_WINDOW_EVENT_FOCUS,  /* payload: focus                         */
    OPTICS_WINDOW_EVENT_CLOSE,  /* payload: none                          */
} optics_window_event_type;

/* Event flags, OR'd into optics_window_event.flags. */
typedef enum optics_window_event_flag : uint32_t {
    OPTICS_WINDOW_EVENT_F_NONE = 0,
    /* Lifecycle-critical: window must observe it to keep window state
     * coherent. The host may read it but cannot drop it; window ignores a
     * DROP disposition for flagged events (ADR-0107). */
    OPTICS_WINDOW_EVENT_F_UNDROPPABLE = 1u << 0,
} optics_window_event_flag;

/* ================================================================== */
/*  Event payloads & tagged event                                     */
/* ================================================================== */

typedef struct optics_window_event_pointer {
    float x, y; /* logical surface-local pixels */
} optics_window_event_pointer;

typedef struct optics_window_event_button {
    float x, y;   /* logical surface-local pixels     */
    int button;   /* OPTICS_WINDOW_BUTTON_*           */
    bool pressed; /* true = down edge, false = up edge */
} optics_window_event_button;

typedef struct optics_window_event_scroll {
    float dx, dy;    /* delta; units per `continuous` */
    bool continuous; /* true = logical px (touchpad)  */
                     /* false = wheel notches         */
} optics_window_event_scroll;

typedef struct optics_window_event_keyboard {
    int key;       /* OPTICS_WINDOW_KEY_* or printable ASCII      */
    bool pressed;  /* down edge / up edge                         */
    bool repeat;   /* synthesised auto-repeat press               */
    uint32_t mods; /* OPTICS_WINDOW_MOD_* level state at the edge */
} optics_window_event_keyboard;

typedef struct optics_window_event_modifiers {
    uint32_t mods; /* OPTICS_WINDOW_MOD_* level state */
} optics_window_event_modifiers;

typedef struct optics_window_event_text {
    const char *utf8; /* committed UTF-8, NUL-terminated */
    size_t len;       /* byte length, excluding NUL      */
} optics_window_event_text;

typedef struct optics_window_event_preedit {
    const char *utf8; /* preedit UTF-8, NUL-terminated */
    size_t len;       /* byte length, excluding NUL    */
    uint32_t cursor;  /* caret byte offset in utf8     */
    uint32_t sel_lo;  /* active clause byte range      */
    uint32_t sel_hi;
} optics_window_event_preedit;

typedef struct optics_window_event_ime_delete {
    uint32_t before; /* bytes to delete before the caret */
    uint32_t after;  /* bytes to delete after the caret  */
} optics_window_event_ime_delete;

typedef struct optics_window_event_tablet {
    float x, y;           /* logical surface-local pixels         */
    float pressure;       /* normalised 0..1                      */
    float tilt_x, tilt_y; /* normalised -1..1                     */
    bool eraser;          /* eraser end of the tool               */
    bool barrel;          /* barrel button pressed                */
    bool proximity;       /* tool in range (not necessarily down) */
} optics_window_event_tablet;

typedef struct optics_window_event_dnd {
    float x, y;       /* logical surface-local pixels        */
    const char *text; /* dropped text, or NULL               */
    size_t len;
    const char *mime; /* offered MIME type, or NULL          */
    bool active;      /* enter/motion = true, leave = false  */
} optics_window_event_dnd;

typedef struct optics_window_event_resize {
    uint32_t width, height; /* logical surface size */
} optics_window_event_resize;

typedef struct optics_window_event_scale {
    float scale; /* device-pixel ratio */
} optics_window_event_scale;

typedef struct optics_window_event_focus {
    bool focused; /* true = gained focus, false = lost */
} optics_window_event_focus;

typedef struct optics_window_event {
    optics_window_event_type type; /* which union member is live           */
    uint32_t flags;                /* optics_window_event_flag bits        */
    uint64_t sequence;             /* monotonic, stamped by window on emit */

    union {
        optics_window_event_pointer pointer;
        optics_window_event_button button;
        optics_window_event_scroll scroll;
        optics_window_event_keyboard keyboard;
        optics_window_event_modifiers modifiers;
        optics_window_event_text text;
        optics_window_event_preedit preedit;
        optics_window_event_ime_delete ime_delete;
        optics_window_event_tablet tablet;
        optics_window_event_dnd dnd;
        optics_window_event_resize resize;
        optics_window_event_scale scale;
        optics_window_event_focus focus;

        /* OPTICS_WINDOW_EVENT_NONE / OPTICS_WINDOW_EVENT_POINTER_LEAVE / OPTICS_WINDOW_EVENT_CLOSE
         * carry no payload. */
    };
} optics_window_event;

/* ================================================================== */
/*  Host interception seam                                             */
/* ================================================================== */

/* The host's answer to one intercepted event. */
typedef enum optics_window_event_disposition : uint32_t {
    OPTICS_WINDOW_EVENT_PASS = 0, /* deliver the (possibly rewritten) event   */
    OPTICS_WINDOW_EVENT_DROP = 1, /* discard it; it never reaches the snapshot */
} optics_window_event_disposition;

/* Host interception callback (optics_window_opts.on_event, ADR-0107).
 *
 * Invoked once per reified event, in arrival order, synchronously on the
 * optics_window_run thread, before folding and outside the UI layer's frame.
 * The host may mutate `*ev` fields and return PASS to deliver the
 * rewritten event, or return DROP to discard it. For events flagged
 * OPTICS_WINDOW_EVENT_F_UNDROPPABLE the host is still called but window ignores a
 * DROP. `user` is the pointer passed in optics_window_opts.
 *
 * The callback must not re-enter optics_window_run. */
typedef optics_window_event_disposition (*optics_window_event_fn)(optics_window_event *ev,
                                                                  void *user);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_WINDOW_EVENT_H */
