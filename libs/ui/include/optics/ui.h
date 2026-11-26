/*
 * ui/ui.h — immediate-mode façade over a retained-mode core.
 *
 * One library (liboptics_ui), one umbrella header. You write immediate-mode
 * code each frame; ui reconciles it against a retained tree that owns
 * layout, interaction, animation, and the draw list.
 *
 * Design contract (see docs/adr):
 *   - Public symbols are `optics_ui_*`; library internals use an
 *     `optics_ui__*` prefix and are not exported (ADR-0031, ADR-0170).
 *   - Every widget call computes a stable optics_ui_id from an id stack; the
 *     id keys the retained store (ADR-0026, ADR-0027).
 *   - ui draws only through <optics/gfx/canvas.h> (ADR-0025).
 *   - Per frame:
 *       optics_ui_begin(ui, &input) ->
 *       build (optics_ui_row_begin / optics_ui_button / ...) ->
 *       optics_ui_end(ui)            (reconcile, layout, interaction) ->
 *       optics_ui_snapshot_create(...) -> optics_ui_snapshot_submit(...) ->
 * optics_ui_snapshot_activate(...)
 */

#ifndef OPTICS_UI_H
#define OPTICS_UI_H

/* Self-contained: ui.h uses bool, uint32_t, size_t directly and must
 * not rely on gfx headers happening to provide them first. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <optics/gfx/canvas.h>
#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <optics/ui/export.h> /* OPTICS_UI_API — single source of truth */
#include <optics/ui/icon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */
/* OPTICS_UI_API is defined in <optics/ui/export.h>, included above. Do not define */
/* it here: two spellings of one export macro drift apart.              */

#define OPTICS_UI_VERSION_MAJOR 0
#define OPTICS_UI_VERSION_MINOR 0
#define OPTICS_UI_VERSION_PATCH 87

/* Stringify helpers used by optics_ui_version_string(); OPTICS_UI_STRINGIFY_ adds the
 * indirection level required for macro-expansion of literal tokens. */
#define OPTICS_UI_STRINGIFY_(x) #x
#define OPTICS_UI_STRINGIFY(x) OPTICS_UI_STRINGIFY_(x)

/* Packed integer version, monotonic — identical layout to
 * OPTICS_GFX_VERSION_NUMBER (major in bits 16..23, minor 8..15, patch 0..7).
 * The whole stack shares one versioning scheme so a consumer can check
 * every library it loads the same way. */
#define OPTICS_UI_VERSION_NUMBER                                                                   \
    (((uint32_t)OPTICS_UI_VERSION_MAJOR << 16) | ((uint32_t)OPTICS_UI_VERSION_MINOR << 8) |        \
     (uint32_t)OPTICS_UI_VERSION_PATCH)

OPTICS_UI_API void optics_ui_version(int *major, int *minor, int *patch);
OPTICS_UI_API uint32_t optics_ui_version_number(void);
/* True when the linked library can stand in for the one compiled
 * against: same major, and its (minor, patch) is >= the requested one.
 * ABI is major-locked; API additions ride minors. */
OPTICS_UI_API bool optics_ui_version_check(int major, int minor, int patch);
OPTICS_UI_API const char *optics_ui_version_string(void);

/* ================================================================== */
/*  Core types                                                        */
/* ================================================================== */

typedef struct optics_ui optics_ui;           /* opaque context (the retained core) */
typedef struct optics_ui_node optics_ui_node; /* opaque retained node slot          */
typedef struct optics_ui_font optics_ui_font; /* opaque font handle (text seam)     */

typedef uint64_t optics_ui_id; /* stable widget identity; 0 == none  */

typedef enum optics_ui_axis : uint32_t { OPTICS_UI_ROW = 0, OPTICS_UI_COLUMN = 1 } optics_ui_axis;
typedef enum optics_ui_align : uint32_t {
    OPTICS_UI_START = 0,
    OPTICS_UI_CENTER = 1,
    OPTICS_UI_END = 2,
    OPTICS_UI_STRETCH = 3
} optics_ui_align;

typedef enum optics_ui_mouse_button : uint32_t {
    OPTICS_UI_MOUSE_LEFT = 0,
    OPTICS_UI_MOUSE_RIGHT = 1,
    OPTICS_UI_MOUSE_MIDDLE = 2,
} optics_ui_mouse_button;
#define OPTICS_UI_MOUSE_COUNT 3

typedef enum optics_ui_node_phase : uint32_t {
    OPTICS_UI_NODE_ENTERING = 0, /* first frame this id was seen        */
    OPTICS_UI_NODE_STABLE = 1,   /* seen this frame and last frame      */
    OPTICS_UI_NODE_LEAVING = 2,  /* not seen this frame; in grace window */
} optics_ui_node_phase;

/* ================================================================== */
/*  Input snapshot (ADR-0029)                                         */
/* ================================================================== */

#define OPTICS_UI_INPUT_MAX_KEYS 16

/* Portable key sentinels. The application maps platform keycodes to
 * these when filling optics_ui_key_event.key. Codes outside this range pass
 * through untouched (custom shortcuts).
 *
 * Printable ASCII (0x20-0x7E) arrives as key events carrying the ASCII
 * codepoint itself (unshifted: 'a' not 'A') — the window backends
 * guarantee this on every platform, so character shortcuts ('z', ',',
 * space) can be matched straight off optics_ui_key_event.key. Non-ASCII
 * printable input never arrives as a key event; it only enters
 * text_utf8 through the IME/layout commit path. */
#define OPTICS_UI_KEY_ESCAPE 256
#define OPTICS_UI_KEY_TAB 258
#define OPTICS_UI_KEY_RETURN 259
#define OPTICS_UI_KEY_BACKSPACE 260
#define OPTICS_UI_KEY_DELETE 261
#define OPTICS_UI_KEY_LEFT 262
#define OPTICS_UI_KEY_RIGHT 263
#define OPTICS_UI_KEY_HOME 264
#define OPTICS_UI_KEY_END 265
#define OPTICS_UI_KEY_UP 266
#define OPTICS_UI_KEY_DOWN 267
#define OPTICS_UI_KEY_F1 280
#define OPTICS_UI_KEY_F2 281
#define OPTICS_UI_KEY_F3 282
#define OPTICS_UI_KEY_F4 283
#define OPTICS_UI_KEY_F5 284
#define OPTICS_UI_KEY_F6 285
#define OPTICS_UI_KEY_F7 286
#define OPTICS_UI_KEY_F8 287
#define OPTICS_UI_KEY_F9 288
#define OPTICS_UI_KEY_F10 289
#define OPTICS_UI_KEY_F11 290
#define OPTICS_UI_KEY_F12 291

/* Modifier masks for optics_ui_input.mods. */
#define OPTICS_UI_MOD_SHIFT (1u << 0)
#define OPTICS_UI_MOD_CTRL (1u << 1)
#define OPTICS_UI_MOD_ALT (1u << 2)
#define OPTICS_UI_MOD_SUPER (1u << 3)

typedef struct optics_ui_key_event {
    int key;      /* platform keycode */
    bool pressed; /* down edge if true, up edge if false */
    bool repeat;
} optics_ui_key_event;

#define OPTICS_UI_PREEDIT_MAX 256

typedef struct optics_ui_input {
    uint32_t size; /* sizeof(optics_ui_input); 0 = trust
                      full struct (forward-compat
                      guard, ADR-0036) */

    optics_gfx_point cursor; /* UI-space pixels */
    bool mouse_down[OPTICS_UI_MOUSE_COUNT];
    bool mouse_pressed[OPTICS_UI_MOUSE_COUNT];
    bool mouse_released[OPTICS_UI_MOUSE_COUNT];
    float scroll_x, scroll_y; /* wheel-step deltas */

    uint32_t mods;       /* modifier bitmask */
    char text_utf8[256]; /* committed text this frame; sized for
                            full-sentence IME conversion, not just
                            key-at-a-time input */

    optics_ui_key_event keys[OPTICS_UI_INPUT_MAX_KEYS];
    uint32_t key_count;

    optics_gfx_point display_size; /* layout root extent */
    float dt_seconds;              /* frame delta, drives animation */

    /* IME composition in progress this frame; empty when none (ADR-0036).
     * The committed result still arrives through `text_utf8`. */
    char preedit_utf8[OPTICS_UI_PREEDIT_MAX];
    uint32_t preedit_cursor; /* caret byte-offset in preedit */
    uint32_t preedit_sel_lo; /* active clause, byte range    */
    uint32_t preedit_sel_hi;

    /* Precise surface-local pixel deltas from touchpads or other continuous
     * sources. Kept separate from wheel steps so widgets do not multiply
     * finger motion by their line-scroll factor. Appended for size-guarded
     * ABI compatibility with callers built against older headers. */
    float scroll_pixels_x, scroll_pixels_y;

    /* IME `delete_surrounding_text` request (ADR: text-input-v3 full).
     * Set by the host when the IME asks the application to remove bytes
     * immediately before/after the current caret; textfield/textarea
     * consume it during their IME update. Both are byte counts in the
     * UTF-8 buffer the host most recently reported via text_utf8 /
     * preedit_utf8; zero means no deletion requested this frame. */
    uint32_t ime_delete_before;
    uint32_t ime_delete_after;

    /* Tablet / pen state this frame (size-guarded append; hosts built
     * against older headers keep working — ui reads these only when
     * `size` covers them). `pen_active` is true while a tool is in
     * proximity AND down; `pen_pressure` is normalised 0..1 (hosts should
     * pass 1.0 for tools without pressure so opacity math degrades to
     * full). `pen_tool` distinguishes eraser-end from tip. Mouse input
     * arrives exactly as before; a frame can carry both (a user mixing
     * devices), and widgets that only read the mouse fields are
     * unaffected. */
    bool pen_active;
    float pen_pressure;
    bool pen_eraser;
} optics_ui_input;

/* Host clipboard interface (ADR-0036). Supplied in optics_ui_desc; optional.
 * Paste is asynchronous (matches Wayland wl_data_device): a request is
 * answered later by the host calling optics_ui_paste. */
