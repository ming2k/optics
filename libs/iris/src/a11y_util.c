/* a11y_util.c — pure helpers for the AT-SPI bridge. See a11y_util.h. */

#include "a11y_util.h"

#include "platform_text.h"

#include <stdlib.h>
#include <string.h>

bool iris_a11y__supports_action(lens_role r) {
    switch (r) {
    case LENS_ROLE_BUTTON:
    case LENS_ROLE_CHECKBOX:
    case LENS_ROLE_RADIO:
    case LENS_ROLE_DISCLOSURE:
    case LENS_ROLE_MENU:
    case LENS_ROLE_MENUITEM:
    case LENS_ROLE_LINK:
        return true;
    case LENS_ROLE_LABEL:
    case LENS_ROLE_GROUP:
    case LENS_ROLE_SCROLLAREA:
    case LENS_ROLE_TEXTFIELD:
    case LENS_ROLE_TEXTAREA:
    case LENS_ROLE_SLIDER:
    case LENS_ROLE_DIALOG:
    case LENS_ROLE_PROGRESS:
    case LENS_ROLE_TABLE:
    case LENS_ROLE_ROW:
    case LENS_ROLE_NONE:
    default:
        return false;
    }
}

bool iris_a11y__supports_text(lens_role r) {
    return r == LENS_ROLE_TEXTFIELD || r == LENS_ROLE_TEXTAREA;
}

bool iris_a11y__supports_value(lens_role r) {
    return r == LENS_ROLE_SLIDER || r == LENS_ROLE_PROGRESS;
}

/* The single advertised action is "click" (ADR-0062): one activation verb
 * every actionable role shares; AT clients invoke it via DoAction(0). */
const char *iris_a11y__action_name(lens_role r) {
    return iris_a11y__supports_action(r) ? "click" : NULL;
}

double iris_a11y__parse_value(const char *s) {
    if (!s || !*s)
        return 0.0;
    /* strtod skips leading whitespace and parses as much as is valid; a
     * non-numeric readout (e.g. a label accidentally queried) yields 0.0
     * because end == s after the parse. */
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s)
        return 0.0;
    return v;
}

int iris_a11y__char_count_utf8(const char *s) {
    if (!s)
        return 0;
    /* AT-SPI Text offsets count Unicode code points, not bytes or UTF-16
     * units. A UTF-8 leading byte is any byte not in the 10xxxxxx
     * continuation range, so counting those gives the code-point count. */
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80)
            n++;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Role / state mapping                                               */
/* ------------------------------------------------------------------ */

iris_atspi_role iris_a11y__map_role(lens_role r) {
    switch (r) {
    case LENS_ROLE_LABEL:
        return IRIS_ATSPI_ROLE_LABEL;
    case LENS_ROLE_BUTTON:
        return IRIS_ATSPI_ROLE_PUSH_BUTTON;
    case LENS_ROLE_CHECKBOX:
        return IRIS_ATSPI_ROLE_CHECK_BOX;
    case LENS_ROLE_SLIDER:
        return IRIS_ATSPI_ROLE_SLIDER;
    case LENS_ROLE_DISCLOSURE:
        return IRIS_ATSPI_ROLE_TOGGLE_BUTTON;
    case LENS_ROLE_SCROLLAREA:
        return IRIS_ATSPI_ROLE_SCROLL_PANE;
    case LENS_ROLE_TEXTFIELD:
        return IRIS_ATSPI_ROLE_ENTRY;
    case LENS_ROLE_TEXTAREA:
        return IRIS_ATSPI_ROLE_TEXT;
    case LENS_ROLE_MENU:
        return IRIS_ATSPI_ROLE_MENU;
    case LENS_ROLE_RADIO:
        return IRIS_ATSPI_ROLE_RADIO_BUTTON;
    case LENS_ROLE_GROUP:
        return IRIS_ATSPI_ROLE_PANEL;
    case LENS_ROLE_DIALOG:
        return IRIS_ATSPI_ROLE_DIALOG;
    case LENS_ROLE_PROGRESS:
        return IRIS_ATSPI_ROLE_PROGRESS_BAR;
    case LENS_ROLE_TABLE:
        return IRIS_ATSPI_ROLE_TABLE;
    case LENS_ROLE_ROW:
        return IRIS_ATSPI_ROLE_TABLE_ROW;
    case LENS_ROLE_MENUITEM:
        return IRIS_ATSPI_ROLE_MENU_ITEM;
    case LENS_ROLE_LINK:
        return IRIS_ATSPI_ROLE_LINK;
    case LENS_ROLE_NONE:
    default:
        return IRIS_ATSPI_ROLE_UNKNOWN;
    }
}

