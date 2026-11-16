/*
 * iris/event.h — the reified, platform-neutral input event (ADR-0107).
 *
 * iris owns the platform event loop and is the sole event pump of the
 * stack (ADR-0043, ADR-0084). Native events — Wayland wl_pointer /
 * wl_keyboard / text-input-v3, Win32 WM_*, Cocoa NSEvent — are reified
 * into one `iris_event` per native event, in arrival order. The host may
 * observe, drop, or rewrite each event at the single pre-fold seam
 * (`iris_event_fn` in iris/app.h) before iris folds the survivors into the
 * frame's `iris_input` (iris/host.h).
 *
 * The pipeline is deliberately one-directional:
 *
 *     native → [reify] → iris_event → [intercept: on_event] → [accumulate]
 *            → [fold into iris_input] → iris_ui_host.frame(in)
 *
 * Rules this type upholds (ADR-0107):
 *   - Platform-neutral. No native handles, pointers, or compositor
 *     serials. Platform-specific capability travels through the capability
 *     APIs (iris/capability.h, ADR-0099), never through this struct.
 *   - One vocabulary. Key ids, modifier masks and pointer buttons are the
 *     IRIS_KEY_* / IRIS_MOD_* / IRIS_BUTTON_* values defined below; events and
 *     the folded iris_input share them.
 *   - Intrinsically ordered. `sequence` is a monotonic counter iris stamps
 *     at emit time; it never wraps within an app run.
 *
 * Lifetime: an `iris_event` and any string it points at are valid only for
 * the duration of one `on_event` callback. Hosts that need to retain data
 * must copy it. Pointers are NULL / zero for event kinds without that
 * payload.
 */
#ifndef IRIS_EVENT_H
#define IRIS_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Input vocabulary                                                   */
/* ================================================================== */

typedef enum iris_button : uint32_t {
    IRIS_BUTTON_LEFT = 0,
    IRIS_BUTTON_RIGHT = 1,
    IRIS_BUTTON_MIDDLE = 2,
} iris_button;
#define IRIS_BUTTON_COUNT 3

/* Key ids. Printable ASCII (0x20-0x7E) travels as its unshifted codepoint
 * ('a', never 'A'); the values below name the non-printing keys. Non-ASCII
 * printable input never produces a key event — it arrives as committed text. */
#define IRIS_KEY_ESCAPE 256
#define IRIS_KEY_TAB 258
#define IRIS_KEY_RETURN 259
#define IRIS_KEY_BACKSPACE 260
#define IRIS_KEY_DELETE 261
#define IRIS_KEY_LEFT 262
#define IRIS_KEY_RIGHT 263
#define IRIS_KEY_HOME 264
#define IRIS_KEY_END 265
#define IRIS_KEY_UP 266
#define IRIS_KEY_DOWN 267
#define IRIS_KEY_F1 280
#define IRIS_KEY_F2 281
#define IRIS_KEY_F3 282
#define IRIS_KEY_F4 283
#define IRIS_KEY_F5 284
#define IRIS_KEY_F6 285
#define IRIS_KEY_F7 286
#define IRIS_KEY_F8 287
#define IRIS_KEY_F9 288
#define IRIS_KEY_F10 289
#define IRIS_KEY_F11 290
#define IRIS_KEY_F12 291

/* Modifier level-state bits. */
#define IRIS_MOD_SHIFT (1u << 0)
#define IRIS_MOD_CTRL (1u << 1)
#define IRIS_MOD_ALT (1u << 2)
#define IRIS_MOD_SUPER (1u << 3)

/* ================================================================== */
/*  Event kind                                                         */
/* ================================================================== */

/* Fixed underlying type (C23): the wire identity of an event is a
 * uint32_t, stable across the stack and across platforms. */
typedef enum iris_event_type : uint32_t {
    IRIS_EVENT_NONE = 0,

    IRIS_EVENT_POINTER_ENTER,  /* payload: pointer                       */
    IRIS_EVENT_POINTER_LEAVE,  /* payload: pointer (off-screen sentinel) */
    IRIS_EVENT_POINTER_MOVE,   /* payload: pointer                       */
    IRIS_EVENT_POINTER_BUTTON, /* payload: button                        */
    IRIS_EVENT_SCROLL,         /* payload: scroll                        */

    IRIS_EVENT_KEY,         /* payload: keyboard                      */
    IRIS_EVENT_MODIFIERS,   /* payload: modifiers (level state)       */
    IRIS_EVENT_TEXT_COMMIT, /* payload: text (IME / layout commit)    */
    IRIS_EVENT_PREEDIT,     /* payload: preedit                       */
    IRIS_EVENT_IME_DELETE,  /* payload: ime_delete (delete_surrounding)*/

    IRIS_EVENT_TABLET, /* payload: tablet (pen: pressure, tilt)  */
    IRIS_EVENT_DND,    /* payload: dnd (drag-and-drop)           */

    IRIS_EVENT_RESIZE, /* payload: resize                        */
    IRIS_EVENT_SCALE,  /* payload: scale                         */
    IRIS_EVENT_FOCUS,  /* payload: focus                         */
    IRIS_EVENT_CLOSE,  /* payload: none                          */
} iris_event_type;