typedef struct optics_ui_clipboard {
    void (*request_text)(void *user); /* -> optics_ui_paste */
    void (*set_text)(const char *utf8, size_t len, void *user);
    void *user;
} optics_ui_clipboard;

/* Host Drag-and-Drop interface (ADR-0086). Supplied in optics_ui_desc; optional. */
typedef struct optics_ui_dnd_host {
    int (*start_drag)(const char *text, size_t len, uint32_t actions, void *user);
    void *user;
} optics_ui_dnd_host;

/* ================================================================== */
/*  Interaction result (ADR-0029; state bits ADR-0058)                */
/* ================================================================== */

/* Widget state bits, OR'd into optics_ui_response.state. The interaction core
 * (optics_ui__interact) produces HOVERED/PRESSED/FOCUSED/FOCUS_VISIBLE/DISABLED
 * for every widget; each widget ORs in the bits only it can know —
 * SELECTED from a selection argument, ACTIVE from a toggle value, DRAGGED
 * while a captured pointer drives a value. FOCUS_VISIBLE is FOCUSED via
 * keyboard navigation (Tab traversal); a pointer press focuses without it,
 * so chrome can show a focus ring only for keyboard users. */
typedef enum optics_ui_widget_state : uint32_t {
    OPTICS_UI_STATE_NONE = 0,
    OPTICS_UI_STATE_HOVERED = 1u << 0,       /* cursor over the widget          */
    OPTICS_UI_STATE_PRESSED = 1u << 1,       /* held: captured by the pointer   */
    OPTICS_UI_STATE_FOCUSED = 1u << 2,       /* holds keyboard focus            */
    OPTICS_UI_STATE_FOCUS_VISIBLE = 1u << 3, /* focused via keyboard navigation */
    OPTICS_UI_STATE_DISABLED = 1u << 4,      /* non-interactive                 */
    OPTICS_UI_STATE_SELECTED = 1u << 5,      /* picked out of a set (selectable) */
    OPTICS_UI_STATE_ACTIVE = 1u << 6,        /* on-state of a toggle            */
    OPTICS_UI_STATE_DRAGGED = 1u << 7,       /* captured pointer drives a value */
    OPTICS_UI_STATE_DROP_TARGET = 1u << 8,   /* cursor or drop over target zone */
} optics_ui_widget_state;

typedef struct optics_ui_response {
    optics_ui_id id;
    optics_gfx_rect rect; /* last frame's final_rect (what was hit-tested) */
    bool hovered;
    bool pressed;        /* left button held this frame */
    bool clicked;        /* left press + release inside */
    bool right_clicked;  /* right press + release inside */
    bool middle_clicked; /* middle press + release inside */
    bool changed;        /* value mutated this frame */
    bool focused;
    bool submitted; /* textedit: Return pressed on single-line input */
    bool tabbed;    /* textedit: Tab pressed when accept_tab enabled */
    uint32_t state; /* optics_ui_widget_state bits (ADR-0058) */
} optics_ui_response;

/* Semantic cursor requested by the hovered ui widget. ui only reports
 * intent; the windowing host remains responsible for mapping it to the
 * platform cursor API once per frame. */
typedef enum optics_ui_cursor_hint : uint32_t {
    OPTICS_UI_CURSOR_DEFAULT = 0,
    OPTICS_UI_CURSOR_POINTER,
    OPTICS_UI_CURSOR_TEXT,
    OPTICS_UI_CURSOR_RESIZE_EW,
    OPTICS_UI_CURSOR_RESIZE_NS,
} optics_ui_cursor_hint;

/* ================================================================== */
/*  Accessibility semantics (ADR-0035)                                */
/* ================================================================== */

typedef enum optics_ui_role : uint32_t {
    OPTICS_UI_ROLE_NONE = 0, /* decorative; not surfaced to assistive tech */
    OPTICS_UI_ROLE_GROUP,    /* a semantic grouping container              */
    OPTICS_UI_ROLE_LABEL,
    OPTICS_UI_ROLE_BUTTON,
    OPTICS_UI_ROLE_CHECKBOX,
    OPTICS_UI_ROLE_SLIDER,
    OPTICS_UI_ROLE_DISCLOSURE, /* collapsing header                         */
    OPTICS_UI_ROLE_SCROLLAREA,
    OPTICS_UI_ROLE_TEXTFIELD, /* reserved: text input                      */
    OPTICS_UI_ROLE_TEXTAREA,  /* multi-line text input                   */
    OPTICS_UI_ROLE_MENU,      /* popup menu (ADR-0040)                     */
    OPTICS_UI_ROLE_RADIO,
    OPTICS_UI_ROLE_DIALOG,   /* modal dialog window (ADR-0039)            */
    OPTICS_UI_ROLE_PROGRESS, /* progress indicator; read-only value, NOT a slider */
    OPTICS_UI_ROLE_TABLE,    /* grid container; rows/cells nest beneath   */
    OPTICS_UI_ROLE_ROW,      /* a row inside a TABLE                      */
    OPTICS_UI_ROLE_MENUITEM, /* item inside a MENU (distinct from BUTTON) */
    OPTICS_UI_ROLE_LINK,     /* inline navigation action (distinct from BUTTON) */
} optics_ui_role;

/* State bits for optics_ui_semantics.flags. */
enum {
    OPTICS_UI_A11Y_FOCUSED = 1u << 0,
    OPTICS_UI_A11Y_DISABLED = 1u << 1,
    OPTICS_UI_A11Y_CHECKED = 1u << 2,
    OPTICS_UI_A11Y_EXPANDED = 1u << 3,
    OPTICS_UI_A11Y_READONLY = 1u << 4,
    OPTICS_UI_A11Y_SELECTED = 1u << 5, /* selected row/tab/item            */
};

typedef struct optics_ui_semantics {
    optics_ui_role role;
    const char *name;  /* accessible name; arena-owned for the frame */
    const char *value; /* slider readout, field contents, ...        */
    uint32_t flags;
} optics_ui_semantics;

/* Override or enrich the semantics of the most recently built widget —
 * for icon-only controls whose visible label is not the accessible name.
 * Zeroed fields are left at the widget's default; `flags` are OR'd on. */
typedef struct optics_ui_a11y_desc {
    optics_ui_role role; /* OPTICS_UI_ROLE_NONE = keep the widget default */
    const char *name;    /* NULL = keep                            */
    const char *value;   /* NULL = keep                            */
    uint32_t flags;      /* added to the existing state bits        */
} optics_ui_a11y_desc;

OPTICS_UI_API void optics_ui_a11y(optics_ui *ui, const optics_ui_a11y_desc *desc);

/* Read-only walk of the activated snapshot. Failed or skipped presentation
 * leaves its geometry and semantic content unchanged. Visits every node carrying non-decorative
 * semantics in pre-order with its solved bounds and its nearest semantic ancestor. The platform
 * AT-SPI bridge (or a test) consumes it; ui calls no assistive API itself (same host separation
 * as input, ADR-0029). */
typedef void (*optics_ui_a11y_visit_fn)(const optics_ui_semantics *s, optics_gfx_rect bounds,
                                        optics_ui_id id, optics_ui_id parent, void *user);
OPTICS_UI_API void optics_ui_accessibility_walk(const optics_ui *ui, optics_ui_a11y_visit_fn visit,
                                                void *user);

/* Request activation of a widget by id (ADR-0062) — the write direction of
 * the a11y seam. The host's AT bridge (window's AT-SPI Action.DoAction)
 * calls this when an assistive client activates a control; ui records a
 * single pending activation id and the next frame's build reports
 * `clicked` for that node through the normal interaction path
 * (optics_ui__interact), so every focusable widget is activatable without
 * per-widget code.
 *
 * Semantics: the request fires once (single-shot). It is respected only
 * for a node that is focusable and not disabled when built; pointer
 * occlusion does not block it (AT users navigate the semantic tree, not
 * pixels). An unconsumed request is dropped at frame end — a widget that
 * vanished or is disabled simply never fires. Focus moves to the
 * activated node. Threading is the same as optics_ui_paste: the host's main
 * thread, outside optics_ui_begin/end is the intended window, though a call
 * mid-build takes effect for widgets built after it. */
OPTICS_UI_API void optics_ui_a11y_activate(optics_ui *ui, optics_ui_id id);

/* ================================================================== */
/*  Theme tokens (ADR / reference)                                    */
/* ================================================================== */

typedef struct optics_ui_theme {
    uint32_t size; /* sizeof(optics_ui_theme); forward-compat guard */

    optics_gfx_color color_bg;
    optics_gfx_color color_fg;
    optics_gfx_color color_accent;
    optics_gfx_color color_border;
    optics_gfx_color color_hover;
    optics_gfx_color color_active; /* pressed and selected surface fill */
    optics_gfx_color color_disabled;
    optics_gfx_color color_error;

    /* Semantic surface tokens (ADR-0105) */
    optics_gfx_color color_surface;          /* card / container resting surface */
    optics_gfx_color color_surface_sunken;   /* input field / segmented track sunken surface */
    optics_gfx_color color_surface_elevated; /* popup / tooltip / menu elevated surface */

    float padding;
    float gap;
    float corner_radius;
    float border_width;

    optics_ui_font *font;
    float font_size;
    float icon_size; /* Default glyph/icon box size. 0 = fall back to font_size. */

    /* Semantic text sizes (ADR-0034 extension). 0 = fall back to font_size. */
    float font_size_title;
    float font_size_h1;
    float font_size_h2;
    float font_size_h3;

    /* Font weight tokens. 0 = fall back to 400/700. */
    float font_weight;
    float font_weight_bold;

    /* Scrollbar styling for scroll areas. The thumb is drawn flush against
     * the right edge of a scroll container; `scrollbar_width` is also
     * reserved on the content's cross axis so nothing paints underneath
     * the thumb. `scrollbar_radius` is the pill corner roundness (use
     * width*0.5 for a fully rounded pill). `scrollbar_min_thumb_h` keeps
     * very long lists grabbable. The three thumb colours are the rest /
     * hover / drag states; `color_scrollbar_track` fills the full viewport
     * height — set its alpha to 0 to hide the track entirely. Defaults are
     * applied via theme normalisation. */
    float scrollbar_width;
    float scrollbar_radius;
    float scrollbar_min_thumb_h;
    optics_gfx_color color_scrollbar_track;
    optics_gfx_color color_scrollbar_thumb;
    optics_gfx_color color_scrollbar_thumb_hover;
    optics_gfx_color color_scrollbar_thumb_active;

    /* Slider styling. Non-positive geometry tokens fall back to 6 logical px
     * for the track and 14 logical px for the knob. Zero-valued colour tokens
     * inherit color_border / color_accent / color_fg during theme
     * normalisation. Keeping the knob separate from the fill lets value
     * controls use a high-contrast handle without losing the application's
     * accent on the filled range. */
    float slider_track_thickness;
    float slider_knob_size;
    optics_gfx_color color_slider_track;
    optics_gfx_color color_slider_fill;
    optics_gfx_color color_slider_knob;
} optics_ui_theme;