const char *iris_a11y__role_name(iris_atspi_role r) {
    switch (r) {
    case IRIS_ATSPI_ROLE_PUSH_BUTTON:
        return "push button";
    case IRIS_ATSPI_ROLE_CHECK_BOX:
        return "check box";
    case IRIS_ATSPI_ROLE_RADIO_BUTTON:
        return "radio button";
    case IRIS_ATSPI_ROLE_SLIDER:
        return "slider";
    case IRIS_ATSPI_ROLE_LABEL:
        return "label";
    case IRIS_ATSPI_ROLE_PANEL:
        return "panel";
    case IRIS_ATSPI_ROLE_SCROLL_PANE:
        return "scroll pane";
    case IRIS_ATSPI_ROLE_ENTRY:
        return "entry";
    case IRIS_ATSPI_ROLE_TEXT:
        return "text";
    case IRIS_ATSPI_ROLE_TOGGLE_BUTTON:
        return "toggle button";
    case IRIS_ATSPI_ROLE_MENU:
        return "menu";
    case IRIS_ATSPI_ROLE_MENU_ITEM:
        return "menu item";
    case IRIS_ATSPI_ROLE_LINK:
        return "link";
    case IRIS_ATSPI_ROLE_DIALOG:
        return "dialog";
    case IRIS_ATSPI_ROLE_PROGRESS_BAR:
        return "progress bar";
    case IRIS_ATSPI_ROLE_TABLE:
        return "table";
    case IRIS_ATSPI_ROLE_TABLE_ROW:
        return "table row";
    case IRIS_ATSPI_ROLE_FRAME:
        return "frame";
    default:
        return "unknown";
    }
}

/* Pack a state bit into the (lo, hi) pair. */
static void state_set(uint32_t *lo, uint32_t *hi, int bit) {
    if (bit < 32)
        *lo |= (1u << bit);
    else
        *hi |= (1u << (bit - 32));
}

void iris_a11y__state_bits(uint32_t lens_flags, uint32_t *out_lo, uint32_t *out_hi) {
    uint32_t lo = 0, hi = 0;
    /* Every widget is focusable+enabled+sensitive+showing unless the lens
     * DISABLED flag is set. */
    if (!(lens_flags & LENS_A11Y_DISABLED)) {
        state_set(&lo, &hi, IRIS_ATSPI_STATE_FOCUSABLE);
        state_set(&lo, &hi, IRIS_ATSPI_STATE_ENABLED);
        state_set(&lo, &hi, IRIS_ATSPI_STATE_SENSITIVE);
    }
    state_set(&lo, &hi, IRIS_ATSPI_STATE_SHOWING);
    if (lens_flags & LENS_A11Y_FOCUSED)
        state_set(&lo, &hi, IRIS_ATSPI_STATE_FOCUSED);
    if (lens_flags & LENS_A11Y_CHECKED)
        state_set(&lo, &hi, IRIS_ATSPI_STATE_CHECKED);
    if (lens_flags & LENS_A11Y_EXPANDED)
        state_set(&lo, &hi, IRIS_ATSPI_STATE_EXPANDED);
    if (lens_flags & LENS_A11Y_SELECTED)
        state_set(&lo, &hi, IRIS_ATSPI_STATE_SELECTED);
    *out_lo = lo;
    *out_hi = hi;
}

/* ------------------------------------------------------------------ */
/*  Per-frame tree diff                                                */
/* ------------------------------------------------------------------ */

void iris_a11y__node_fill(iris_a11y__node *dst, const lens_semantics *s, lens_id id,
                          lens_id parent, const iris_a11y__node *cur, size_t n_cur) {
    dst->id = id;
    dst->parent = parent;
    dst->role = s->role;
    iris_utf8_copy(dst->name, sizeof dst->name, s->name);
    iris_utf8_copy(dst->value, sizeof dst->value, s->value);
    dst->flags = s->flags;
    /* Sibling index: count of already-walked nodes with the same parent.
     * The walk is pre-order, so the index is stable for the whole frame. */
    int32_t index = 0;
    for (size_t i = 0; i < n_cur; i++)
        if (cur[i].parent == parent)
            index++;
    dst->index = index;
}

static const iris_a11y__node *find_by_id(const iris_a11y__node *nodes, size_t n, lens_id id) {
    for (size_t i = 0; i < n; i++)
        if (nodes[i].id == id)
            return &nodes[i];
    return NULL;
}

/* Code points in a byte span of valid UTF-8 (same lead-byte counting as
 * iris_a11y__char_count_utf8, bounded by length instead of NUL). */
static int32_t span_codepoints(const char *s, size_t bytes) {
    int32_t n = 0;
    for (size_t i = 0; i < bytes; i++)
        if (((unsigned char)s[i] & 0xC0) != 0x80)
            n++;
    return n;
}