/* Event flags, OR'd into iris_event.flags. */
typedef enum iris_event_flag : uint32_t {
    IRIS_EVENT_F_NONE = 0,
    /* Lifecycle-critical: iris must observe it to keep window state
     * coherent. The host may read it but cannot drop it; iris ignores a
     * DROP disposition for flagged events (ADR-0107). */
    IRIS_EVENT_F_UNDROPPABLE = 1u << 0,
} iris_event_flag;

/* ================================================================== */
/*  The event                                                          */
/* ================================================================== */

typedef struct iris_event {
    iris_event_type type; /* which union member is live             */
    uint32_t flags;       /* iris_event_flag bits                   */
    uint64_t sequence;    /* monotonic, stamped by iris on emit     */

    union {
        struct {
            float x, y; /* logical surface-local pixels */
        } pointer;

        struct {
            float x, y;   /* logical surface-local pixels     */
            int button;   /* IRIS_BUTTON_*            */
            bool pressed; /* true = down edge, false = up edge  */
        } button;

        struct {
            float dx, dy;    /* delta; units per `continuous`   */
            bool continuous; /* true = logical px (touchpad)    */
                             /* false = wheel notches           */
        } scroll;

        struct {
            int key;       /* IRIS_KEY_* or printable ASCII      */
            bool pressed;  /* down edge / up edge                */
            bool repeat;   /* synthesised auto-repeat press      */
            uint32_t mods; /* IRIS_MOD_* level state at the edge */
        } keyboard;

        struct {
            uint32_t mods; /* IRIS_MOD_* level state */
        } modifiers;

        struct {
            const char *utf8; /* committed UTF-8, NUL-terminated */
            size_t len;       /* byte length, excluding NUL      */
        } text;

        struct {
            const char *utf8; /* preedit UTF-8, NUL-terminated   */
            size_t len;       /* byte length, excluding NUL      */
            uint32_t cursor;  /* caret byte offset in utf8       */
            uint32_t sel_lo;  /* active clause byte range        */
            uint32_t sel_hi;
        } preedit;

        struct {
            uint32_t before; /* bytes to delete before the caret */
            uint32_t after;  /* bytes to delete after the caret  */
        } ime_delete;

        struct {
            float x, y;           /* logical surface-local pixels */
            float pressure;       /* normalised 0..1             */
            float tilt_x, tilt_y; /* normalised -1..1        */
            bool eraser;          /* eraser end of the tool      */
            bool barrel;          /* barrel button pressed       */
            bool proximity;       /* tool in range (not necessarily down) */
        } tablet;

        struct {
            float x, y;       /* logical surface-local pixels   */
            const char *text; /* dropped text, or NULL          */
            size_t len;
            const char *mime; /* offered MIME type, or NULL     */
            bool active;      /* enter/motion = true, leave = false */
        } dnd;

        struct {
            uint32_t width, height; /* logical surface size */
        } resize;

        struct {
            float scale; /* device-pixel ratio */
        } scale;

        struct {
            bool focused; /* true = gained focus, false = lost */
        } focus;

        /* IRIS_EVENT_NONE / IRIS_EVENT_POINTER_LEAVE / IRIS_EVENT_CLOSE
         * carry no payload. */
    };
} iris_event;

/* ================================================================== */
/*  Host interception seam                                             */
/* ================================================================== */

/* The host's answer to one intercepted event. */
typedef enum iris_event_disposition : uint32_t {
    IRIS_EVENT_PASS = 0, /* deliver the (possibly rewritten) event   */
    IRIS_EVENT_DROP = 1, /* discard it; it never reaches the snapshot */
} iris_event_disposition;

/* Host interception callback (iris_app_opts.on_event, ADR-0107).
 *
 * Invoked once per reified event, in arrival order, synchronously on the
 * iris_app_run thread, before folding and outside the UI layer's frame.
 * The host may mutate `*ev` fields and return PASS to deliver the
 * rewritten event, or return DROP to discard it. For events flagged
 * IRIS_EVENT_F_UNDROPPABLE the host is still called but iris ignores a
 * DROP. `user` is the pointer passed in iris_app_opts.
 *
 * The callback must not re-enter iris_app_run. */
typedef iris_event_disposition (*iris_event_fn)(iris_event *ev, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_EVENT_H */