OPTICS_UI_API optics_ui_theme optics_ui_theme_default(void);
OPTICS_UI_API optics_ui_theme optics_ui_theme_dark(void);

/* Classic theme generator: provides a 1px wireframe border baseline (border_width = 1.0f)
 * for applications desiring traditional desktop control outlines. */
OPTICS_UI_API optics_ui_theme optics_ui_theme_classic(bool dark);

/* ================================================================== */
/*  Style cascade (ADR-0058, amended by ADR-0061)                     */
/*                                                                    */
/*  A optics_ui_style is a sparse set of style ATOMS. Every field is       */
/*  optional: the `fields` mask names the fields that carry values.   */
/*  The mask doubles as the forward-compat guard: a caller compiled   */
/*  against an older header never sets bits it does not know, and the */
/*  library never reads fields whose bit is clear.                    */
/*                                                                    */
/*  Resolution is one fixed per-field cascade (ADR-0061):             */
/*      per-call style (optics_ui_box.style)                               */
/*        > nearest enclosing scope (optics_ui_push_style)                 */
/*        > theme                                                     */
/*  Unset fields fall through each layer; the resolver's derivation   */
/*  order (hover/pressed derivation, disabled dim) is unchanged and   */
/*  remains the only adjustment site. With nothing set anywhere the   */
/*  output is the verbatim theme, so default rendering is unchanged.  */
/* ================================================================== */

/* Field tags for optics_ui_style.fields — one bit per optional field. */
typedef enum optics_ui_style_field : uint32_t {
    OPTICS_UI_STYLE_BG = 1u << 0,         /* resting surface           */
    OPTICS_UI_STYLE_BG_HOVER = 1u << 1,   /* hovered surface           */
    OPTICS_UI_STYLE_BG_PRESSED = 1u << 2, /* pressed/selected surface  */
    OPTICS_UI_STYLE_FG = 1u << 3,         /* foreground (text, glyphs) */
    OPTICS_UI_STYLE_BORDER = 1u << 4,     /* border stroke             */
    OPTICS_UI_STYLE_ACCENT = 1u << 5,     /* accent (emphasis colour)    */
    OPTICS_UI_STYLE_CORNER_RADIUS = 1u << 6,
    OPTICS_UI_STYLE_BORDER_WIDTH = 1u << 7,
    OPTICS_UI_STYLE_PADDING = 1u << 8,
    OPTICS_UI_STYLE_GAP = 1u << 9,
    OPTICS_UI_STYLE_FONT_SIZE = 1u << 10,
    OPTICS_UI_STYLE_ICON_SIZE = 1u << 11,     /* glyph/icon box size        */
    OPTICS_UI_STYLE_OUTLINE_COLOR = 1u << 12, /* foreground contour colour  */
    OPTICS_UI_STYLE_OUTLINE_WIDTH = 1u << 13, /* contour radius, logical px */
} optics_ui_style_field;

typedef struct optics_ui_style {
    uint32_t fields; /* OPTICS_UI_STYLE_* mask: which fields below are set */
    optics_gfx_color bg;
    optics_gfx_color bg_hover;
    optics_gfx_color bg_pressed;
    optics_gfx_color fg;
    optics_gfx_color border;
    optics_gfx_color accent;
    float corner_radius;
    float border_width;
    float padding;
    float gap;
    float font_size;
    float icon_size;
    /* Opt-in contour behind foreground content (text, glyphs, images)
     * floating over imagery or translucent backdrops. State-independent
     * decoration atoms: the resolver neither derives nor dims them, and
     * no theme token exists — unset means none. */
    optics_gfx_color outline_color;
    float outline_width;
} optics_ui_style;

/* Empty style: nothing set, everything resolves to the theme. */
#define OPTICS_UI_STYLE_INIT {.fields = 0}

static inline optics_ui_style optics_ui_style_init(void) {
    return (optics_ui_style)OPTICS_UI_STYLE_INIT;
}

/* The output of style resolution (ADR-0058/0061): optics_ui_style with every
 * slot concrete — the per-call > scope > theme cascade, hover/pressed
 * derivation, and the disabled dim have already been applied in the
 * resolver's documented order, so a consumer reads slots directly.
 * `disabled` is the designed disabled-surface colour; optics_ui_style has no
 * instance slot for it, so it is always the theme token. The outline slots
 * resolve to transparent/0 when unset (no theme tokens exist for them).
 * Skins (ADR-0059) receive this in the widget record. */
typedef struct optics_ui_style_resolved {
    optics_gfx_color bg;
    optics_gfx_color bg_hover;
    optics_gfx_color bg_pressed;
    optics_gfx_color fg;
    optics_gfx_color border;
    optics_gfx_color accent;
    optics_gfx_color disabled;
    float corner_radius;
    float border_width;
    float padding;
    float gap;
    float font_size;
    float icon_size;
    optics_gfx_color outline_color;
    float outline_width;
} optics_ui_style_resolved;

/* Scoped style stack (ADR-0061 item 4): every widget declared between
 * optics_ui_push_style and optics_ui_pop_style resolves its unset style atoms
 * against the merged scope — the primitive from which callers build their
 * own design-system scopes ("danger", "sidebar"). This is a strictly
 * nested scope, NOT a floating "applies to the next widget" modifier (the
 * ordering hazard optics_ui_box's documentation rejects): it affects exactly
 * the widgets declared between push and pop, including the terse forms,
 * and optics_ui_begin resets the stack every frame so a forgotten pop cannot
 * leak across frames. Overflow of the scope stack flags optics_ui_overflowed. */
OPTICS_UI_API void optics_ui_push_style(optics_ui *ui, optics_ui_style style);
OPTICS_UI_API void optics_ui_pop_style(optics_ui *ui);

/* ================================================================== */
/*  Text seam (ADR-0033)                                              */
/* ================================================================== */

/* Typeface family for subsequently built widgets. Values mirror
 * glyph_family so the seam can cast between them. OPTICS_UI_TEXT_FAMILY_DEFAULT
 * keeps the engine's default (sans-serif). Like optics_ui_set_theme, this is a
 * context switch read at widget-build time: set it, build the widgets that
 * need another voice (e.g. a serif display title), then set it back. */
typedef enum optics_ui_text_family : uint32_t {
    OPTICS_UI_TEXT_FAMILY_DEFAULT = 0,
    OPTICS_UI_TEXT_FAMILY_SANS = 1,
    OPTICS_UI_TEXT_FAMILY_SERIF = 2,
    OPTICS_UI_TEXT_FAMILY_MONO = 3,
} optics_ui_text_family;

OPTICS_UI_API void optics_ui_set_text_family(optics_ui *ui, optics_ui_text_family family);
OPTICS_UI_API optics_ui_text_family optics_ui_get_text_family(const optics_ui *ui);

typedef struct optics_ui_text_metrics {
    float width;
    float height;
    float baseline; /* from top */
} optics_ui_text_metrics;

/* Query descriptor, not a widget descriptor: text measurement has no layout
 * box. Widget `optics_ui_*_opts` still begin with `optics_ui_box` at offset 0. */
typedef struct optics_ui_text_measure_opts {
    const char *utf8; /* NUL-terminated text to shape */
    float size_px;    /* 0 uses the engine default */
    float weight;     /* 0 uses the regular slot; >500 selects bold */
} optics_ui_text_measure_opts;

/* The only text entry point layout (ADR-0028 pass 1) may call. Backed
 * by glyph_measure via the text seam (ADR-0033). */
OPTICS_UI_API optics_ui_text_metrics
optics_ui_text_measure(optics_ui *ui, const optics_ui_text_measure_opts *opts);

/* Release the text engine's high-water scratch and shaping cache (the
 * shared engine's `glyph_compact`). The engine grows layout/run/
 * codepoint scratch to the largest text ever shaped and keeps it; for a
 * one-off megabyte paste that peak lingers for the session. Call this
 * from the host's idle path (window does, on the low-power frame cadence)
 * — cheap enough to call every frame, and the next measure/draw
 * reallocates to whatever it actually needs. Null-safe. */
OPTICS_UI_API void optics_ui_text_compact(optics_ui *ui);

/* ================================================================== */
/*  Widget skins (ADR-0059)                                           */
/*                                                                    */
/*  A widget's last phase — emission of its draw commands — is a      */
/*  replaceable skin function. The widget keeps identity, measuring,  */
/*  interaction, animation, and accessibility; the skin receives a    */
/*  plain-data record (kind + state + bounds + resolved style +       */
/*  content) and is the only code that writes draw commands. Skins    */
/*  are replaceable context-wide (optics_ui_set_skin); NULL restores the   */
/*  built-in default, so default rendering is unchanged.              */
/* ================================================================== */

typedef enum optics_ui_widget_kind : uint32_t {
    OPTICS_UI_WIDGET_LABEL = 0,
    OPTICS_UI_WIDGET_ICON = 1,
    OPTICS_UI_WIDGET_IMAGE = 2,
    OPTICS_UI_WIDGET_SEPARATOR = 3,
    OPTICS_UI_WIDGET_BUTTON = 4,
    OPTICS_UI_WIDGET_CHECKBOX = 5,
    OPTICS_UI_WIDGET_SELECTABLE = 6,
    OPTICS_UI_WIDGET_SLIDER = 7,
    OPTICS_UI_WIDGET_TEXTEDIT = 8,
    OPTICS_UI_WIDGET_KIND_COUNT,

    /* ---- Host-reserved range (ADR-0073) --------------------------- */
    OPTICS_UI_WIDGET_KIND_USER_BASE = 0x40000000u,
} optics_ui_widget_kind;