bool iris_a11y__text_delta_of(const char *prev, const char *cur, iris_a11y__text_delta *out) {
    if (!prev)
        prev = "";
    if (!cur)
        cur = "";
    size_t lp = strlen(prev), lc = strlen(cur);

    /* Longest common byte prefix, then back off to a code-point boundary
     * (byte-level equality can stop mid-sequence: "é" vs "è" share C3). */
    size_t p = 0;
    size_t pmax = lp < lc ? lp : lc;
    while (p < pmax && prev[p] == cur[p])
        p++;
    while (p > 0 && ((unsigned char)prev[p] & 0xC0) == 0x80)
        p--;

    /* Longest common byte suffix not overlapping the prefix, likewise
     * backed off to a boundary. The suffix bytes are identical in both
     * strings, so checking `prev` suffices. */
    size_t e = 0;
    size_t emax = (lp < lc ? lp : lc) - p;
    while (e < emax && prev[lp - 1 - e] == cur[lc - 1 - e])
        e++;
    while (e > 0 && ((unsigned char)prev[lp - e] & 0xC0) == 0x80)
        e--;

    size_t rb = lp - p - e; /* removed byte run  */
    size_t ib = lc - p - e; /* inserted byte run */
    if (rb == 0 && ib == 0)
        return false;

    out->offset = span_codepoints(prev, p);
    out->removed_text = prev + p;
    out->removed_bytes = rb;
    out->removed = span_codepoints(prev + p, rb);
    out->inserted_text = cur + p;
    out->inserted_bytes = ib;
    out->inserted = span_codepoints(cur + p, ib);
    return true;
}

size_t iris_a11y__diff(const iris_a11y__node *prev, size_t n_prev,
                       const iris_a11y__node *cur, size_t n_cur, iris_a11y__event *out,
                       size_t cap) {
    size_t n = 0;
#define PUSH(ev)                          \
    do {                                  \
        if (n < cap)                      \
            out[n] = (ev);                \
        n++;                              \
    } while (0)

    /* Removals first (clients drop the subtree before additions land). */
    for (size_t i = 0; i < n_prev; i++) {
        const iris_a11y__node *p = &prev[i];
        if (!find_by_id(cur, n_cur, p->id)) {
            PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_REMOVE,
                                     .id = p->id,
                                     .parent = p->parent,
                                     .index = p->index,
                                     .prev_node = p}));
        }
    }

    /* Additions. */
    for (size_t i = 0; i < n_cur; i++) {
        const iris_a11y__node *c = &cur[i];
        if (!find_by_id(prev, n_prev, c->id)) {
            PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_ADD,
                                     .id = c->id,
                                     .parent = c->parent,
                                     .index = c->index,
                                     .node = c}));
        }
    }

    /* In-place changes for nodes present in both frames. */
    for (size_t i = 0; i < n_cur; i++) {
        const iris_a11y__node *c = &cur[i];
        const iris_a11y__node *p = find_by_id(prev, n_prev, c->id);
        if (!p)
            continue;
        if (c->role != p->role) {
            PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_ROLE,
                                     .id = c->id,
                                     .role = c->role,
                                     .node = c}));
        }
        if (strcmp(c->name, p->name) != 0) {
            PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_NAME, .id = c->id, .node = c}));
        }
        if (iris_a11y__supports_value(c->role) && strcmp(c->value, p->value) != 0) {
            PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_VALUE, .id = c->id, .node = c}));
        }
        /* Text nodes (ADR-0062): emit the edit delta for TextChanged; the
         * common prefix/suffix split gives orca's typing echo exactly what
         * it listens for. */
        if (iris_a11y__supports_text(c->role) && strcmp(c->value, p->value) != 0) {
            iris_a11y__text_delta d;
            if (iris_a11y__text_delta_of(p->value, c->value, &d)) {
                PUSH(((iris_a11y__event){.kind = IRIS_A11Y__EV_TEXT,
                                         .id = c->id,
                                         .text = d,
                                         .node = c}));
            }
        }
        static const struct {
            uint32_t lens_flag;
            int32_t atspi_state;
        } tracked[] = {
            {LENS_A11Y_CHECKED, IRIS_ATSPI_STATE_CHECKED},
            {LENS_A11Y_EXPANDED, IRIS_ATSPI_STATE_EXPANDED},
            {LENS_A11Y_SELECTED, IRIS_ATSPI_STATE_SELECTED},
        };
        for (size_t t = 0; t < sizeof tracked / sizeof tracked[0]; t++) {
            bool was = (p->flags & tracked[t].lens_flag) != 0;
            bool is = (c->flags & tracked[t].lens_flag) != 0;
            if (was != is) {
                PUSH(((iris_a11y__event){.kind = is ? IRIS_A11Y__EV_STATE_ON
                                                    : IRIS_A11Y__EV_STATE_OFF,
                                         .id = c->id,
                                         .state = tracked[t].atspi_state,
                                         .node = c}));
            }
        }
    }
#undef PUSH
    return n;
}
