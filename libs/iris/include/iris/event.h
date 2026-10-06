/*
 * iris/event.h — the reified, platform-neutral input event (ADR-0107).
 *
 * iris owns the platform event loop and is the sole event pump of the
 * stack (ADR-0043, ADR-0084). Native events — Wayland wl_pointer /
 * wl_keyboard / text-input-v3, Win32 WM_*, Cocoa NSEvent — are reified
 * into one `iris_event` per native event, in arrival order. The host may
 * observe, drop, or rewrite each event at the single pre-fold seam
 * (`iris_event_fn` in iris/app.h) before iris folds the survivors into the
 * frame's `lens_input`.
 *
 * The pipeline is deliberately one-directional:
 *
 *     native → [reify] → iris_event → [intercept: on_event] → [accumulate]
 *            → [fold into lens_input] → [lens consumes] → build(ui, in)
 *
 * Rules this type upholds (ADR-0107):
 *   - Platform-neutral. No native handles, pointers, or compositor
 *     serials. Platform-specific capability travels through the capability
 *     APIs (iris/capability.h, ADR-0099), never through this struct.
 *   - One vocabulary. Key ids and modifier masks are the LENS_KEY_* /
 *     LENS_MOD_* values from <lens/lens.h>; pointer buttons are
 *     LENS_MOUSE_LEFT / _RIGHT / _MIDDLE. There are no parallel enums.
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

#include <lens/lens.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

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

    IRIS_EVENT_KEY,            /* payload: keyboard                      */
    IRIS_EVENT_MODIFIERS,      /* payload: modifiers (level state)       */
    IRIS_EVENT_TEXT_COMMIT,    /* payload: text (IME / layout commit)    */
    IRIS_EVENT_PREEDIT,        /* payload: preedit                       */
    IRIS_EVENT_IME_DELETE,     /* payload: ime_delete (delete_surrounding)*/

    IRIS_EVENT_TABLET,         /* payload: tablet (pen: pressure, tilt)  */
    IRIS_EVENT_DND,            /* payload: dnd (drag-and-drop)           */

    IRIS_EVENT_RESIZE,         /* payload: resize                        */
    IRIS_EVENT_SCALE,          /* payload: scale                         */
    IRIS_EVENT_FOCUS,          /* payload: focus                         */
    IRIS_EVENT_CLOSE,          /* payload: none                          */
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
            float x, y;    /* logical surface-local pixels     */
            int button;    /* LENS_MOUSE_LEFT / _RIGHT / _MIDDLE */
            bool pressed;  /* true = down edge, false = up edge  */
        } button;

        struct {
            float dx, dy;    /* delta; units per `continuous`   */
            bool continuous; /* true = logical px (touchpad)    */
                             /* false = wheel notches           */
        } scroll;

        struct {
            int key;       /* LENS_KEY_* or printable ASCII      */
            bool pressed;  /* down edge / up edge                */
            bool repeat;   /* synthesised auto-repeat press      */
            uint32_t mods; /* LENS_MOD_* level state at the edge */
        } keyboard;

        struct {
            uint32_t mods; /* LENS_MOD_* level state */
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
            float x, y;      /* logical surface-local pixels */
            float pressure;   /* normalised 0..1             */
            float tilt_x, tilt_y; /* normalised -1..1        */
            bool eraser;      /* eraser end of the tool      */
            bool barrel;      /* barrel button pressed       */
            bool proximity;   /* tool in range (not necessarily down) */
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
 * iris_app_run thread, before folding, and outside any lens_begin/end
 * pair. The host may mutate `*ev` fields and return PASS to deliver the
 * rewritten event, or return DROP to discard it. For events flagged
 * IRIS_EVENT_F_UNDROPPABLE the host is still called but iris ignores a
 * DROP. `user` is the pointer passed in iris_app_opts.
 *
 * The callback runs outside a lens_begin/end pair, so it must not call
 * lens APIs or re-enter iris_app_run. */
typedef iris_event_disposition (*iris_event_fn)(iris_event *ev, void *user);

#ifdef __cplusplus
}
#endif

#endif /* IRIS_EVENT_H */
