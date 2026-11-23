/* a11y_util.h — pure, headless helpers for the AT-SPI bridge.
 *
 * This translation unit compiles UNCONDITIONALLY (no sd-bus dependency) so
 * the role / value logic can be unit-tested and reused by the D-Bus bridge
 * (a11y_atspi.c). Keeping the pure predicates out of the sd-bus-gated source
 * also means the stub build (no libsystemd) still has a coherent answer to
 * "which interfaces does this role support" if a future host needs it.
 *
 * It also owns the pieces of the AT-SPI wire contract that must stay
 * verifiable without a bus: the AtspiRole / AtspiState numeric constants
 * (values cross-checked against at-spi2-core's atspi-constants.h — getting
 * these wrong makes orca announce the wrong control type), the ui→AT-SPI
 * role/state mapping, and the per-frame tree diff that decides which
 * Event.Object signals the bridge emits.
 */
#ifndef OPTICS_WINDOW_A11Y_UTIL_H
#define OPTICS_WINDOW_A11Y_UTIL_H

#include <optics/gfx/math.h>
#include <optics/window/host.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Roles that expose org.a11y.atspi.Action — the AT can invoke them.
 * Buttons, checkboxes, radios, disclosure toggles, menu items, and links
 * all do something when activated; labels / panels / scroll areas do not. */
bool optics_window_a11y__supports_action(optics_window_role r);

/* Roles that expose org.a11y.atspi.Text — single- or multi-line text input. */
bool optics_window_a11y__supports_text(optics_window_role r);

/* Roles that expose org.a11y.atspi.Value — anything with a scalar range
 * (sliders; progress bars read back their completion through it). */
bool optics_window_a11y__supports_value(optics_window_role r);

/* The localized-independent action name AT-SPI clients announce for the
 * primary action of `r` ("click" / "toggle" / "press"), or NULL when the
 * role is not actionable. */
const char *optics_window_a11y__action_name(optics_window_role r);

/* Parse a ui semantic `value` string (a slider readout like "1.5" or
 * "42") into a double. Returns 0.0 for NULL / empty / non-numeric input. */
double optics_window_a11y__parse_value(const char *s);

/* Count UTF-8 code points (what AT-SPI Text offsets are measured in) in `s`.
 * Returns 0 for NULL. */
int optics_window_a11y__char_count_utf8(const char *s);

/* ------------------------------------------------------------------ */
/*  AT-SPI constants (subset of at-spi2-core atspi/atspi-constants.h)  */
/* ------------------------------------------------------------------ */

/* Numeric values MUST match AtspiRole exactly — clients compare them
 * numerically, so a wrong value announces the wrong control type. */
typedef enum optics_window_atspi_role : uint32_t {
    OPTICS_WINDOW_ATSPI_ROLE_CHECK_BOX = 7,
    OPTICS_WINDOW_ATSPI_ROLE_DIALOG = 16,
    OPTICS_WINDOW_ATSPI_ROLE_FRAME = 23, /* the application root         */
    OPTICS_WINDOW_ATSPI_ROLE_LABEL = 29,
    OPTICS_WINDOW_ATSPI_ROLE_MENU = 33,
    OPTICS_WINDOW_ATSPI_ROLE_MENU_ITEM = 35,
    OPTICS_WINDOW_ATSPI_ROLE_PANEL = 39,
    OPTICS_WINDOW_ATSPI_ROLE_PROGRESS_BAR = 42,
    OPTICS_WINDOW_ATSPI_ROLE_PUSH_BUTTON = 43,
    OPTICS_WINDOW_ATSPI_ROLE_RADIO_BUTTON = 44,
    OPTICS_WINDOW_ATSPI_ROLE_SCROLL_PANE = 49,
    OPTICS_WINDOW_ATSPI_ROLE_SLIDER = 51,
    OPTICS_WINDOW_ATSPI_ROLE_TABLE = 55,
    OPTICS_WINDOW_ATSPI_ROLE_TEXT = 61,
    OPTICS_WINDOW_ATSPI_ROLE_TOGGLE_BUTTON = 62,
    OPTICS_WINDOW_ATSPI_ROLE_UNKNOWN = 67,
    OPTICS_WINDOW_ATSPI_ROLE_ENTRY = 79,
    OPTICS_WINDOW_ATSPI_ROLE_LINK = 88,
    OPTICS_WINDOW_ATSPI_ROLE_TABLE_ROW = 90,
} optics_window_atspi_role;