/* The boundary is ABI: a future library version must keep the reserved
 * range reserved (hosts bake these values into their own enums). */
static_assert((uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE == 0x40000000u,
              "user widget-kind base is an ABI boundary (ADR-0073)");
static_assert((uint32_t)OPTICS_UI_WIDGET_KIND_COUNT <= (uint32_t)OPTICS_UI_WIDGET_KIND_USER_BASE,
              "built-in kinds must never enter the host-reserved range");

/* One wrapped line of text for a OPTICS_UI_WIDGET_LABEL record (and the visible
 * body lines of a multiline OPTICS_UI_WIDGET_TEXTEDIT record): the widget owns
 * wrapping / windowing (behaviour), the skin only draws. Strings are
 * borrowed for the frame; coordinates are node-local. */
typedef struct optics_ui_text_line {
    const char *text;
    float x, y;
    /* Shaped advance of this slice. Recorded on the emitted text command so the
     * rasteriser and damage tracking position the run identically without either
     * re-shaping. */
    float advance;
} optics_ui_text_line;

typedef enum optics_ui_button_variant : uint32_t {
    OPTICS_UI_BUTTON_DEFAULT = 0,
    OPTICS_UI_BUTTON_PRIMARY = 1,
    OPTICS_UI_BUTTON_SUBTLE = 2,
    OPTICS_UI_BUTTON_LINK = 3,
} optics_ui_button_variant;

typedef enum optics_ui_checkbox_appearance : uint32_t {
    OPTICS_UI_CHECKBOX_BOX = 0,    /* square checkbox */
    OPTICS_UI_CHECKBOX_SWITCH = 1, /* toggle switch */
    OPTICS_UI_CHECKBOX_RADIO = 2,  /* radio circle */
} optics_ui_checkbox_appearance;

/* Per-kind content payload for optics_ui_widget_record.
 *
 * Borrow contract (ADR-0085): every string/line pointer in this struct
 * is valid only for the skin-emission call that carries it — the widget
 * build path either copies into the frame arena (semantics.name, wrapped
 * lines, preedit display) or points directly at the caller's own label
 * buffer (label/checkbox/selectable/slider), which the caller guarantees
 * is stable through the frame. Skins must not cache these pointers past
 * the emit call; skins that need the text later must copy it. */
typedef struct optics_ui_widget_content {
    const char *label;
    optics_ui_text_metrics text;
    float text_size;
    float text_weight;
    bool compact;
    const optics_ui_text_line *lines;
    int line_count;
    optics_ui_align align;
    optics_ui_icon_id icon;
    float glyph_size;
    optics_gfx_image *image;
    optics_gfx_color tint;
    float ratio;
    bool vertical;
    bool error;
    optics_ui_checkbox_appearance appearance;
    optics_ui_button_variant variant;

    /* TEXTEDIT: text-edit payload */
    bool multiline;
    const char *edit_text;
    float edit_text_x;
    float edit_text_y;
    bool show_placeholder;
    const optics_gfx_rect *sel_rects;
    int sel_rect_count;
    optics_gfx_rect caret;
    bool show_caret;
    optics_gfx_rect preedit_underline;
    bool has_preedit;
    optics_gfx_rect preedit_clause;
} optics_ui_widget_content;

/* Everything a skin needs to draw one widget for one frame.
 *
 * `bounds` is the widget's content box in NODE-LOCAL coordinates — the
 * same space the skin's emission calls (and optics_ui_draw_cmd.rel) use. Its
 * origin is always (0,0) because the final position is solved by layout
 * after the skin runs; w/h are this frame's measured size (fixed-size
 * hints applied). `last_bounds` is last frame's arranged rect in UI
 * space — what interaction hit-tested — zeroed on the widget's first
 * frame (the documented one-frame latency, ADR-0029). Skins that scale
 * chrome with the arranged size (the slider track) read last_bounds. */
typedef struct optics_ui_widget_record {
    optics_ui_widget_kind kind;
    uint32_t state;                 /* optics_ui_widget_state bits (ADR-0058) */
    optics_gfx_rect bounds;         /* node-local content box            */
    optics_gfx_rect last_bounds;    /* last frame's arranged rect        */
    optics_ui_style_resolved style; /* resolved instance/theme style     */
    uint32_t style_fields;          /* OPTICS_UI_STYLE_* bits the instance set;
                                       0 = everything came from the theme */
    float hover_t;                  /* eased hover, updated this frame   */
    float active_t;                 /* eased active, updated this frame  */
    optics_ui_widget_content content;
} optics_ui_widget_record;

/* A skin draws one widget for one frame. It must not retain the record,
 * the node, or content strings past the call. `node` is the widget's
 * retained slot — skins push commands onto it and may read its public
 * accessors (e.g. optics_ui_node_bounds), nothing more. */
typedef void (*optics_ui_skin_fn)(optics_ui *ui, optics_ui_node *node,
                                  const optics_ui_widget_record *rec);

/* Skin + caller context. The plain optics_ui_skin_fn signature has no closure
 * slot, which forced any stateful skin (a spring indicator carrying its
 * own physics) to reach the state through a process global — the exact
 * hazard ADR-0059 flagged as future work. This form passes `user` back on
 * every emission; register it with optics_ui_set_skin_userdata. */
typedef void (*optics_ui_skin_userdata_fn)(optics_ui *ui, optics_ui_node *node,
                                           const optics_ui_widget_record *rec, void *user);

/* Replace the skin for a widget kind context-wide; NULL restores the
 * built-in default. The context is the single override granularity
 * (ADR-0061 retired the per-call *_skinned forms): for a one-off override,
 * set the skin, build the widget, restore NULL. */
OPTICS_UI_API void optics_ui_set_skin(optics_ui *ui, optics_ui_widget_kind kind,
                                      optics_ui_skin_fn fn);
/* Same, with a closure pointer delivered to the skin on every emission.
 * `user` is stored verbatim and passed back unmodified; the library never
 * dereferences it. NULL fn restores the default (user is then ignored).
 * Registered userdata skins also receive first-touch zeroes from
 * optics_ui_skin_scratch like any other skin. */
OPTICS_UI_API void optics_ui_set_skin_userdata(optics_ui *ui, optics_ui_widget_kind kind,
                                               optics_ui_skin_userdata_fn fn, void *user);
/* The built-in default skin for a kind — for wrapping: call it from a
 * custom skin to keep the stock chrome, then add your own. */
OPTICS_UI_API optics_ui_skin_fn optics_ui_default_skin(optics_ui_widget_kind kind);

/* Per-node scratch storage for skins (ADR-0061 item 9): OPTICS_UI_SKIN_SCRATCH_FLOATS
 * retained floats, zeroed on the node's first touch, living and dying with the
 * node (ADR-0038's GC reclaims the node and the scratch goes with it).
 * This is the inline fast path for the common case (one 1-D spring:
 * position/velocity/target/stamp). Need more — an X/Y pair of springs, a
 * scale+opacity+rotation bundle? Graduate to optics_ui_node_state(node, bytes):
 * arbitrary size, same zero-on-first-touch, same GC lifetime, no external
 * hashtable required. This is mechanism, not animation: the library stores
 * state; it never integrates anything. See libs/motion (ADR-0077, ADR-0106) for the
 * math to run on it. */
#define OPTICS_UI_SKIN_SCRATCH_FLOATS 4
OPTICS_UI_API float *optics_ui_skin_scratch(optics_ui *ui, optics_ui_node *node);

/* Skin emission seam — the skin's pen. Rects are node-local, same space
 * as the record's `bounds`; a zero w/h rect spans the full node box. For
 * text, a negative rel.w centres horizontally in the resolved node rect
 * and a negative rel.h centres vertically (centring is resolved at render
 * time, so it stays correct when a parent constrains the node). */
typedef struct optics_ui_skin_rect_opts {
    optics_ui_node *node;
    optics_gfx_rect rel;
    optics_gfx_color color;
    float radius;
} optics_ui_skin_rect_opts;

typedef struct optics_ui_skin_border_opts {
    optics_ui_node *node;
    optics_gfx_rect rel;
    optics_gfx_color color;
    float radius;
    float width;
} optics_ui_skin_border_opts;

typedef struct optics_ui_skin_text_opts {
    optics_ui_node *node;
    optics_gfx_rect rel;
    optics_gfx_color color;
    const char *utf8;
    float size_px;
    float weight;
} optics_ui_skin_text_opts;

typedef struct optics_ui_skin_icon_opts {
    optics_ui_node *node;
    optics_gfx_rect rel;
    optics_gfx_color color;
    float stroke;
    optics_ui_icon_id icon;
} optics_ui_skin_icon_opts;

typedef struct optics_ui_skin_clip_push_opts {
    optics_ui_node *node;
    optics_gfx_rect rel;
} optics_ui_skin_clip_push_opts;

OPTICS_UI_API void optics_ui_skin_rect(optics_ui *ui, const optics_ui_skin_rect_opts *opts);
OPTICS_UI_API void optics_ui_skin_border(optics_ui *ui, const optics_ui_skin_border_opts *opts);
OPTICS_UI_API void optics_ui_skin_text(optics_ui *ui, const optics_ui_skin_text_opts *opts);
OPTICS_UI_API void optics_ui_skin_icon(optics_ui *ui, const optics_ui_skin_icon_opts *opts);
/* Nested logical clip for skins that clip sub-regions (the table's cells).
 * Balanced push/pop; the rect is intersected with the enclosing clip at
 * render time. */
OPTICS_UI_API void optics_ui_skin_clip_push(optics_ui *ui,
                                            const optics_ui_skin_clip_push_opts *opts);
OPTICS_UI_API void optics_ui_skin_clip_pop(optics_ui *ui, optics_ui_node *node);

/* Emit a skin record for a HOST-RESERVED widget kind (ADR-0073).
 *
 * This is the emission half of the user-widget contract: a composite
 * built from optics_ui_pressable_begin + children calls this with its own
 * user-range kind and the skin registered for that kind fires — identity
 * and re-skinning parity with built-ins, with no library-side
 * measure/interact behavior implied. `kind` outside the reserved range is
 * rejected (built-in kinds emit through their own widget entry points,
 * with their own records; going around them here would bypass their
 * semantics). Content fields default as in optics_ui_widget_record; set what
 * the skin needs via `content`. No-op (not an error) when the node is
 * NULL or no skin is registered for the kind. */
typedef struct optics_ui_skin_emit_user_opts {
    optics_ui_node *node;
    optics_ui_widget_kind kind;
    optics_ui_widget_record rec;
} optics_ui_skin_emit_user_opts;

OPTICS_UI_API void optics_ui_skin_emit_user(optics_ui *ui,
                                            const optics_ui_skin_emit_user_opts *opts);

/* ================================================================== */
/*  Context lifecycle (ADR-0032)                                      */
/* ================================================================== */

typedef struct optics_ui_desc {
    /* Size guard, same pattern as optics_ui_input/optics_ui_theme (ADR-0032): set
     * to sizeof(optics_ui_desc). 0 = legacy caller (pre-guard layout trusted
     * whole). The library copies only min(caller, library) bytes, so a
     * caller built against an older or newer header degrades cleanly
     * instead of over-reading. Future fields append after `clipboard`. */
    uint32_t size;

    optics_gfx_device *device;     /* retained; persistent allocator source.
                                NULL = use libc malloc (headless/tests,
                                render unavailable). */
    optics_ui_theme theme;         /* initial token set; zeroed = default */
    size_t arena_bytes;            /* per-frame arena capacity; 0 = default */
    uint32_t store_capacity;       /* initial node-store slots; 0 = default */
    float scale;                   /* device-pixel scale; 0 = 1.0          */
    float text_scale;              /* accessibility text scale; 0/1 = 1.0  */
    optics_ui_clipboard clipboard; /* optional host clipboard (ADR-0036)   */
    optics_ui_dnd_host dnd;        /* optional host DnD interface (ADR-0086) */
    /* Caret blink period in seconds; 0 = the library default (ADR-0151).
     * Appended under the size guard, so a caller built against an older
     * header keeps the default and never makes the library read past its
     * own struct. */
    float caret_blink_period_s;
} optics_ui_desc;

OPTICS_GFX_NODISCARD OPTICS_UI_API optics_gfx_result optics_ui_create(const optics_ui_desc *desc,
                                                                      optics_ui **out);

/* Zero-init + append-safe descriptor preset (matches the OPTICS_GFX_*_DESC_INIT
 * convention). New code should prefer this over a bare {0}: it pins the
 * size guard so a future field append can't silently leave it stale. */
#define OPTICS_UI_DESC_INIT /* clang-format off */ \
    { .size = sizeof(optics_ui_desc) } /* clang-format on */

OPTICS_UI_API void optics_ui_release(optics_ui *ui);
OPTICS_UI_API void optics_ui_set_theme(optics_ui *ui, optics_ui_theme theme);
OPTICS_UI_API optics_ui_theme optics_ui_get_theme(const optics_ui *ui);

/* Frame-scoped opacity switch (0..1; default 1.0): the single fade knob
 * for enter/exit motion. Every node built while an opacity is in effect
 * carries it as a build-time stamp. Its own visual commands form one
 * isolated opacity group; children independently capture the current setting.
 * Rects, borders, text, icons, images and scrollbars fade together. Like
 * the style scope stack, the switch resets to 1.0 at every optics_ui_begin, so
 * a forgotten restore cannot dim the next frame; within a frame, set it
 * back after building the faded subtree. Mechanism, not animation: the
 * host owns the clock. */
OPTICS_UI_API void optics_ui_set_opacity(optics_ui *ui, float opacity);
OPTICS_UI_API float optics_ui_opacity(const optics_ui *ui);
OPTICS_UI_API float optics_ui_dt(const optics_ui *ui); /* frame delta, seconds */

/* Scoped frame clip.
 *
 * Every node built between `optics_ui_clip_begin` and `optics_ui_clip_end` inherits
 * `opts.rect` as a build-time stamp. The stamp clips the node's own draw
 * commands, its children, its snapshot/hit region, and — unlike a parent
 * container's flow clip — a placed (`OPTICS_UI_PLACE_ABS`) node that escapes
 * its ancestors. This is the clip for a chrome overlay whose children are
 * deliberately placed outside the parent's box (a carousel strip, a
 * floating panel with absolute labels).
 *
 * Nested calls intersect with the enclosing clip. The stack resets at
 * every `optics_ui_begin`; an unbalanced `optics_ui_clip_end` is a no-op and a
 * deeper-than-supported push sets `optics_ui_overflowed` rather than dropping
 * the matching pop. Coordinates and the input frame are output-logical
 * pixels. */
typedef struct optics_ui_clip_opts {
    optics_gfx_rect rect;
} optics_ui_clip_opts;

OPTICS_UI_API void optics_ui_clip_begin(optics_ui *ui, const optics_ui_clip_opts *opts);
OPTICS_UI_API void optics_ui_clip_end(optics_ui *ui);

/* Ghost replay — the leave-animation render surface (ADR-0078).
 *
 * Call this every frame while an exit animation runs for a subtree the
 * build no longer produces: it re-pins the subtree's snapshot (captured
 * at its last live optics_ui_end) and paints it at `alpha` through the same
 * command emitter and alpha bake as a live subtree (ADR-0068 semantics).
 * No call → the snapshot counts down and expires after
 * OPTICS_UI_GHOST_MAX_FRAMES frames. Unknown or never-seen ids are ignored
 * (advisory, never a host crash). Ghosts never hit-test, focus, or enter
 * the accessibility tree; they render after live content in their band.
 * Mechanism, not animation: the host owns the clock and the easing. */
typedef struct optics_ui_set_ghost_opts {
    optics_ui_id subtree_root;
    float alpha;
} optics_ui_set_ghost_opts;

OPTICS_UI_API void optics_ui_set_ghost(optics_ui *ui, const optics_ui_set_ghost_opts *opts);

/* Device-pixel scale (HiDPI). The application reports the compositor /
 * window-system scale here; layout, input, and `optics_ui_input.display_size`
 * stay in *logical* pixels, and snapshot publication captures the scale
 * transform by this factor so 1 logical pixel maps to `scale` device
 * pixels. Default 1.0. */
OPTICS_UI_API void optics_ui_set_scale(optics_ui *ui, float scale);
OPTICS_UI_API float optics_ui_scale(const optics_ui *ui);

/* Accessibility text scale — the OS "make text bigger" preference. A pure
 * multiplier on every font-size token (body, headings, explicit label
 * point sizes) applied at the resolved-style funnel, so glyphs, caret
 * metrics, and every widget height derived from a font token grow
 * together: text scales, boxes follow, nothing clips. Orthogonal to
 * optics_ui_set_scale (device pixels): a 1.25 text scale at 2x DPI renders
 * 1.25x taller glyphs at the same 2x raster density. Pure-px geometry
 * (padding, stroke widths) deliberately stays put. ui executes the
 * factor; the host owns the policy (window reads the system preference on
 * all backends). Non-finite and <= 0 are ignored. Default 1.0. Not
 * frame-scoped — persists like scale/reduced_motion. */
OPTICS_UI_API void optics_ui_set_text_scale(optics_ui *ui, float factor);
OPTICS_UI_API float optics_ui_text_scale(const optics_ui *ui);

/* ================================================================== */
/*  Frame lifecycle (ADR-0024, frame-lifecycle.md)                    */
/* ================================================================== */

OPTICS_UI_API void optics_ui_begin(optics_ui *ui, const optics_ui_input *input);
OPTICS_UI_API void optics_ui_end(optics_ui *ui);
typedef struct optics_ui_scene_snapshot optics_ui_scene_snapshot;

/* Create an immutable snapshot of the resolved layout tree and compiled visuals.
 * The published snapshot is GPU-independent, owned, and survives destruction of
 * the mutable ui context. */
OPTICS_GFX_NODISCARD OPTICS_UI_API optics_gfx_result
optics_ui_snapshot_create(optics_ui *ui, optics_ui_scene_snapshot **out_snapshot);
OPTICS_UI_API optics_ui_scene_snapshot *
optics_ui_snapshot_retain(optics_ui_scene_snapshot *snapshot);
OPTICS_UI_API void optics_ui_snapshot_release(optics_ui_scene_snapshot *snapshot);
OPTICS_UI_API uint64_t optics_ui_snapshot_generation(const optics_ui_scene_snapshot *snapshot);
OPTICS_GFX_NODISCARD OPTICS_UI_API optics_gfx_result
optics_ui_snapshot_submit(const optics_ui_scene_snapshot *snapshot, optics_gfx_canvas *canvas);

/* Generation progression & presentation handshake (ADR-0094).
 * A new generation is published at each optics_ui_end. The presentation engine
 * activates the corresponding snapshot upon successful frame presentation. */
OPTICS_UI_API uint64_t optics_ui_generation(const optics_ui *ui);
/* Activate exactly the successfully presented snapshot, or explicitly select
 * one in a headless host. Foreign and older generations are rejected. */
OPTICS_GFX_NODISCARD OPTICS_UI_API optics_gfx_result
optics_ui_snapshot_activate(optics_ui *ui, optics_ui_scene_snapshot *snapshot);
OPTICS_UI_API void optics_ui_snapshot_accessibility_walk(const optics_ui_scene_snapshot *snapshot,
                                                         optics_ui_a11y_visit_fn visit, void *user);
OPTICS_UI_API uint64_t optics_ui_last_presented_generation(const optics_ui *ui);

/* Borrowed immutable list, valid while the snapshot is retained. */
OPTICS_UI_API const optics_gfx_display_list *
optics_ui_snapshot_display_list(const optics_ui_scene_snapshot *snapshot);

/* True if the per-frame arena overflowed during the frame just built. */
OPTICS_UI_API bool optics_ui_overflowed(const optics_ui *ui);

/* True if the same widget id was linked more than once under one parent in
 * the frame just built. The later occurrence is omitted because retained ids
 * name nodes one-to-one; callers building repeated labels must scope them
 * with optics_ui_push_id[_int] or pass an explicit optics_ui_box.id. */
OPTICS_UI_API bool optics_ui_has_duplicate_ids(const optics_ui *ui);

/* True if an eased value (hover/active fade, …) was still in transit during
 * the frame just built. An input-driven host that only paints on events should
 * schedule one more frame while this holds so animations settle to rest instead
 * of freezing mid-fade when input stops. Valid between optics_ui_end and the next
 * optics_ui_begin. */
OPTICS_UI_API bool optics_ui_anim_pending(const optics_ui *ui);

/* True if the frame just built would paint anything different from what is
 * already on screen: base-tree or placed-subtree damage (geometry, draw
 * lists, lifecycle), an appearing/disappearing transient node or tooltip,
 * an eased value still in transit, or the caret's blink phase *flipping*
 * against the phase that is actually presented.
 *
 * A focused caret is not damage by its own existence (ADR-0151): the blink
 * is a two-state clock, so only a transition between its states changes
 * pixels. That distinction is what makes quiescence reachable for a host with
 * a focused text field — the previous term made this function permanently
 * true while any widget held focus.
 *
 * A damage-driven host may skip the whole acquire/paint/present cycle when
 * this returns false. Read-only; valid between optics_ui_end and the next
 * optics_ui_begin. Resize/scale changes are host-visible, not ui state — the
 * host must still paint the first frame after those itself. */
OPTICS_UI_API bool optics_ui_frame_needs_repaint(const optics_ui *ui);

/* Describe the damaged area of the frame just built as a bounded list of
 * rectangles in UI logical units, and return the number written to
 * `opts->rects`.
 *
 * Every rectangle the frame can have changed is covered, and no rectangle is
 * contained in another: damage is a set, so a host may scissor each one
 * independently, hand the whole list to a per-plane damage hint, or — when its
 * presentation API only accepts one rectangle — union them with
 * `optics_gfx_rect_union` at that boundary. This is the *only* damage query. There is
 * deliberately no single-rect variant: a producer that answers with a box before
 * the consumer has seen the set destroys the localization irrecoverably, and the
 * union of two edge bands is the whole output (ADR-0143).
 *
 * Localization rules, in the order the walk applies them:
 *   - a placed ABS subtree (a band root) contributes its whole subtree's
 *     damage: it owns every pixel it draws;
 *   - a FLOW interior node contributes only its own paint delta, not its
 *     descendants' — an edited cell reports its cell, not the column that
 *     contains it, whose rect is the display;
 *   - a text run contributes the measured glyph extent, not its command
 *     rectangle, because negative `rel` extents centre the run inside the box;
 *   - the caret, the tooltip anchor, and device-space fade ghosts are added
 *     independently.
 *
 * Degradation is always toward covering more, never less. A frame that needs a
 * repaint but names no region reports the viewport. A frame whose damage exceeds
 * `OPTICS_UI_DAMAGE_RECTS_CAPACITY` — a compile-time bound, so the walk allocates
 * nothing — reports the viewport, because the regions that did not fit have no
 * coordinates to contribute and any smaller answer would strand their pixels. */
enum : uint32_t {
    OPTICS_UI_DAMAGE_RECTS_CAPACITY = 64,
};

typedef struct optics_ui_damage_opts {
    /* Caller-owned buffer. nullptr or `capacity == 0` asks only for the
     * required count, which is how a host sizes its buffer without a
     * second contract. */
    optics_gfx_rect *rects;
    uint32_t capacity;
} optics_ui_damage_opts;

/* Returns the number of rectangles written. When damage exceeds
 * `opts->capacity` the answer is one full-viewport rectangle and the count is 1.
 * When the frame needs no repaint the count is 0. Valid between optics_ui_end and the
 * next optics_ui_begin. */
OPTICS_UI_API uint32_t optics_ui_damage_rects(const optics_ui *ui,
                                              const optics_ui_damage_opts *opts);

/* Accessibility reduced-motion switch. When enabled, every eased value in
 * ui resolves to its target within one frame — no fades, slides, or other
 * transitions — and optics_ui_anim_pending stays false. The host owns the policy
 * (user preference); ui executes it. Default false. */
OPTICS_UI_API void optics_ui_set_reduced_motion(optics_ui *ui, bool reduced);
OPTICS_UI_API bool optics_ui_reduced_motion(const optics_ui *ui);

/* ================================================================== */
/*  Identity (ADR-0026)                                               */
/* ================================================================== */

OPTICS_UI_API void optics_ui_push_id(optics_ui *ui, const char *seed);
OPTICS_UI_API void optics_ui_push_id_int(optics_ui *ui, int64_t seed);
OPTICS_UI_API void optics_ui_pop_id(optics_ui *ui);
OPTICS_UI_API optics_ui_id optics_ui_current_id(const optics_ui *ui, const char *label);

/* ================================================================== */
/*  Identity & per-widget options                                     */
/* ================================================================== */

/* optics_ui_box — per-widget layout, identity, and state, common to every
 * widget descriptor. Each field applies to *this* call only; there are
 * no floating "applies to the next/last widget" modifiers, so there is
 * no ordering hazard. Zeroed fields take the widget default. */
typedef struct optics_ui_box {
    const char *id;        /* explicit identity; NULL = derive from label/content */
    float flex;            /* main-axis grow factor; 0 = don't grow       */
    float width;           /* fixed width, logical px;  0 = intrinsic      */
    float height;          /* fixed height, logical px; 0 = intrinsic      */
    float min_width;       /* minimum width constraint; 0 = unconstrained  */
    float min_height;      /* minimum height constraint; 0 = unconstrained */
    float max_width;       /* maximum width constraint; 0 = unconstrained  */
    float max_height;      /* maximum height constraint; 0 = unconstrained */
    int32_t col_span;      /* grid: columns spanned (0 or 1 = 1 column)    */
    int32_t row_span;      /* grid: rows spanned (0 or 1 = 1 row)          */
    int32_t grid_col;      /* grid: 1-based col position (0 = auto-place)  */
    int32_t grid_row;      /* grid: 1-based row position (0 = auto-place)  */
    optics_ui_align align; /* horizontal/main item alignment override      */
    optics_ui_align cross; /* vertical/cross item alignment override       */
    bool disabled;         /* non-interactive + dimmed                     */
    bool error;            /* validation-error styling                     */
    const char *tooltip;   /* shown while this widget is hovered           */
    optics_ui_style style; /* per-call style overrides (ADR-0061)          */
} optics_ui_box;

/* ================================================================== */
/*  Containers / layout (ADR-0028, ADR-0104)                          */
/* ================================================================== */

typedef struct optics_ui_layout_opts {
    optics_ui_box box;       /* sole source of geometry, constraints, identity, style */
    float gap;               /* between children, main axis                  */
    float pad;               /* inside the container, all sides              */
    optics_ui_align align;   /* main-axis distribution                       */
    optics_ui_align cross;   /* cross-axis alignment; OPTICS_UI_STRETCH fills    */
    optics_gfx_color bg;     /* background fill; alpha 0 = transparent       */
    float radius;            /* corner radius                                */
    optics_gfx_color border; /* border stroke                                */
    float border_width;      /* border width                                 */
    bool wrap;               /* multi-line wrapping (ADR-0105)               */
    float row_gap;           /* vertical gap between wrapped lines (0 = gap) */
} optics_ui_layout_opts;

OPTICS_UI_API void optics_ui_row_begin(optics_ui *ui, const optics_ui_layout_opts *opts);
OPTICS_UI_API void optics_ui_row_end(optics_ui *ui);
OPTICS_UI_API void optics_ui_column_begin(optics_ui *ui, const optics_ui_layout_opts *opts);
OPTICS_UI_API void optics_ui_column_end(optics_ui *ui);
OPTICS_UI_API void optics_ui_wrap_begin(optics_ui *ui, const optics_ui_layout_opts *opts);
OPTICS_UI_API void optics_ui_wrap_end(optics_ui *ui);
OPTICS_UI_API void optics_ui_stack_begin(optics_ui *ui, const optics_ui_layout_opts *opts);
OPTICS_UI_API void optics_ui_stack_end(optics_ui *ui);
OPTICS_UI_API void optics_ui_close(optics_ui *ui); /* closes the innermost open container */

typedef struct optics_ui_grid_opts {
    optics_ui_box box;
    int columns;
    float col_gap;
    float row_gap;
    float pad;
    float row_height;        /* fixed uniform row height; 0 = content height */
    optics_ui_align align;   /* cell horizontal alignment (default OPTICS_UI_START) */
    optics_ui_align cross;   /* cell vertical alignment (default OPTICS_UI_STRETCH) */
    optics_gfx_color bg;     /* background fill; alpha 0 = transparent       */
    float radius;            /* corner radius                                */
    optics_gfx_color border; /* border stroke                                */
    float border_width;      /* border width                                 */
} optics_ui_grid_opts;

OPTICS_UI_API void optics_ui_grid_begin(optics_ui *ui, const optics_ui_grid_opts *opts);
OPTICS_UI_API void optics_ui_grid_end(optics_ui *ui);

typedef struct optics_ui_scroll_opts {
    optics_ui_box box;
    bool autohide;
} optics_ui_scroll_opts;

typedef struct optics_ui_scroll_offset_opts {
    const char *id;
    float *out_x;
    float *out_y;
} optics_ui_scroll_offset_opts;

typedef struct optics_ui_scroll_to_opts {
    const char *id;
    float x;
    float y;
} optics_ui_scroll_to_opts;

OPTICS_UI_API bool optics_ui_scroll_begin(optics_ui *ui, const optics_ui_scroll_opts *opts);
OPTICS_UI_API void optics_ui_scroll_end(optics_ui *ui);
OPTICS_UI_NODISCARD OPTICS_UI_API bool
optics_ui_scroll_offset(const optics_ui *ui, const optics_ui_scroll_offset_opts *opts);
OPTICS_UI_API void optics_ui_scroll_to(optics_ui *ui, const optics_ui_scroll_to_opts *opts);

typedef struct optics_ui_pressable_opts {
    optics_ui_box box;
    const char *label;
    int mouse_button;
    optics_ui_layout_opts layout;
} optics_ui_pressable_opts;

OPTICS_UI_API optics_ui_response optics_ui_pressable_begin(optics_ui *ui,
                                                           const optics_ui_pressable_opts *opts);
OPTICS_UI_API void optics_ui_pressable_end(optics_ui *ui);

/* Layout modifiers */
OPTICS_UI_API void optics_ui_flex(optics_ui *ui, float grow);
OPTICS_UI_API void optics_ui_spacer(optics_ui *ui, float size);
OPTICS_UI_API void optics_ui_space_between(optics_ui *ui);
OPTICS_UI_API void optics_ui_fit(optics_ui *ui);
OPTICS_UI_API void optics_ui_size(optics_ui *ui, float width, float height);
OPTICS_UI_API void optics_ui_col_span(optics_ui *ui, uint32_t span);
OPTICS_UI_API void optics_ui_row_span(optics_ui *ui, uint32_t span);
OPTICS_UI_API void optics_ui_grid_at(optics_ui *ui, int32_t col, int32_t row);

/* ================================================================== */
/*  Absolute placement & overlays (ADR-0060)                          */
/* ================================================================== */

typedef enum optics_ui_band : uint32_t {
    OPTICS_UI_BAND_BASE = 0,
    OPTICS_UI_BAND_BACKDROP = 1,
    OPTICS_UI_BAND_CHROME = 2,
    OPTICS_UI_BAND_POPUP = 3,
    OPTICS_UI_BAND_MODAL = 4,
    OPTICS_UI_BAND_TOOLTIP = 5,
    OPTICS_UI_BAND_NOTIFY = 6,
    OPTICS_UI_BAND_COUNT = 7,
} optics_ui_band;

typedef enum optics_ui_place_mode : uint32_t {
    OPTICS_UI_PLACE_EXACT = 0,
    OPTICS_UI_PLACE_ANCHORED = 1,
    OPTICS_UI_PLACE_CENTERED = 2,
    OPTICS_UI_PLACE_TOOLTIP = 3,
    OPTICS_UI_PLACE_BACKDROP = 4,
} optics_ui_place_mode;

typedef struct optics_ui_place_opts {
    optics_ui_box box;
    optics_ui_band band;
    optics_ui_place_mode mode;
    optics_gfx_rect rect;
    optics_gfx_rect bounds;
    bool transient;
    bool interactive;
    optics_ui_layout_opts layout;
} optics_ui_place_opts;

OPTICS_UI_API bool optics_ui_place_begin(optics_ui *ui, const optics_ui_place_opts *opts);
OPTICS_UI_API void optics_ui_place_end(optics_ui *ui);
OPTICS_UI_API void optics_ui_place_open(optics_ui *ui, const char *id);
OPTICS_UI_API void optics_ui_place_close(optics_ui *ui, const char *id);
OPTICS_UI_API void optics_ui_place_toggle(optics_ui *ui, const char *id);
OPTICS_UI_NODISCARD OPTICS_UI_API bool optics_ui_place_is_open(const optics_ui *ui, const char *id);
OPTICS_UI_API void optics_ui_place_close_all(optics_ui *ui);
OPTICS_UI_NODISCARD OPTICS_UI_API bool optics_ui_place_hovered(const optics_ui *ui, const char *id);

/* ================================================================== */
/*  Minimal Orthogonal Widgets (Single Descriptor API)                */
/* ================================================================== */

/* 1. Label — text rendering */
typedef struct optics_ui_label_opts {
    optics_ui_box box;
    const char *text;
    float size;            /* <= 0 = theme font size */
    float weight;          /* <= 0 = theme regular weight */
    bool wrap;             /* wrap text on max_width or container bounds */
    optics_ui_align align; /* OPTICS_UI_START, OPTICS_UI_CENTER, OPTICS_UI_END */
} optics_ui_label_opts;

OPTICS_UI_API optics_ui_response optics_ui_label(optics_ui *ui, const optics_ui_label_opts *opts);

/* 2. Icon — vector glyph */
typedef struct optics_ui_icon_opts {
    optics_ui_box box;
    optics_ui_icon_id id;   /* glyph id */
    float size;             /* <= 0 = theme font size */
    optics_gfx_color color; /* 0 = theme foreground */
} optics_ui_icon_opts;

OPTICS_UI_API optics_ui_response optics_ui_icon(optics_ui *ui, const optics_ui_icon_opts *opts);

/* 3. Image — raster texture */
typedef struct optics_ui_image_opts {
    optics_ui_box box;
    optics_gfx_image *image;
    float width;
    float height;
    optics_gfx_color tint;
} optics_ui_image_opts;

OPTICS_UI_API optics_ui_response optics_ui_image(optics_ui *ui, const optics_ui_image_opts *opts);

/* 4. Separator — horizontal or vertical dividing line */
typedef struct optics_ui_separator_opts {
    optics_ui_box box;
    optics_ui_axis axis; /* OPTICS_UI_ROW (horizontal line) or OPTICS_UI_COLUMN (vertical line) */
    float thickness;     /* <= 0 = 1.0f */
} optics_ui_separator_opts;

OPTICS_UI_API optics_ui_response optics_ui_separator(optics_ui *ui,
                                                     const optics_ui_separator_opts *opts);

/* 5. Button — clickable trigger */
typedef struct optics_ui_button_opts {
    optics_ui_box box;
    const char *label;
    optics_ui_icon_id icon;
    optics_gfx_image *image;
    optics_ui_button_variant variant;
    bool active;
    int mouse_button;
} optics_ui_button_opts;

OPTICS_UI_API optics_ui_response optics_ui_button(optics_ui *ui, const optics_ui_button_opts *opts);

/* 6. Checkbox — boolean toggle */
typedef struct optics_ui_checkbox_opts {
    optics_ui_box box;
    const char *label;
    bool *value;
    optics_ui_checkbox_appearance appearance;
} optics_ui_checkbox_opts;

OPTICS_UI_API optics_ui_response optics_ui_checkbox(optics_ui *ui,
                                                    const optics_ui_checkbox_opts *opts);

/* 7. Selectable — selectable row item */
typedef struct optics_ui_selectable_opts {
    optics_ui_box box;
    const char *label;
    bool selected;
    optics_ui_icon_id icon;
    float icon_size; /* 0 = fall back to style / theme icon_size */
    bool
        pointer_cursor; /* true = show web hand pointer, false (default) = standard desktop arrow */
} optics_ui_selectable_opts;

OPTICS_UI_API optics_ui_response optics_ui_selectable(optics_ui *ui,
                                                      const optics_ui_selectable_opts *opts);

/* 8. Slider — scalar value range dragging */
typedef struct optics_ui_slider_opts {
    optics_ui_box box;
    const char *label;
    float *value;
    float min;
    float max;
    float step;
    optics_ui_axis axis;
    const char *format;
} optics_ui_slider_opts;

OPTICS_UI_API optics_ui_response optics_ui_slider(optics_ui *ui, const optics_ui_slider_opts *opts);

/* 8. Textedit — single-line or multi-line text editor */
typedef struct optics_ui_textedit_opts {
    optics_ui_box box;
    char *buf;
    size_t cap;
    const char *placeholder;
    bool multiline;
    uint32_t rows;
    bool password;
    bool select_all_on_focus;
    bool readonly;
    bool accept_tab;      /* intercept Tab: sets r.tabbed, consumes Tab key */
    bool submit_on_enter; /* single-line: Return sets r.submitted, consumes Return key */
} optics_ui_textedit_opts;

typedef struct optics_ui_textedit_set_caret_opts {
    const char *label;
    uint32_t caret;
} optics_ui_textedit_set_caret_opts;

typedef struct optics_ui_textedit_get_caret_opts {
    const char *label;
    uint32_t *out_caret;
} optics_ui_textedit_get_caret_opts;

typedef struct optics_ui_textedit_set_selection_opts {
    const char *label;
    uint32_t sel_start;
    uint32_t sel_end;
} optics_ui_textedit_set_selection_opts;

typedef struct optics_ui_textedit_get_selection_opts {
    const char *label;
    uint32_t *out_sel_start;
    uint32_t *out_sel_end;
} optics_ui_textedit_get_selection_opts;

OPTICS_UI_API void optics_ui_consume_key(optics_ui *ui, int32_t key);
OPTICS_UI_API optics_ui_response optics_ui_textedit(optics_ui *ui,
                                                    const optics_ui_textedit_opts *opts);
OPTICS_UI_API void optics_ui_textedit_set_caret(optics_ui *ui,
                                                const optics_ui_textedit_set_caret_opts *opts);
OPTICS_UI_API bool optics_ui_textedit_get_caret(const optics_ui *ui,
                                                const optics_ui_textedit_get_caret_opts *opts);
OPTICS_UI_API void
optics_ui_textedit_set_selection(optics_ui *ui, const optics_ui_textedit_set_selection_opts *opts);
OPTICS_UI_API bool
optics_ui_textedit_get_selection(const optics_ui *ui,
                                 const optics_ui_textedit_get_selection_opts *opts);
OPTICS_UI_API void optics_ui_set_cursor_hint(optics_ui *ui, optics_ui_cursor_hint hint);

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static_assert(offsetof(optics_ui_layout_opts, box) == 0,
              "optics_ui_layout_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_grid_opts, box) == 0,
              "optics_ui_grid_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_scroll_opts, box) == 0,
              "optics_ui_scroll_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_pressable_opts, box) == 0,
              "optics_ui_pressable_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_place_opts, box) == 0,
              "optics_ui_place_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_label_opts, box) == 0,
              "optics_ui_label_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_icon_opts, box) == 0,
              "optics_ui_icon_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_image_opts, box) == 0,
              "optics_ui_image_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_separator_opts, box) == 0,
              "optics_ui_separator_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_button_opts, box) == 0,
              "optics_ui_button_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_checkbox_opts, box) == 0,
              "optics_ui_checkbox_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_selectable_opts, box) == 0,
              "optics_ui_selectable_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_slider_opts, box) == 0,
              "optics_ui_slider_opts.box must be at offset 0");
static_assert(offsetof(optics_ui_textedit_opts, box) == 0,
              "optics_ui_textedit_opts.box must be at offset 0");
#endif

/* ================================================================== */
/*  Interaction queries (ADR-0029)                                    */
/* ================================================================== */

OPTICS_UI_API optics_ui_response optics_ui_get_response(const optics_ui *ui); /* last widget */
OPTICS_UI_API optics_ui_cursor_hint optics_ui_get_cursor_hint(const optics_ui *ui);
OPTICS_UI_API bool optics_ui_focused(const optics_ui *ui, optics_ui_id id);
OPTICS_UI_API void optics_ui_set_focus(optics_ui *ui, optics_ui_id id);
OPTICS_UI_API optics_ui_id optics_ui_active(const optics_ui *ui);

/* ================================================================== */
/*  Clipboard and IME (ADR-0036)                                      */
/* ================================================================== */

/* Caret rect of the focused text widget in UI-space, zero when none.
 * The platform layer forwards this to the IME (e.g. zwp_text_input) so
 * the candidate window can position itself. This is *geometry* only: it
 * says where the caret is, never whether it is painted this frame. */
OPTICS_UI_API optics_gfx_rect optics_ui_caret_rect(const optics_ui *ui);

/* Current caret blink phase: true while the caret is in its visible half of
 * the blink cycle (ADR-0151).
 *
 * ui owns the blink clock because only ui can make it a *damage* signal.
 * The clock advances once per frame by `optics_ui_input.dt_seconds`, so a host
 * that paces frames from the display's pulse gets a display-sampled blink for
 * free, and a host that skips frames resumes on the phase it would have
 * reached. A skin checks this before drawing the caret; the host never has to
 * model a blink at all.
 *
 * Always true while the app has an IME composition in progress: frames are
 * event-driven then, and a blink would freeze mid-composition. */
OPTICS_UI_API bool optics_ui_caret_visible(const optics_ui *ui);

/* Override the caret blink period in seconds. `period <= 0` disables the
 * blink: the caret then stays solid, and ui reports no caret damage at all.
 * Accessibility preference (reduced motion) is a host policy, so the host
 * decides; ui executes. */
OPTICS_UI_API void optics_ui_set_caret_blink(optics_ui *ui, float period_seconds);

/* True when the caret's blink phase differs from the phase the last activated
 * snapshot put on screen (ADR-0151).
 *
 * This is the host's whole caret-damage contract: forward it to the frame
 * pacer as "one frame is owed", and never model a blink yourself. Comparing
 * against the *presented* phase rather than the last built one is what makes
 * a skipped frame keep its flip pending instead of dropping it, and it is why
 * a host cannot derive this from `optics_ui_caret_visible` alone (that is only the
 * current phase, with no memory of what reached the display).
 *
 * False when no caret is focused: a phase flip nobody can see is not damage. */
OPTICS_UI_API bool optics_ui_caret_flip_pending(const optics_ui *ui);

/* True when a caret blink clock is running: a text widget holds focus and the
 * blink is enabled (ADR-0151).
 *
 * A host needs this distinct from `optics_ui_caret_flip_pending`, and the two are
 * not interchangeable. The clock only advances inside a build (it integrates
 * `optics_ui_input.dt_seconds`), so a host that stopped producing frames could
 * never observe the next flip — the caret would freeze mid-blink. This level
 * therefore tells the pacer to keep its *low idle tick* alive; it does not ask
 * for a paint, and it must not buy the active-rate cadence. The paint is owed
 * separately, by the frame on which the flip lands. */
OPTICS_UI_API bool optics_ui_caret_clock_live(const optics_ui *ui);

/* Surrounding-text and content-hint context of the focused text widget,
 * for the host's IME integration (text-input-v3 set_surrounding_text /
 * set_content_type). Refreshed every frame by the focused text widget
 * alongside the caret rect; `utf8 == NULL` when no text widget is focused.
 * The buffer is BORROWED from the widget — valid until the next
 * optics_ui_begin. `cursor` is the caret byte offset in `utf8`. */
typedef struct optics_ui_text_context {
    const char *utf8;
    uint32_t len;
    uint32_t cursor;
    bool multiline; /* textarea vs single-line textfield */
} optics_ui_text_context;

OPTICS_UI_API optics_ui_text_context optics_ui_text_context_get(const optics_ui *ui);

typedef struct optics_ui_copy_opts {
    const char *utf8;
    size_t len;
} optics_ui_copy_opts;

typedef struct optics_ui_paste_opts {
    const char *utf8;
    size_t len;
} optics_ui_paste_opts;

typedef struct optics_ui_take_paste_opts {
    char *dst;
    uint32_t cap;
} optics_ui_take_paste_opts;

/* Place text on the system clipboard (calls the host optics_ui_clipboard.set_text
 * if any). No-op when no clipboard interface was supplied. */
OPTICS_UI_API void optics_ui_copy(optics_ui *ui, const optics_ui_copy_opts *opts);

/* Ask the host for clipboard text (calls optics_ui_clipboard.request_text). The
 * host later delivers it via optics_ui_paste. */
OPTICS_UI_API void optics_ui_request_paste(optics_ui *ui);

/* Host-driven delivery of clipboard text into ui. Queues the payload
 * for the focused text widget to consume on the next frame's build. */
OPTICS_UI_API void optics_ui_paste(optics_ui *ui, const optics_ui_paste_opts *opts);

/* Drain a pending paste payload for an app-owned editing surface (one that
 * renders text outside ui widgets and therefore has no focused widget to
 * consume the queue). Copies up to `cap-1` payload bytes into `dst`,
 * NUL-terminates, and returns the payload byte length excluding the NUL.
 * Returns 0 when nothing is pending; a drained payload is consumed.
 * Text widgets keep their own internal drain — app code should only call
 * this while its own surface is the paste target. */
OPTICS_UI_API uint32_t optics_ui_take_paste(optics_ui *ui, const optics_ui_take_paste_opts *opts);

/* Report the caret rect of an app-owned editing surface, same consumer as
 * the widget-reported rect: the platform layer forwards it to the IME so
 * the candidate window follows the caret. UI-space logical pixels. */
OPTICS_UI_API void optics_ui_set_caret_rect(optics_ui *ui, optics_gfx_rect r);

/* ================================================================== */
/*  Drag-and-Drop (DnD, ADR-0086)                                     */
/* ================================================================== */

typedef struct optics_ui_dnd_source_desc {
    optics_ui_id id;
    const char *text; /* Static text payload (e.g. URI or text) */
    size_t text_len;
    uint32_t actions;             /* Allowed actions bitmask (1=copy, 2=move) */
    optics_gfx_rect preview_rect; /* UI-space visual bounding box */
} optics_ui_dnd_source_desc;

typedef struct optics_ui_dnd_drop_info {
    bool is_hovered;           /* Cursor is over this drop zone */
    bool is_dropped;           /* Payload dropped on this target this frame */
    optics_gfx_point drop_pos; /* Drop position relative to target bounding box */
    uint32_t action;           /* Negotiated action */
} optics_ui_dnd_drop_info;

typedef struct optics_ui_dnd_drop_target_opts {
    optics_ui_id id;
    uint32_t accepted_actions;
    optics_ui_dnd_drop_info *out_info;
} optics_ui_dnd_drop_target_opts;

typedef struct optics_ui_deliver_drop_opts {
    const char *payload;
    size_t len;
    optics_gfx_point pos;
} optics_ui_deliver_drop_opts;

typedef struct optics_ui_take_drop_opts {
    char *dst;
    uint32_t cap;
} optics_ui_take_drop_opts;

/* Declare a node as a drag source. Returns true if drag was actively initiated/active. */
OPTICS_UI_API bool optics_ui_dnd_source(optics_ui *ui, const optics_ui_dnd_source_desc *desc);

/* Declare a node as a drop target zone. Returns true if hovered or dropped. */
OPTICS_UI_API bool optics_ui_dnd_drop_target(optics_ui *ui,
                                             const optics_ui_dnd_drop_target_opts *opts);

/* Platform delivery of drop payload into ui. */
OPTICS_UI_API void optics_ui_deliver_drop(optics_ui *ui, const optics_ui_deliver_drop_opts *opts);

/* Retrieve and consume pending drop payload during drop frame. Returns bytes copied. */
OPTICS_UI_API uint32_t optics_ui_take_drop(optics_ui *ui, const optics_ui_take_drop_opts *opts);

/* ================================================================== */
/*  Escape hatch — retained nodes (ADR-0031)                          */
/*                                                                    */
/*  Handles are borrows valid only within the current frame. Re-resolve*/
/*  by optics_ui_id each frame; never cache a node pointer past optics_ui_begin.  */
/* ================================================================== */

OPTICS_UI_API optics_ui_node *optics_ui_root(optics_ui *ui);
OPTICS_UI_API optics_ui_node *optics_ui_find(optics_ui *ui, optics_ui_id id);
OPTICS_UI_API optics_ui_id optics_ui_node_id(const optics_ui_node *n);
OPTICS_UI_API optics_gfx_rect optics_ui_node_bounds(const optics_ui_node *n); /* final_rect */
OPTICS_UI_API optics_ui_node_phase optics_ui_node_phase_of(const optics_ui_node *n);
OPTICS_UI_API optics_ui_node *optics_ui_node_parent(const optics_ui_node *n);
OPTICS_UI_API optics_ui_node *optics_ui_node_first_child(const optics_ui_node *n);
OPTICS_UI_API optics_ui_node *optics_ui_node_next_sibling(const optics_ui_node *n);
/* Borrow (allocating + zeroing on first touch) persistent per-node state. The
 * same pointer returns every frame the id survives. A node id has one fixed
 * state type: after first allocation, requesting a different byte size returns
 * NULL rather than moving the allocation and invalidating prior borrows. */
OPTICS_UI_API void *optics_ui_node_state(optics_ui_node *n, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_H */