/* AT-SPI State enum bit positions (AtspiStateType). State is two uint32
 * bitfields (64 bits) on the wire; bits 0-31 live in `lo`, 32-63 in `hi`. */
typedef enum optics_window_atspi_state : uint32_t {
    OPTICS_WINDOW_ATSPI_STATE_CHECKED = 4,
    OPTICS_WINDOW_ATSPI_STATE_ENABLED = 8,
    OPTICS_WINDOW_ATSPI_STATE_EXPANDED = 10,
    OPTICS_WINDOW_ATSPI_STATE_FOCUSABLE = 11,
    OPTICS_WINDOW_ATSPI_STATE_FOCUSED = 12,
    OPTICS_WINDOW_ATSPI_STATE_SELECTED = 23,
    OPTICS_WINDOW_ATSPI_STATE_SENSITIVE = 24,
    OPTICS_WINDOW_ATSPI_STATE_SHOWING = 25,
} optics_window_atspi_state;

/* Map ui role → AT-SPI role (OPTICS_WINDOW_ATSPI_ROLE_UNKNOWN when unmapped). */
optics_window_atspi_role optics_window_a11y__map_role(optics_window_role r);

/* The RoleName string clients display for an AT-SPI role ("push button", …). */
const char *optics_window_a11y__role_name(optics_window_atspi_role r);

/* Build the AT-SPI (lo, hi) state bitfield from ui's OPTICS_WINDOW_A11Y_* flags. */
void optics_window_a11y__state_bits(uint32_t flags, uint32_t *out_lo, uint32_t *out_hi);

/* ------------------------------------------------------------------ */
/*  Per-frame tree diff (which Event.Object signals to emit)           */
/* ------------------------------------------------------------------ */

/* One snapshot node. name/value are deep-copied out of ui's per-frame
 * arena (the pointers ui hands to the walk are only valid until the next
 * frame), truncated on a UTF-8 code-point boundary when overlong. */
#define OPTICS_WINDOW_A11Y__NODE_NAME_MAX 128
#define OPTICS_WINDOW_A11Y__NODE_VALUE_MAX 256

typedef struct optics_window_a11y__node {
    optics_window_node_id id;
    optics_window_node_id parent;
    optics_window_role role;
    char name[OPTICS_WINDOW_A11Y__NODE_NAME_MAX];
    char value[OPTICS_WINDOW_A11Y__NODE_VALUE_MAX];
    uint32_t flags;         /* OPTICS_WINDOW_A11Y_*                        */
    int32_t index;          /* index among same-parent siblings    */
    optics_gfx_rect bounds; /* solved bounds this frame (Component / BoundsChanged) */
} optics_window_a11y__node;

/* Copy one walked ui node into snapshot storage (deep string copies,
 * sibling index computed against the partially built `cur` array). */
void optics_window_a11y__node_fill(optics_window_a11y__node *dst, const optics_window_semantics *s,
                                   optics_window_node_id id, optics_window_node_id parent,
                                   const optics_window_a11y__node *cur, size_t n_cur);

/* ------------------------------------------------------------------ */
/*  Text-changed delta (Event.Object::TextChanged)                     */
/* ------------------------------------------------------------------ */

/* The edit between two frames of a text node's value, as a common
 * prefix/suffix split — the shape AT-SPI's TextChanged signal carries.
 * All offsets/lengths are in CODE POINTS (the AT-SPI Text convention);
 * the *_text spans point into the caller's strings (NOT NUL-terminated)
 * and stay valid as long as those strings do. A replace (both sides
 * non-empty) is meant to be emitted as delete-then-insert. */
typedef struct optics_window_a11y__text_delta {
    int32_t offset;           /* code-point offset of the change           */
    int32_t removed;          /* code points removed (0 = pure insert)     */
    int32_t inserted;         /* code points inserted (0 = pure delete)    */
    const char *removed_text; /* removed run inside `prev`          */
    size_t removed_bytes;
    const char *inserted_text; /* inserted run inside `cur`          */
    size_t inserted_bytes;
} optics_window_a11y__text_delta;

/* Diff two UTF-8 values. Returns false when they are identical (out
 * untouched). Both inputs must be valid UTF-8 (the bridge guarantees this
 * by construction); the split never lands inside a multi-byte sequence. */
bool optics_window_a11y__text_delta_of(const char *prev, const char *cur,
                                       optics_window_a11y__text_delta *out);

/* What changed between two frames. The bridge translates these into
 * org.a11y.atspi.Event.Object signals (a11y_atspi.c). Focus is deliberately
 * NOT diffed here: it is tracked by pointer (the focused id) so removals of
 * the focused node still produce a focused-off event. */
typedef enum optics_window_a11y__event_kind : uint32_t {
    OPTICS_WINDOW_A11Y__EV_ADD,       /* node appeared (id, parent, index set)      */
    OPTICS_WINDOW_A11Y__EV_REMOVE,    /* node vanished (id, parent, index = old)    */
    OPTICS_WINDOW_A11Y__EV_NAME,      /* accessible-name changed (name = new)       */
    OPTICS_WINDOW_A11Y__EV_VALUE,     /* accessible-value changed (value = new)     */
    OPTICS_WINDOW_A11Y__EV_ROLE,      /* accessible-role changed (role = new)       */
    OPTICS_WINDOW_A11Y__EV_STATE_ON,  /* state bit turned on  (state = AT-SPI bit)  */
    OPTICS_WINDOW_A11Y__EV_STATE_OFF, /* state bit turned off (state = AT-SPI bit)  */
    OPTICS_WINDOW_A11Y__EV_TEXT,      /* text edit on a TEXT* role (text = delta)   */
} optics_window_a11y__event_kind;

typedef struct optics_window_a11y__event {
    optics_window_a11y__event_kind kind;
    optics_window_node_id id;
    optics_window_node_id parent; /* add/remove: the parent the change is reported on */
    int32_t index;                /* add/remove: index in that parent's child list    */
    int32_t state;                /* STATE_ON/OFF: optics_window_atspi_state bit               */
    optics_window_role role;      /* ROLE: the new role                               */
    optics_window_a11y__text_delta text;       /* TEXT: the edit delta                   */
    const optics_window_a11y__node *node;      /* the current-frame node (NULL for REMOVE,
                                         which points at `prev_node` instead)   */
    const optics_window_a11y__node *prev_node; /* REMOVE: the old node            */
} optics_window_a11y__event;

/* Diff two frames. Writes up to `cap` events into `out` (removals first,
 * then additions, then per-node property/state changes — the order AT
 * clients apply them most defensively) and returns the event count, which
 * may exceed `cap` (callers size `out` generously: 2 * max_nodes + slack).
 * Only CHECKED / EXPANDED / SELECTED state bits are diffed; focus is the
 * caller's pointer-tracked concern (see above). */
size_t optics_window_a11y__diff(const optics_window_a11y__node *prev, size_t n_prev,
                                const optics_window_a11y__node *cur, size_t n_cur,
                                optics_window_a11y__event *out, size_t cap);

#endif /* OPTICS_WINDOW_A11Y_UTIL_H */
