/*
 * lens/lens.h — immediate-mode façade over a retained-mode core.
 *
 * One library (liblens), one umbrella header. You write immediate-mode
 * code each frame; lens reconciles it against a retained tree that owns
 * layout, interaction, animation, and the draw list.
 *
 * Design contract (see docs/adr):
 *   - Public symbols are `lens_*`; library internals use a `lensi_*`
 *     prefix and are not exported (ADR-0031).
 *   - Every widget call computes a stable lens_id from an id stack; the
 *     id keys the retained store (ADR-0026, ADR-0027).
 *   - lens draws only through <flux/canvas.h> (ADR-0025).
 *   - Per frame:
 *       lens_begin(ui, &input) ->
 *       build (lens_row / lens_button / ...) ->
 *       lens_end(ui)            (reconcile, layout, interaction) ->
 *       flux_canvas_begin(...) -> lens_render(ui, canvas) -> flux_canvas_end(...)
 */

#ifndef LENS_H
#define LENS_H

#include <flux/canvas.h>
#include <flux/core.h>
#include <flux/math.h>
#include <lens/icon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */

#if defined(_WIN32) && !defined(LENS_STATIC)
#ifdef LENS_BUILDING
#define LENS_API __declspec(dllexport)
#else
#define LENS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define LENS_API __attribute__((visibility("default")))
#else
#define LENS_API
#endif

#define LENS_VERSION_MAJOR 0
#define LENS_VERSION_MINOR 0
#define LENS_VERSION_PATCH 13

LENS_API const char *lens_version_string(void);

/* ================================================================== */
/*  Core types                                                        */
/* ================================================================== */

typedef struct lens lens;           /* opaque context (the retained core) */
typedef struct lens_node lens_node; /* opaque retained node slot          */
typedef struct lens_font lens_font; /* opaque font handle (text seam)     */

typedef uint64_t lens_id; /* stable widget identity; 0 == none  */

typedef enum lens_axis { LENS_ROW = 0, LENS_COLUMN = 1 } lens_axis;
typedef enum lens_align {
    LENS_START = 0,
    LENS_CENTER = 1,
    LENS_END = 2,
    LENS_STRETCH = 3
} lens_align;

typedef enum lens_mouse_button {
    LENS_MOUSE_LEFT = 0,
    LENS_MOUSE_RIGHT = 1,
    LENS_MOUSE_MIDDLE = 2,
} lens_mouse_button;
#define LENS_MOUSE_COUNT 3

typedef enum lens_node_phase {
    LENS_NODE_ENTERING = 0, /* first frame this id was seen        */
    LENS_NODE_STABLE = 1,   /* seen this frame and last frame      */
    LENS_NODE_LEAVING = 2,  /* not seen this frame; in grace window */
} lens_node_phase;

/* ================================================================== */
/*  Input snapshot (ADR-0029)                                         */
/* ================================================================== */

#define LENS_INPUT_MAX_KEYS 16

/* Portable key sentinels. The application maps platform keycodes to
 * these when filling lens_key_event.key. Codes outside this range pass
 * through untouched (custom shortcuts). */
#define LENS_KEY_ESCAPE 256
#define LENS_KEY_TAB 258
#define LENS_KEY_RETURN 259
#define LENS_KEY_BACKSPACE 260
#define LENS_KEY_DELETE 261
#define LENS_KEY_LEFT 262
#define LENS_KEY_RIGHT 263
#define LENS_KEY_HOME 264
#define LENS_KEY_END 265
#define LENS_KEY_UP 266
#define LENS_KEY_DOWN 267

/* Modifier masks for lens_input.mods. */
#define LENS_MOD_SHIFT (1u << 0)
#define LENS_MOD_CTRL (1u << 1)
#define LENS_MOD_ALT (1u << 2)
#define LENS_MOD_SUPER (1u << 3)

typedef struct lens_key_event {
    int key;      /* platform keycode */
    bool pressed; /* down edge if true, up edge if false */
    bool repeat;
} lens_key_event;

#define LENS_PREEDIT_MAX 64

typedef struct lens_input {
    uint32_t size; /* sizeof(lens_input); 0 = trust
                      full struct (forward-compat
                      guard, ADR-0036) */

    flux_point cursor; /* UI-space pixels */
    bool mouse_down[LENS_MOUSE_COUNT];
    bool mouse_pressed[LENS_MOUSE_COUNT];
    bool mouse_released[LENS_MOUSE_COUNT];
    float scroll_x, scroll_y; /* wheel-step deltas */

    uint32_t mods;      /* modifier bitmask */
    char text_utf8[32]; /* committed text this frame */

    lens_key_event keys[LENS_INPUT_MAX_KEYS];
    uint32_t key_count;

    flux_point display_size; /* layout root extent */
    float dt_seconds;        /* frame delta, drives animation */

    /* IME composition in progress this frame; empty when none (ADR-0036).
     * The committed result still arrives through `text_utf8`. */
    char preedit_utf8[LENS_PREEDIT_MAX];
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
} lens_input;

/* Host clipboard interface (ADR-0036). Supplied in lens_desc; optional.
 * Paste is asynchronous (matches Wayland wl_data_device): a request is
 * answered later by the host calling lens_paste. */
typedef struct lens_clipboard {
    void (*request_text)(void *user); /* -> lens_paste */
    void (*set_text)(const char *utf8, size_t len, void *user);
    void *user;
} lens_clipboard;

/* ================================================================== */
/*  Interaction result (ADR-0029; state bits ADR-0058)                */
/* ================================================================== */

/* Widget state bits, OR'd into lens_response.state. The interaction core
 * (lensi_interact) produces HOVERED/PRESSED/FOCUSED/FOCUS_VISIBLE/DISABLED
 * for every widget; each widget ORs in the bits only it can know —
 * SELECTED from a selection argument, ACTIVE from a toggle value, DRAGGED
 * while a captured pointer drives a value. FOCUS_VISIBLE is FOCUSED via
 * keyboard navigation (Tab traversal); a pointer press focuses without it,
 * so chrome can show a focus ring only for keyboard users. */
typedef enum lens_widget_state : uint32_t {
    LENS_STATE_NONE = 0,
    LENS_STATE_HOVERED = 1u << 0,       /* cursor over the widget          */
    LENS_STATE_PRESSED = 1u << 1,       /* held: captured by the pointer   */
    LENS_STATE_FOCUSED = 1u << 2,       /* holds keyboard focus            */
    LENS_STATE_FOCUS_VISIBLE = 1u << 3, /* focused via keyboard navigation */
    LENS_STATE_DISABLED = 1u << 4,      /* non-interactive                 */
    LENS_STATE_SELECTED = 1u << 5,      /* picked out of a set (selectable) */
    LENS_STATE_ACTIVE = 1u << 6,        /* on-state of a toggle            */
    LENS_STATE_DRAGGED = 1u << 7,       /* captured pointer drives a value */
} lens_widget_state;

typedef struct lens_response {
    lens_id id;
    flux_rect rect; /* last frame's final_rect (what was hit-tested) */
    bool hovered;
    bool pressed;        /* left button held this frame */
    bool clicked;        /* left press + release inside */
    bool right_clicked;  /* right press + release inside */
    bool middle_clicked; /* middle press + release inside */
    bool changed;        /* value mutated this frame */
    bool focused;
    uint32_t state;      /* lens_widget_state bits (ADR-0058) */
} lens_response;

/* Semantic cursor requested by the hovered Lens widget. Lens only reports
 * intent; the windowing host remains responsible for mapping it to the
 * platform cursor API once per frame. */
typedef enum lens_cursor_hint {
    LENS_CURSOR_DEFAULT = 0,
    LENS_CURSOR_POINTER,
    LENS_CURSOR_TEXT,
    LENS_CURSOR_RESIZE_EW,
    LENS_CURSOR_RESIZE_NS,
} lens_cursor_hint;

/* ================================================================== */
/*  Accessibility semantics (ADR-0035)                                */
/* ================================================================== */

typedef enum lens_role {
    LENS_ROLE_NONE = 0, /* decorative; not surfaced to assistive tech */
    LENS_ROLE_GROUP,    /* a semantic grouping container              */
    LENS_ROLE_LABEL,
    LENS_ROLE_BUTTON,
    LENS_ROLE_CHECKBOX,
    LENS_ROLE_SLIDER,
    LENS_ROLE_DISCLOSURE, /* collapsing header                         */
    LENS_ROLE_SCROLLAREA,
    LENS_ROLE_TEXTFIELD, /* reserved: text input                      */
    LENS_ROLE_TEXTAREA,  /* multi-line text input                   */
    LENS_ROLE_MENU,      /* popup menu (ADR-0040)                     */
    LENS_ROLE_RADIO,
    LENS_ROLE_DIALOG, /* modal dialog window (ADR-0039)            */
    LENS_ROLE_PROGRESS, /* progress indicator; read-only value, NOT a slider */
    LENS_ROLE_TABLE,    /* grid container; rows/cells nest beneath   */
    LENS_ROLE_ROW,      /* a row inside a TABLE                      */
    LENS_ROLE_MENUITEM, /* item inside a MENU (distinct from BUTTON) */
    LENS_ROLE_LINK,     /* inline navigation action (distinct from BUTTON) */
} lens_role;

/* State bits for lens_semantics.flags. */
enum {
    LENS_A11Y_FOCUSED = 1u << 0,
    LENS_A11Y_DISABLED = 1u << 1,
    LENS_A11Y_CHECKED = 1u << 2,
    LENS_A11Y_EXPANDED = 1u << 3,
    LENS_A11Y_READONLY = 1u << 4,
    LENS_A11Y_SELECTED = 1u << 5, /* selected row/tab/item            */
};

typedef struct lens_semantics {
    lens_role role;
    const char *name;  /* accessible name; arena-owned for the frame */
    const char *value; /* slider readout, field contents, ...        */
    uint32_t flags;
} lens_semantics;

/* Override or enrich the semantics of the most recently built widget —
 * for icon-only controls whose visible label is not the accessible name.
 * Zeroed fields are left at the widget's default; `flags` are OR'd on. */
typedef struct lens_a11y_desc {
    lens_role role;    /* LENS_ROLE_NONE = keep the widget default */
    const char *name;  /* NULL = keep                            */
    const char *value; /* NULL = keep                            */
    uint32_t flags;    /* added to the existing state bits        */
} lens_a11y_desc;

LENS_API void lens_a11y(lens *ui, const lens_a11y_desc *desc);

/* Read-only walk of the retained tree, valid between lens_end and the
 * next lens_begin. Visits every node carrying non-decorative semantics
 * in pre-order with its solved bounds and its nearest semantic ancestor.
 * The platform AT-SPI bridge (or a test) consumes it; lens calls no
 * assistive API itself (same host separation as input, ADR-0029). */
typedef void (*lens_a11y_visit_fn)(const lens_semantics *s, flux_rect bounds, lens_id id,
                                   lens_id parent, void *user);
LENS_API void lens_accessibility_walk(const lens *ui, lens_a11y_visit_fn visit, void *user);

/* Request activation of a widget by id (ADR-0062) — the write direction of
 * the a11y seam. The host's AT bridge (iris's AT-SPI Action.DoAction)
 * calls this when an assistive client activates a control; lens records a
 * single pending activation id and the next frame's build reports
 * `clicked` for that node through the normal interaction path
 * (lensi_interact), so every focusable widget is activatable without
 * per-widget code.
 *
 * Semantics: the request fires once (single-shot). It is respected only
 * for a node that is focusable and not disabled when built; pointer
 * occlusion does not block it (AT users navigate the semantic tree, not
 * pixels). An unconsumed request is dropped at frame end — a widget that
 * vanished or is disabled simply never fires. Focus moves to the
 * activated node. Threading is the same as lens_paste: the host's main
 * thread, outside lens_begin/end is the intended window, though a call
 * mid-build takes effect for widgets built after it. */
LENS_API void lens_a11y_activate(lens *ui, lens_id id);

/* ================================================================== */
/*  Theme tokens (ADR / reference)                                    */
/* ================================================================== */

typedef struct lens_theme {
    uint32_t size; /* sizeof(lens_theme); forward-compat guard */

    flux_color color_bg;
    flux_color color_fg;
    flux_color color_accent;
    flux_color color_border;
    flux_color color_hover;
    flux_color color_active; /* pressed and selected surface fill */
    flux_color color_disabled;
    flux_color color_error;

    float padding;
    float gap;
    float corner_radius;
    float border_width;

    lens_font *font;
    float font_size;

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
    flux_color color_scrollbar_track;
    flux_color color_scrollbar_thumb;
    flux_color color_scrollbar_thumb_hover;
    flux_color color_scrollbar_thumb_active;

    /* Slider styling. Non-positive geometry tokens fall back to 6 logical px
     * for the track and 14 logical px for the knob. Zero-valued colour tokens
     * inherit color_border / color_accent / color_fg during theme
     * normalisation. Keeping the knob separate from the fill lets value
     * controls use a high-contrast handle without losing the application's
     * accent on the filled range. */
    float slider_track_thickness;
    float slider_knob_size;
    flux_color color_slider_track;
    flux_color color_slider_fill;
    flux_color color_slider_knob;
} lens_theme;

LENS_API lens_theme lens_theme_default(void);
LENS_API lens_theme lens_theme_dark(void);

/* ================================================================== */
/*  Style cascade (ADR-0058, amended by ADR-0061)                     */
/*                                                                    */
/*  A lens_style is a sparse set of style ATOMS. Every field is       */
/*  optional: the `fields` mask names the fields that carry values.   */
/*  The mask doubles as the forward-compat guard: a caller compiled   */
/*  against an older header never sets bits it does not know, and the */
/*  library never reads fields whose bit is clear.                    */
/*                                                                    */
/*  Resolution is one fixed per-field cascade (ADR-0061):             */
/*      per-call style (lens_box.style)                               */
/*        > nearest enclosing scope (lens_push_style)                 */
/*        > theme                                                     */
/*  Unset fields fall through each layer; the resolver's derivation   */
/*  order (hover/pressed derivation, disabled dim) is unchanged and   */
/*  remains the only adjustment site. With nothing set anywhere the   */
/*  output is the verbatim theme, so default rendering is unchanged.  */
/* ================================================================== */

/* Field tags for lens_style.fields — one bit per optional field. */
typedef enum lens_style_field : uint32_t {
    LENS_STYLE_BG = 1u << 0,             /* resting surface           */
    LENS_STYLE_BG_HOVER = 1u << 1,       /* hovered surface           */
    LENS_STYLE_BG_PRESSED = 1u << 2,     /* pressed/selected surface  */
    LENS_STYLE_FG = 1u << 3,             /* foreground (text, glyphs) */
    LENS_STYLE_BORDER = 1u << 4,         /* border stroke             */
    LENS_STYLE_ACCENT = 1u << 5,         /* accent (emphasis colour)    */
    LENS_STYLE_CORNER_RADIUS = 1u << 6,
    LENS_STYLE_BORDER_WIDTH = 1u << 7,
    LENS_STYLE_PADDING = 1u << 8,
    LENS_STYLE_GAP = 1u << 9,
    LENS_STYLE_FONT_SIZE = 1u << 10,
    LENS_STYLE_OUTLINE_COLOR = 1u << 12, /* foreground contour colour  */
    LENS_STYLE_OUTLINE_WIDTH = 1u << 13, /* contour radius, logical px */
} lens_style_field;

typedef struct lens_style {
    uint32_t fields; /* LENS_STYLE_* mask: which fields below are set */
    flux_color bg;
    flux_color bg_hover;
    flux_color bg_pressed;
    flux_color fg;
    flux_color border;
    flux_color accent;
    float corner_radius;
    float border_width;
    float padding;
    float gap;
    float font_size;
    /* Opt-in contour behind foreground content (text, glyphs, images)
     * floating over imagery or translucent material. State-independent
     * decoration atoms: the resolver neither derives nor dims them, and
     * no theme token exists — unset means none. */
    flux_color outline_color;
    float outline_width;
} lens_style;

/* Empty style: nothing set, everything resolves to the theme. */
#define LENS_STYLE_INIT {.fields = 0}

static inline lens_style lens_style_init(void) {
    return (lens_style)LENS_STYLE_INIT;
}

/* The output of style resolution (ADR-0058/0061): lens_style with every
 * slot concrete — the per-call > scope > theme cascade, hover/pressed
 * derivation, and the disabled dim have already been applied in the
 * resolver's documented order, so a consumer reads slots directly.
 * `disabled` is the designed disabled-surface colour; lens_style has no
 * instance slot for it, so it is always the theme token. The outline slots
 * resolve to transparent/0 when unset (no theme tokens exist for them).
 * Skins (ADR-0059) receive this in the widget record. */
typedef struct lens_style_resolved {
    flux_color bg;
    flux_color bg_hover;
    flux_color bg_pressed;
    flux_color fg;
    flux_color border;
    flux_color accent;
    flux_color disabled;
    float corner_radius;
    float border_width;
    float padding;
    float gap;
    float font_size;
    flux_color outline_color;
    float outline_width;
} lens_style_resolved;

/* Scoped style stack (ADR-0061 item 4): every widget declared between
 * lens_push_style and lens_pop_style resolves its unset style atoms
 * against the merged scope — the primitive from which callers build their
 * own design-system scopes ("danger", "sidebar"). This is a strictly
 * nested scope, NOT a floating "applies to the next widget" modifier (the
 * ordering hazard lens_box's documentation rejects): it affects exactly
 * the widgets declared between push and pop, including the terse forms,
 * and lens_begin resets the stack every frame so a forgotten pop cannot
 * leak across frames. Overflow of the scope stack flags lens_overflowed. */
LENS_API void lens_push_style(lens *ui, lens_style style);
LENS_API void lens_pop_style(lens *ui);

/* ================================================================== */
/*  Text seam (ADR-0033)                                              */
/* ================================================================== */

/* Typeface family for subsequently built widgets. Values mirror
 * flux_text_family so the seam can cast between them. LENS_TEXT_FAMILY_DEFAULT
 * keeps the engine's default (sans-serif). Like lens_set_theme, this is a
 * context switch read at widget-build time: set it, build the widgets that
 * need another voice (e.g. a serif display title), then set it back. */
typedef enum lens_text_family {
    LENS_TEXT_FAMILY_DEFAULT = 0,
    LENS_TEXT_FAMILY_SANS = 1,
    LENS_TEXT_FAMILY_SERIF = 2,
    LENS_TEXT_FAMILY_MONO = 3,
} lens_text_family;

LENS_API void lens_set_text_family(lens *ui, lens_text_family family);
LENS_API lens_text_family lens_get_text_family(const lens *ui);

typedef struct lens_text_metrics {
    float width;
    float height;
    float baseline; /* from top */
} lens_text_metrics;

/* The only text entry point layout (ADR-0028 pass 1) may call. Backed
 * by a monospace metrics stub until flux core ships canvas text. */
LENS_API lens_text_metrics lens_text_measure(lens *ui, lens_font *font, const char *utf8,
                                             float size_px);
LENS_API lens_text_metrics lens_text_measure_ex(lens *ui, lens_font *font, const char *utf8,
                                                float size_px, float weight);

/* ================================================================== */
/*  Widget skins (ADR-0059)                                           */
/*                                                                    */
/*  A widget's last phase — emission of its draw commands — is a      */
/*  replaceable skin function. The widget keeps identity, measuring,  */
/*  interaction, animation, and accessibility; the skin receives a    */
/*  plain-data record (kind + state + bounds + resolved style +       */
/*  content) and is the only code that writes draw commands. Skins    */
/*  are replaceable context-wide (lens_set_skin); NULL restores the   */
/*  built-in default, so default rendering is unchanged.              */
/* ================================================================== */

typedef enum lens_widget_kind : uint32_t {
    LENS_WIDGET_BUTTON = 0,
    LENS_WIDGET_SELECTABLE = 1,
    LENS_WIDGET_CHECKBOX = 2,
    LENS_WIDGET_SWITCH = 3,
    LENS_WIDGET_RADIO = 4,
    LENS_WIDGET_SLIDER = 5,      /* horizontal and vertical share a kind */
    LENS_WIDGET_ICON_BUTTON = 6, /* the lens_icon_button* family         */
    LENS_WIDGET_TABS = 7,        /* one record per strip per frame       */
    LENS_WIDGET_LABEL = 8,       /* label/title/heading, incl. wrapped   */
    LENS_WIDGET_SEPARATOR = 9,
    LENS_WIDGET_ICON = 10,       /* bare glyph (lens_icon)               */
    LENS_WIDGET_IMAGE = 11,      /* static image and the image buttons   */
    LENS_WIDGET_PROGRESS = 12,
    LENS_WIDGET_TEXTFIELD = 13,
    LENS_WIDGET_TEXTAREA = 14,
    LENS_WIDGET_COLLAPSING = 15,
    LENS_WIDGET_TREE = 16,
    LENS_WIDGET_TABLE = 17,
    LENS_WIDGET_SPLIT = 18,
    LENS_WIDGET_MENU_ITEM = 19,  /* bar trigger, item, submenu, separator */
    LENS_WIDGET_DROPDOWN = 20,   /* the trigger; the popup is place+cascade */
    LENS_WIDGET_LINK = 21,
    LENS_WIDGET_KIND_COUNT
} lens_widget_kind;

/* One wrapped line of text for a LENS_WIDGET_LABEL record (and the visible
 * body lines of a LENS_WIDGET_TEXTAREA record): the widget owns wrapping /
 * windowing (behaviour), the skin only draws. Strings are borrowed for the
 * frame; coordinates are node-local. */
typedef struct lens_text_line {
    const char *text;
    float x, y;
} lens_text_line;

/* Table grid payload for LENS_WIDGET_TABLE (the visible window only — the
 * virtualization stays in the widget, ADR-0042). */
typedef struct lens_grid_column {
    const char *title; /* frame-borrowed; NULL = no title cell   */
    float x, w;        /* node-local column band                */
    lens_align align;  /* cell text alignment within the band   */
} lens_grid_column;

typedef struct lens_grid_row {
    const char **cells;   /* column_count strings, frame-borrowed      */
    const float *cell_x;  /* per-cell text x, node-local, precomputed
                             (alignment resolved by the widget — a skin
                             never measures text). A cell with an icon
                             is shifted right past the glyph box, so the
                             icon lands at cell_x - (font_size + 8)    */
    const lens_icon_id *icons; /* per-cell glyph ids, parallel to cells
                             (ADR-0066); NULL when the table was built
                             without an icon callback. Only
                             LENS_START-aligned columns carry icons   */
    float y;              /* node-local top edge                       */
    int index;            /* absolute row index (zebra parity)         */
    uint32_t state;       /* LENS_STATE_SELECTED when selected;
                             LENS_STATE_FOCUSED on the cursor row     */
} lens_grid_row;

/* Per-tab data for a LENS_WIDGET_TABS record (ADR-0061): the strip gets one
 * skin call per frame, carrying every tab the skin needs to draw. Strings
 * are borrowed for the frame. */
typedef struct lens_tab_item {
    const char *label;      /* tab text                                      */
    lens_text_metrics text; /* measured label; a skin never re-measures      */
    uint32_t state;         /* this tab's lens_widget_state bits (ADR-0058)  */
    float hover_t;          /* eased hover [0,1], updated this frame         */
    flux_rect last_bounds;  /* last frame's arranged rect, UI space          */
} lens_tab_item;

/* Per-kind content payload for lens_widget_record. A member is valid only
 * for the kinds its comment lists; everything else is zero. Strings are
 * borrowed for the frame (arena-backed); `text` metrics come from the
 * widget's own measure pass, so a skin never re-measures the label. */
typedef struct lens_widget_content {
    const char *label;       /* BUTTON SELECTABLE CHECKBOX SWITCH RADIO SLIDER
                                LABEL LINK COLLAPSING TREE MENU_ITEM DROPDOWN */
    lens_text_metrics text;  /* measured label (same kinds + TEXTFIELD: the
                                "Ag" line metrics the caret height reads)    */
    float text_size;         /* LABEL: explicit point size; 0 = resolved style */
    float text_weight;       /* LABEL: 0 = default weight                    */
    bool compact;            /* LABEL: unpadded form (lens_label_compact_ex) */
    const lens_text_line *lines; /* LABEL (wrapped) / TEXTAREA: line slices  */
    int line_count;              /* LABEL (wrapped) / TEXTAREA               */
    const char *description; /* SWITCH: supporting text; NULL = none           */
    lens_text_metrics desc_text; /* SWITCH: measured description               */
    lens_icon_id icon;       /* SELECTABLE / ICON_BUTTON: leading glyph;
                                DROPDOWN: the chevron; LENS_ICON_INVALID = none */
    float glyph_size;        /* ICON_BUTTON / ICON: glyph box, logical px    */
    const char *badge;       /* ICON_BUTTON: corner badge; NULL/empty = none   */
    bool rounded;            /* ICON_BUTTON: rounded-tile variant              */
    bool active_surface;     /* ICON_BUTTON: steady active tint variant        */
    bool accent_checked;     /* ICON_BUTTON: accent glyph while checked        */
    float ratio;             /* SLIDER / PROGRESS / SPLIT: fraction in [0,1]   */
    bool vertical;           /* SLIDER / SEPARATOR / SPLIT: orientation        */
    bool error;              /* SLIDER / TEXTFIELD / TEXTAREA: error styling   */
    const lens_tab_item *tabs; /* TABS: per-tab array, `tab_count` entries   */
    int tab_count;             /* TABS                                        */
    int active_index;          /* TABS: selected tab, clamped into range      */
    bool expanded;             /* COLLAPSING / TREE: body open                */
    bool leaf;                 /* TREE: leaf row (dot, no chevron)            */
    const char *shortcut;        /* MENU_ITEM: trailing hint; NULL = none    */
    lens_text_metrics shortcut_text; /* MENU_ITEM: measured shortcut         */
    bool menu_check;             /* MENU_ITEM: draw a check glyph            */
    bool menu_radio;             /* MENU_ITEM: the glyph is a radio dot      */
    bool menu_separator;         /* MENU_ITEM: divider row; rest unused      */
    bool menu_trigger;           /* MENU_ITEM: a menu-bar trigger            */
    bool submenu;                /* MENU_ITEM: trailing submenu chevron      */
    bool popup_open;             /* MENU_ITEM (trigger) / DROPDOWN: open     */
    flux_image *image;           /* IMAGE: host texture, frame-borrowed      */
    flux_color tint;             /* IMAGE: premultiplied modulation          */
    bool image_button;           /* IMAGE: the interactive button variant    */
    float split_pos;             /* SPLIT: divider main offset, node-local   */
    float split_thickness;       /* SPLIT: handle strip thickness            */
    const lens_grid_column *columns; /* TABLE: column bands                 */
    int column_count;                /* TABLE                                */
    const lens_grid_row *rows;       /* TABLE: the visible row window       */
    int row_count;                   /* TABLE: visible rows                 */
    float header_height;             /* TABLE: 0 = header hidden            */
    float row_height;                /* TABLE                                */
    float view_width;                /* TABLE: body width minus scrollbar   */
    bool zebra;                      /* TABLE: alternate-row tint enabled   */
    flux_rect scrollbar_track;       /* TABLE: valid when has_scrollbar     */
    flux_rect scrollbar_thumb;       /* TABLE: valid when has_scrollbar     */
    bool has_scrollbar;              /* TABLE                                */
    uint32_t scrollbar_state;        /* TABLE: HOVERED/DRAGGED thumb bits   */
    /* TEXTFIELD / TEXTAREA: the text-edit payload. Every geometry is
     * node-local and precomputed by the widget — a skin never shapes text.
     * The platform caret reporting (lensi_set_caret_rect, ADR-0036) stays
     * in the widget; it is behaviour, not chrome. */
    const char *edit_text;      /* TEXTFIELD: display string (buffer, or the
                                   buffer with the IME preedit composed in);
                                   TEXTAREA placeholder travels in lines[].
                                   NULL = nothing to draw                    */
    float edit_text_y;          /* TEXTFIELD: node-local y for edit_text     */
    bool show_placeholder;      /* edit_text / lines hold the placeholder:
                                   draw it in the disabled colour           */
    const flux_rect *sel_rects; /* selection highlight quads                */
    int sel_rect_count;
    flux_rect caret;            /* caret quad; valid when show_caret         */
    bool show_caret;
    flux_rect preedit_underline; /* IME composition underline; valid when
                                   has_preedit                               */
    bool has_preedit;
} lens_widget_content;

/* Everything a skin needs to draw one widget for one frame.
 *
 * `bounds` is the widget's content box in NODE-LOCAL coordinates — the
 * same space the skin's emission calls (and lens_draw_cmd.rel) use. Its
 * origin is always (0,0) because the final position is solved by layout
 * after the skin runs; w/h are this frame's measured size (fixed-size
 * hints applied). `last_bounds` is last frame's arranged rect in UI
 * space — what interaction hit-tested — zeroed on the widget's first
 * frame (the documented one-frame latency, ADR-0029). Skins that scale
 * chrome with the arranged size (the slider track) read last_bounds. */
typedef struct lens_widget_record {
    lens_widget_kind kind;
    uint32_t state;            /* lens_widget_state bits (ADR-0058) */
    flux_rect bounds;          /* node-local content box            */
    flux_rect last_bounds;     /* last frame's arranged rect        */
    lens_style_resolved style; /* resolved instance/theme style     */
    uint32_t style_fields;     /* LENS_STYLE_* bits the instance set;
                                  0 = everything came from the theme */
    float hover_t;             /* eased hover, updated this frame   */
    float active_t;            /* eased active, updated this frame  */
    lens_widget_content content;
} lens_widget_record;

/* A skin draws one widget for one frame. It must not retain the record,
 * the node, or content strings past the call. `node` is the widget's
 * retained slot — skins push commands onto it and may read its public
 * accessors (e.g. lens_node_bounds), nothing more. */
typedef void (*lens_skin_fn)(lens *ui, lens_node *node, const lens_widget_record *rec);

/* Replace the skin for a widget kind context-wide; NULL restores the
 * built-in default. The context is the single override granularity
 * (ADR-0061 retired the per-call *_skinned forms): for a one-off override,
 * set the skin, build the widget, restore NULL. */
LENS_API void lens_set_skin(lens *ui, lens_widget_kind kind, lens_skin_fn fn);
/* The built-in default skin for a kind — for wrapping: call it from a
 * custom skin to keep the stock chrome, then add your own. */
LENS_API lens_skin_fn lens_default_skin(lens_widget_kind kind);

/* Per-node scratch storage for skins (ADR-0061 item 9): four retained
 * floats, zeroed on the node's first touch, living and dying with the
 * node (ADR-0038's GC reclaims the node and the scratch goes with it).
 * This is mechanism, not animation: the library provides storage so a
 * caller-owned skin can carry its own state (a spring indicator's
 * position/velocity) with no library-side allocation or lifetime hazard;
 * the library never animates anything itself. */
LENS_API float *lens_skin_scratch(lens *ui, lens_node *node);

/* Skin emission seam — the skin's pen. Rects are node-local, same space
 * as the record's `bounds`; a zero w/h rect spans the full node box. For
 * text, a negative rel.w centres horizontally in the resolved node rect
 * and a negative rel.h centres vertically (centring is resolved at render
 * time, so it stays correct when a parent constrains the node). */
LENS_API void lens_skin_rect(lens *ui, lens_node *node, flux_rect rel, flux_color color,
                             float radius);
LENS_API void lens_skin_border(lens *ui, lens_node *node, flux_rect rel, flux_color color,
                               float radius, float width);
LENS_API void lens_skin_text(lens *ui, lens_node *node, flux_rect rel, flux_color color,
                             const char *utf8, float size_px, float weight);
LENS_API void lens_skin_icon(lens *ui, lens_node *node, flux_rect rel, flux_color color,
                             float stroke, lens_icon_id icon);
/* Nested logical clip for skins that clip sub-regions (the table's cells).
 * Balanced push/pop; the rect is intersected with the enclosing clip at
 * render time. */
LENS_API void lens_skin_clip_push(lens *ui, lens_node *node, flux_rect rel);
LENS_API void lens_skin_clip_pop(lens *ui, lens_node *node);

/* ================================================================== */
/*  Context lifecycle (ADR-0032)                                      */
/* ================================================================== */

typedef struct lens_desc {
    flux_device *device;      /* retained; persistent allocator source.
                                 NULL = use libc malloc (headless/tests,
                                 render unavailable). */
    lens_theme theme;         /* initial token set; zeroed = default */
    size_t arena_bytes;       /* per-frame arena capacity; 0 = default */
    uint32_t store_capacity;  /* initial node-store slots; 0 = default */
    float scale;              /* device-pixel scale; 0 = 1.0          */
    lens_clipboard clipboard; /* optional host clipboard (ADR-0036)   */
} lens_desc;

FLUX_NODISCARD LENS_API flux_result lens_create(const lens_desc *desc, lens **out);
LENS_API void lens_destroy(lens *ui);
LENS_API void lens_set_theme(lens *ui, lens_theme theme);
LENS_API lens_theme lens_get_theme(const lens *ui);
LENS_API float lens_dt(const lens *ui); /* frame delta, seconds */

/* Device-pixel scale (HiDPI). The application reports the compositor /
 * window-system scale here; layout, input, and `lens_input.display_size`
 * stay in *logical* pixels, and `lens_render` scales the canvas
 * transform by this factor so 1 logical pixel maps to `scale` device
 * pixels. Default 1.0. */
LENS_API void lens_set_scale(lens *ui, float scale);
LENS_API float lens_scale(const lens *ui);

/* ================================================================== */
/*  Frame lifecycle (ADR-0024, frame-lifecycle.md)                    */
/* ================================================================== */

LENS_API void lens_begin(lens *ui, const lens_input *input);
LENS_API void lens_end(lens *ui);
FLUX_NODISCARD LENS_API flux_result lens_render(lens *ui, flux_canvas *canvas);

/* True if the per-frame arena overflowed during the frame just built. */
LENS_API bool lens_overflowed(const lens *ui);

/* True if an eased value (hover/active fade, …) was still in transit during
 * the frame just built. An input-driven host that only paints on events should
 * schedule one more frame while this holds so animations settle to rest instead
 * of freezing mid-fade when input stops. Valid between lens_end and the next
 * lens_begin. */
LENS_API bool lens_anim_pending(const lens *ui);

/* True if the frame just built would paint anything different from what is
 * already on screen: base-tree or placed-subtree damage (geometry, draw
 * lists, lifecycle), an appearing/disappearing transient node or tooltip,
 * an eased value still in transit, or a focused text caret that needs its
 * blink clock. A damage-driven host may skip the whole acquire/paint/present
 * cycle when this returns false. Read-only; valid between lens_end and the
 * next lens_begin. Resize/scale changes are host-visible, not lens state —
 * the host must still paint the first frame after those itself. */
LENS_API bool lens_frame_needs_repaint(const lens *ui);

/* Accessibility reduced-motion switch. When enabled, every eased value in
 * lens resolves to its target within one frame — no fades, slides, or other
 * transitions — and lens_anim_pending stays false. The host owns the policy
 * (user preference); lens executes it. Default false. */
LENS_API void lens_set_reduced_motion(lens *ui, bool reduced);
LENS_API bool lens_reduced_motion(const lens *ui);

/* ================================================================== */
/*  Identity (ADR-0026)                                               */
/* ================================================================== */

LENS_API void lens_push_id(lens *ui, const char *seed);
LENS_API void lens_push_id_int(lens *ui, int64_t seed);
LENS_API void lens_pop_id(lens *ui);
LENS_API lens_id lens_current_id(const lens *ui, const char *label);

/* ================================================================== */
/*  Identity & per-widget options                                     */
/* ================================================================== */

/* lens_box — per-widget layout, identity, and state, common to every
 * `*_ex` descriptor. Each field applies to *this* call only; there are
 * no floating "applies to the next/last widget" modifiers, so there is
 * no ordering hazard. Zeroed fields take the widget default.
 *
 * Identity: a widget's stable id is derived from `id` if set, else from
 * its visible label. Give an explicit `id` when two widgets would
 * otherwise share a label, or when the label is empty/dynamic. (The
 * legacy "label##key" suffix is still honoured by the terse forms, but
 * `id` is the documented, explicit way.) */
typedef struct lens_box {
    const char *id;      /* explicit identity; NULL = derive from label */
    float flex;          /* main-axis grow factor; 0 = don't grow       */
    float width;         /* fixed width, logical px;  0 = intrinsic      */
    float height;        /* fixed height, logical px; 0 = intrinsic      */
    bool disabled;       /* non-interactive + dimmed                     */
    bool error;          /* validation-error styling (input widgets)     */
    const char *tooltip; /* shown while this widget is hovered; NULL=none*/
    lens_style style;    /* per-call style atoms (ADR-0061);
                            fields == 0 = inherit scope/theme           */
} lens_box;

/* ================================================================== */
/*  Containers / layout (ADR-0028)                                    */
/* ================================================================== */

typedef struct lens_layout_opts {
    lens_box box;     /* identity, flex, and fixed size of the container
                       itself (.disabled/.error/.tooltip unused here) */
    float min_width;  /* minimum resolved width; 0 = no lower bound */
    float max_width;  /* maximum resolved width; 0 = no upper bound */
    float min_height; /* minimum resolved height; 0 = no lower bound */
    float max_height; /* maximum resolved height; 0 = no upper bound */
    float gap;        /* between children, main axis */
    float pad;        /* inside the container, all sides */
    lens_align cross; /* cross-axis alignment; LENS_STRETCH fills */
    flux_color bg;    /* background fill; alpha 0 = no background */
    float radius;     /* corner radius for the background fill */
    flux_color border;  /* border stroke; alpha 0 = no border */
    float border_width; /* border stroke width when border alpha > 0 */
} lens_layout_opts;

LENS_API void lens_row(lens *ui);
LENS_API void lens_column(lens *ui);
LENS_API void lens_row_ex(lens *ui, lens_layout_opts opts);
LENS_API void lens_column_ex(lens *ui, lens_layout_opts opts);
LENS_API void lens_close(lens *ui); /* close the current container */

/* A composable row with one button-sized interaction target around all of
 * its children. The returned response is resolved against the complete row,
 * not individual labels/images inside it. The row remains open for child
 * declarations until lens_pressable_end. `id` supplies stable identity while
 * `label` is exposed to accessibility; either may be an empty string. */
LENS_API lens_response lens_pressable_begin(lens *ui, const char *id, const char *label,
                                            lens_layout_opts opts);
LENS_API void lens_pressable_end(lens *ui);

/* Positional layout hints applied to the *next* node (widget OR
 * container). Unlike state/styling, these are purely about placement and
 * have no "last widget" counterpart, so there is no ordering ambiguity.
 * For the descriptor (`*_ex`) forms, prefer the equivalent lens_box.flex /
 * lens_box.width / lens_box.height fields instead. */
LENS_API void lens_flex(lens *ui, float grow);       /* next node's main-axis grow factor */
LENS_API void lens_size(lens *ui, float w, float h); /* next node's fixed size (0 = intrinsic) */
LENS_API void lens_spacer(lens *ui, float size);     /* fixed empty main-axis gap */

/* ================================================================== */
/*  Widgets — terse forms (ADR-0031)                                  */
/*                                                                    */
/*  The common case: label doubles as the stable id, no styling. The  */
/*  bool return means: button -> clicked; checkbox/slider/radio/text  */
/*  field/textarea/dropdown -> value changed this frame; collapsing/  */
/*  tab -> currently open/active. For an explicit id, fixed size,     */
/*  flex, disabled/error state, a tooltip, a per-call style, or a     */
/*  placeholder — and for the full lens_response — use the matching   */
/*  `*_ex` form below.                                                */
/* ================================================================== */

LENS_API bool lens_button(lens *ui, const char *label);
/* Inline text action for breadcrumbs and secondary navigation. It has no
 * surface at rest and indicates hover/focus with an accent underline without
 * changing the text's size or weight. */
LENS_API bool lens_link(lens *ui, const char *label);
/* A borderless, full-width list / nav item. Transparent at rest, with a subtle
 * hover fill and a steady `color_active` surface when `selected` — that tint
 * is the neutral selection affordance; decorative rails/indicators belong to
 * caller skins (ADR-0061). Returns true on the frame it is clicked. Use it
 * for sidebar lists where a stack of filled lens_buttons would read as
 * bordered pills. Per-call restyling goes through the descriptor's box.style
 * or an enclosing lens_push_style scope (ADR-0061). */
LENS_API bool lens_selectable(lens *ui, const char *label, bool selected);
LENS_API bool lens_selectable_icon(lens *ui, lens_icon_id icon, const char *label, bool selected);
/* A standalone padded label. The text is drawn centred vertically within
 * the RESOLVED node box (the replay-time convention shared with
 * lens_heading), so it stays centred even when a fixed-height parent row
 * constrains the node below its intrinsic padded height. */
LENS_API void lens_label(lens *ui, const char *text);
LENS_API void lens_label_ex(lens *ui, const char *text, float size);
/* A label constrained to max_width logical pixels. Text wraps at whitespace
 * when possible and falls back to UTF-8 boundaries for long tokens. */
LENS_API void lens_label_wrapped(lens *ui, const char *text, float max_width);
LENS_API void lens_label_wrapped_ex(lens *ui, const char *text, float size, float max_width);
LENS_API void lens_label_compact_ex(lens *ui, const char *text, float size);
LENS_API void lens_title(lens *ui, const char *text);
LENS_API void lens_heading(lens *ui, const char *text, int level);
LENS_API bool lens_checkbox(lens *ui, const char *label, bool *value);
/* Full-width settings row with a trailing platform-style switch. The optional
 * description is available on the descriptor form below. */
LENS_API bool lens_switch(lens *ui, const char *label, bool *value);
/* Horizontal value control. The resting track omits its knob; hover, keyboard
 * focus, or dragging reveals it with the framework's seek-safe transition. */
LENS_API bool lens_slider(lens *ui, const char *label, float *value, float min, float max);
/* Vertical value control. `min` is at the bottom and `max` is at the top.
 * Hovered wheel input and Up/Down keys adjust by `step`; a non-positive step
 * defaults to one twentieth of the range. */
LENS_API bool lens_slider_vertical(lens *ui, const char *label, float *value, float min, float max,
                                   float step);
/* Apply the current vertical wheel delta to `value` when the most recently
 * built widget is hovered. Consumes that delta so an ancestor scroll area
 * cannot also move. Useful for compact value triggers that expand elsewhere. */
LENS_API bool lens_adjust_float_on_scroll(lens *ui, float *value, float min, float max, float step);
LENS_API bool lens_radio(lens *ui, const char *label, int *value, int option_value);
LENS_API bool lens_textfield(lens *ui, const char *label, char *buf, size_t buf_cap);
/* Move the caret / selection of the text field identified by `label` in the
 * current id scope. Offsets are BYTE offsets into the edit buffer, not
 * character indices.
 *
 * Call before the field's lens_textfield build in the same frame (or on an
 * earlier frame) — typically right after programmatically rewriting the
 * buffer for Tab completion or a pre-filled value. The write is
 * unconditional: it wins over the field's remembered position for that
 * frame, then the field's own editing takes over again.
 *
 * Out-of-range offsets clamp to the buffer length and offsets that land
 * mid-character snap back to a UTF-8 boundary, both at the next build.
 * Select-all is anchor 0 + caret UINT32_MAX (the caret clamps to the buffer
 * length).
 *
 * The label is resolved with find-or-create, so calling before the field's
 * first-ever frame works — the state waits in the store until the field
 * appears. A label whose field never appears is dropped after the store's
 * leaving-node grace frames (ADR-0038).
 *
 * While an IME preedit is active the field manages its own caret and
 * selection (it clears the selection each focused preedit frame), so host
 * writes during a preedit have no visible effect. */
LENS_API void lens_textfield_set_caret(lens *ui, const char *label, uint32_t caret);
LENS_API void lens_textfield_set_selection(lens *ui, const char *label, uint32_t anchor,
                                           uint32_t caret);
LENS_API bool lens_textarea(lens *ui, const char *label, char *buf, size_t buf_cap, float min_h);
/* Select trigger with a trailing vector chevron and an opaque floating option
 * surface. Re-clicking the trigger closes once; a popup inside a scroll area
 * inherits that viewport as its placement boundary. The list opens below the
 * trigger when it fits there, flips above otherwise, and is height-capped to
 * the roomier side (at most ~7 rows) with its own scrolling — a wheel over
 * the list scrolls it, a wheel anywhere else closes the popup. A positive
 * height supplied through lens_size/lens_box is raised when needed for its
 * content. */
LENS_API bool lens_dropdown(lens *ui, const char *label, int *selected, const char **items,
                            int count);
LENS_API bool lens_collapsing(lens *ui, const char *label);
/* Force the expanded state of a collapsing section identified by `label`.
 * Call before lens_collapsing on the same label. Intended for hosts that
 * persist the open/closed state across restarts: call it on the first
 * frame to seed the state, then let lens's retained store take over.
 * Has no effect if called after the user has already toggled the section
 * (lens_collapsing reads + toggles the state; this just pre-seeds it). */
LENS_API void lens_collapsing_set_open(lens *ui, const char *label, bool open);

/* ------------------------------------------------------------------ */
/*  Tree (ADR: lens tree widget)                                       */
/* ------------------------------------------------------------------ */

/* A tree is a stack of nested disclosure rows. lens_tree_node returns true
 * when the row is open; the host then opens child rows inside the same
 * lens_column / lens_close block. The default open state is closed; pre-seed
 * with lens_tree_node_set_open for "expand on first appearance" UX.
 *
 * Indentation is applied automatically — each nested lens_tree_node call
 * shifts its header right by t->padding. A leaf (no children) renders a
 * dot instead of a chevron; pass leaf=true to force leaf appearance.
 *
 * Example:
 *   if (lens_tree_node(ui, "root", false)) {
 *       if (lens_tree_node(ui, "child1", false)) { ... }
 *       if (lens_tree_node(ui, "child2", false)) { ... }
 *   }
 *
 * The returned value stays consistent across the begin/end pair; do not
 * call lens_close in response to it. lens_tree_node is a self-closing
 * widget (no separate end); nest begin/end pairs by re-entering with a
 * child label while the parent is still in its "open" body scope. */
LENS_API bool lens_tree_node(lens *ui, const char *label, bool leaf);

/* Pre-seed / force the open state of the tree node identified by `label`
 * in the current id scope. Symmetrical with lens_collapsing_set_open. */
LENS_API void lens_tree_node_set_open(lens *ui, const char *label, bool open);

/* Close the current tree node body. Each lens_tree_node that returned true
 * must be balanced with one lens_tree_node_end before its parent's body
 * closes. (Implementation note: this is a thin alias to lens_close so the
 * host reads symmetrically.) */
LENS_API void lens_tree_node_end(lens *ui);

LENS_API void lens_scroll_begin(lens *ui, const char *id);
LENS_API void lens_scroll_end(lens *ui);
/* Programmatically position a scroll area identified in the current id scope.
 * Call after its begin/end body in the same frame (or on a later frame). The
 * layout pass clamps both offsets to the resolved content bounds. Unknown ids
 * are ignored, so callers may issue the request while content is appearing. */
LENS_API void lens_scroll_to(lens *ui, const char *id, float x, float y);

/* Current offset of a scroll area in the current id scope; false when the
 * id does not resolve to a scroll area (e.g. first frame). Either output
 * pointer may be NULL. */
LENS_API bool lens_scroll_offset(lens *ui, const char *id, float *x, float *y);

typedef struct lens_tabs_opts {
    bool equal_width; /* divide available width into equal hit targets */
} lens_tabs_opts;

/* Horizontal selection strip (ADR-0061: emission lives behind the
 * LENS_WIDGET_TABS skin seam; the default skin is the neutral static
 * indicator — theme accent, fixed thickness, zero animation). Visual
 * tuning goes through the style cascade (box/scope/theme), not opts; a
 * different presentation is a caller-owned skin (see
 * examples/showcase/tabs_spring_skin.c for the spring recipe).
 * lens_tabs_begin_ex exists for the structural knobs (equal_width). Both
 * forms raise an undersized host height to contain the tallest tab
 * content. */
LENS_API bool lens_tabs_begin(lens *ui, const char *id, int *active_tab);
LENS_API bool lens_tabs_begin_ex(lens *ui, const char *id, int *active_tab, lens_tabs_opts opts);
LENS_API bool lens_tab(lens *ui, const char *label);
LENS_API void lens_tabs_end(lens *ui);

LENS_API void lens_progress(lens *ui, const char *label, float value);
LENS_API void lens_separator(lens *ui);

LENS_API void lens_icon(lens *ui, lens_icon_id id, float size);
/* A host-owned raster image drawn as a widget (e.g. an application icon).
 * `image` is borrowed for the frame — the caller owns it and it must remain
 * valid until `lens_render` returns. `w`/`h` are the desired logical size;
 * the image is scaled to fill the measured box. A zero dimension adopts the
 * other (so `lens_image(ui, img, 32, 0)` is a 32×32 square). Both zero falls
 * back to the theme font size. */
LENS_API void lens_image(lens *ui, flux_image *image, float w, float h);
/* As lens_image, with a premultiplied tint applied to the texture. Opaque
 * white preserves the source; white with a lower alpha fades it. */
LENS_API void lens_image_tinted(lens *ui, flux_image *image, float w, float h, flux_color tint);
/* Texture-backed variants of lens_icon_button / lens_icon_button_active:
 * identical hover/active/click behaviour, but draw the host-owned raster
 * image where the glyph would be. NULL image draws the background only
 * (blank tile); pair with lens_icon_button_active as a glyph fallback when
 * no texture is available. */
LENS_API bool lens_image_button(lens *ui, flux_image *image);
LENS_API bool lens_image_button_active(lens *ui, flux_image *image, bool active);
/* Flat icon button for navigation strips and toolbars: transparent at rest,
 * with a subtle hover fill. Returns true on the frame it is clicked. */
LENS_API bool lens_icon_button(lens *ui, lens_icon_id id);
/* As lens_icon_button, but `active` shows a steady neutral tint
 * (style-resolved bg_pressed; theme: color_active) for the selected view —
 * state as data, no flavor (ADR-0061). Corner radius, colours, and rails
 * are style atoms or caller-owned skins, not separate APIs. */
LENS_API bool lens_icon_button_active(lens *ui, lens_icon_id id, bool active);
/* Rounded icon tile with an explicit logical glyph size and optional
 * top-right text badge (for example "1" on repeat-one). The tile stays flat
 * at rest, gains a rounded hover/active surface, and never uses an accent
 * rail. Pass NULL or an empty string for no badge. */
LENS_API bool lens_icon_button_badged(lens *ui, lens_icon_id id, const char *badge,
                                      float glyph_size, bool active);
/* Checkable rounded icon button whose state is expressed by swapping glyphs,
 * not by painting a persistent selected surface. The checked glyph uses the
 * accent colour and accessibility exposes LENS_A11Y_CHECKED. Hover feedback
 * remains the same as a regular rounded icon button. */
LENS_API bool lens_icon_toggle_button(lens *ui, lens_icon_id unchecked_icon,
                                      lens_icon_id checked_icon, float glyph_size, bool checked);

/* ================================================================== */
/*  Widgets — descriptor forms                                        */
/*                                                                    */
/*  Each takes a single options struct (designated initializers) and  */
/*  returns the full lens_response, so you can read .rect/.hovered/etc.  */
/*  without a follow-up lens_get_response() call. Example:               */
/*                                                                    */
/*      lens_response r = lens_button_ex(ui, (lens_button_opts){            */
/*          .label = "Save", .box = { .disabled = !dirty } });        */
/*      if (r.clicked) save();                                        */
/* ================================================================== */

typedef struct lens_button_opts {
    lens_box box;
    const char *label;
} lens_button_opts;
typedef struct lens_selectable_opts {
    lens_box box;
    const char *label;
    bool selected;
} lens_selectable_opts;
typedef struct lens_checkbox_opts {
    lens_box box;
    const char *label;
    bool *value;
} lens_checkbox_opts;
typedef struct lens_switch_opts {
    lens_box box;
    const char *label;
    const char *description;
    bool *value;
} lens_switch_opts;
typedef struct lens_radio_opts {
    lens_box box;
    const char *label;
    int *value;
    int option_value;
} lens_radio_opts;
typedef struct lens_slider_opts {
    lens_box box;
    const char *label;
    float *value;
    float min, max;
} lens_slider_opts;
typedef struct lens_collapsing_opts {
    lens_box box;
    const char *label;
} lens_collapsing_opts;
typedef struct lens_textfield_opts {
    lens_box box;
    const char *label;
    char *buf;
    size_t buf_cap;
    const char *placeholder; /* hint shown while empty; NULL=none */
} lens_textfield_opts;
typedef struct lens_textarea_opts {
    lens_box box;
    const char *label;
    char *buf;
    size_t buf_cap;
    float min_height; /* minimum text-area height */
    const char *placeholder;
} lens_textarea_opts;
typedef struct lens_dropdown_opts {
    lens_box box;
    const char *label;
    int *selected;
    const char **items;
    int count;
} lens_dropdown_opts;

LENS_API lens_response lens_button_ex(lens *ui, lens_button_opts opts);
LENS_API lens_response lens_selectable_ex(lens *ui, lens_selectable_opts opts);
LENS_API lens_response lens_checkbox_ex(lens *ui, lens_checkbox_opts opts);
LENS_API lens_response lens_switch_ex(lens *ui, lens_switch_opts opts);
LENS_API lens_response lens_radio_ex(lens *ui, lens_radio_opts opts);
LENS_API lens_response lens_slider_ex(lens *ui, lens_slider_opts opts);
LENS_API lens_response lens_collapsing_ex(lens *ui, lens_collapsing_opts opts);
LENS_API lens_response lens_textfield_ex(lens *ui, lens_textfield_opts opts);
LENS_API lens_response lens_textarea_ex(lens *ui, lens_textarea_opts opts);
LENS_API lens_response lens_dropdown_ex(lens *ui, lens_dropdown_opts opts);

/* ================================================================== */
/*  Placement & z bands (ADR-0060)                                    */
/*                                                                    */
/*  One tree: an absolutely-placed container keeps its parent chain  */
/*  and sibling position but escapes its parent's layout flow and    */
/*  clip, and renders in a closed z band instead of in tree order.   */
/*  There is no numeric z, ever: within a band, later registration   */
/*  paints (and hit-tests) above earlier. FLOW nodes are always      */
/*  LENS_BAND_BASE.                                                   */
/*                                                                    */
/*  Transient lifetime is orthogonal to stacking: a transient place  */
/*  node is gated by the retained open-set (lens_place_open/close),  */
/*  Escape closes the top dismissable one, and a click outside its   */
/*  last-frame rect closes it (same-frame-open grace applies).       */
/* ================================================================== */

typedef enum lens_band {
    LENS_BAND_BACKDROP = 0, /* below the base tree; hit-transparent by default */
    LENS_BAND_BASE = 1,     /* the base tree itself — FLOW nodes only; an ABS
                               node requesting BASE is clamped to CHROME by
                               lens_place_begin (render/hit-order invariant) */
    LENS_BAND_CHROME = 2,   /* persistent chrome: docks, status bars         */
    LENS_BAND_POPUP = 3,    /* transient popups: dropdowns, menus, modals    */
    LENS_BAND_TOPMOST = 4,  /* above everything: drag ghosts, tooltips       */
    LENS_BAND_COUNT = 5,
} lens_band;

typedef enum lens_place_mode {
    LENS_PLACE_EXACT = 0,    /* rect is the position (+ minimum extent); clamped */
    LENS_PLACE_ANCHORED = 1, /* probe at rect (the owner anchor), drop below,
                                flip above on overflow, clamp */
    LENS_PLACE_CENTERED = 2, /* centred on the bounds (rect ignored) */
} lens_place_mode;

typedef struct lens_place_opts {
    lens_band band;         /* z band for the subtree */
    lens_place_mode mode;   /* how rect/bounds resolve to a position */
    flux_rect rect;         /* EXACT: top-left + minimum extent; ANCHORED: owner
                               anchor (counts as inside for click-outside) */
    flux_rect bounds;       /* placement + render boundary; w/h <= 0 = display */
    bool transient;         /* open-set managed: begin is gated by
                               lens_place_open; Esc/click-outside dismiss */
    bool interactive;       /* BACKDROP only: opt into hit-testing (default:
                               a backdrop is hit-transparent) */
    lens_layout_opts layout; /* the subtree's internal flexbox + surface
                               (gap/pad/cross/bg/border/radius; min_width >
                               0 fixes the node's width, as do box.width /
                               EXACT rect.w) */
} lens_place_opts;

LENS_API void lens_place_open(lens *ui, const char *id);
LENS_API void lens_place_close(lens *ui, const char *id);
LENS_API bool lens_place_is_open(const lens *ui, const char *id);
/* Whether the last-frame bounds of an open transient place node contain the
 * cursor. Complements an owner widget's hovered response for hover popups. */
LENS_API bool lens_place_hovered(const lens *ui, const char *id);

/* Open an absolutely-placed container sub-root. Non-transient nodes are
 * always entered; transient ones only while open (lens_place_open). Only
 * container sub-roots may be placed; leaf widgets cannot. Returns true when
 * the body should build. Pair with lens_place_end on true returns. */
LENS_API bool lens_place_begin(lens *ui, const char *id, lens_place_opts opts);
LENS_API void lens_place_end(lens *ui);

/* ================================================================== */
/*  Modal dialog (ADR-0039)                                           */
/*  A centered popup with a dim backdrop that occludes the base tree,  */
/*  plus a Tab focus trap so keyboard cycling stays inside the dialog  */
/*  body. Open with lens_modal_open; build the body between            */
/*  lens_modal_begin (true) and lens_modal_end; close on a button or   */
/*  via lens_modal_close. Escape and click-outside close it unless     */
/*  `pinned` is set.                                                   */
/* ================================================================== */

typedef struct lens_modal_opts {
    const char *title;   /* optional heading drawn at the top; NULL = none */
    flux_color backdrop; /* dim colour over the base tree; 0 = default 0x80000000 */
    float min_width;     /* content minimum width, logical px; 0 = 240       */
    bool pinned;         /* Escape / click-outside leave it; default false   */
} lens_modal_opts;

LENS_API void lens_modal_open(lens *ui, const char *id);
LENS_API void lens_modal_close(lens *ui, const char *id);
LENS_API bool lens_modal_is_open(const lens *ui, const char *id);

/* Returns true when the modal is open and the body should build; pair
 * with lens_modal_end. The focusable widgets built between the two
 * define the Tab cycle (the trap range). */
LENS_API bool lens_modal_begin(lens *ui, const char *id, lens_modal_opts opts);
LENS_API void lens_modal_end(lens *ui);

/* ================================================================== */
/*  Menus — menu bar, context menu, submenu (ADR-0040)                */
/*                                                                    */
/*  Built on placement (ADR-0060). A menu bar is a horizontal row of    */
/*  triggers with click-then-drag switching; a context menu opens at    */
/*  the cursor on right-click; a submenu nests to the side of its       */
/*  parent item. Items carry an optional shortcut, check/radio mark,    */
/*  and disabled state.                                                 */
/* ================================================================== */

/* Flags for lens_menu_item_flags (OR'd). */
enum {
    LENS_MENU_DISABLED = 1u << 0,
    LENS_MENU_CHECKED = 1u << 1,
    LENS_MENU_RADIO = 1u << 2,
};

LENS_API bool lens_menubar_begin(lens *ui, const char *id);
LENS_API void lens_menubar_end(lens *ui);

/* A menu trigger in a bar: opens its popup on click, switches on hover
 * while a sibling is open. Returns true when the menu body should build;
 * pair with lens_menu_end. Inside the body call lens_menu_item[_*]. */
LENS_API bool lens_menu_begin(lens *ui, const char *label);
LENS_API void lens_menu_end(lens *ui);

/* A menu item. Returns true on the frame clicked (the menu stack closes
 * automatically). `shortcut` is right-aligned, dimmed; NULL = none. */
LENS_API bool lens_menu_item(lens *ui, const char *label, const char *shortcut);
LENS_API bool lens_menu_item_disabled(lens *ui, const char *label, const char *shortcut);
LENS_API bool lens_menu_item_flags(lens *ui, const char *label, const char *shortcut,
                                   uint32_t flags);
LENS_API void lens_menu_separator(lens *ui);

/* A submenu: anchors to the parent item, opens on hover dwell. Returns
 * true when the submenu body should build; pair with lens_submenu_end. */
LENS_API bool lens_submenu_begin(lens *ui, const char *label);
LENS_API void lens_submenu_end(lens *ui);

/* Context menu: open on right-click (or programmatically), anchored at
 * the cursor. `_begin` returns true when the body should build. */
LENS_API void lens_context_menu_open(lens *ui, const char *id, flux_rect owner_rect);
LENS_API bool lens_context_menu_begin(lens *ui, const char *id);
LENS_API void lens_context_menu_end(lens *ui);

/* Close every menu opened by a menu bar (the whole stack). Called
 * automatically when an item fires; exposed for programmatic dismiss. */
LENS_API void lens_menubar_close_all_open(lens *ui);

/* ================================================================== */
/*  Resizable split panel (ADR-0041)                                  */
/*                                                                    */
/*  A two-pane container whose divider the user drags to redistribute */
/*  space. The ratio is retained per id; nest splits for 3/4-pane     */
/*  layouts. The host reads the returned `.hovered`/`.pressed` to set  */
/*  a platform resize cursor.                                         */
/* ================================================================== */

typedef enum lens_split_direction {
    LENS_SPLIT_VERTICAL = 0,   /* left | right, divider dragged horizontally */
    LENS_SPLIT_HORIZONTAL = 1, /* top  | bottom, divider dragged vertically   */
} lens_split_direction;

typedef struct lens_split_opts {
    float ratio;      /* 0..1, first pane's share; 0 = 0.5 (seed only) */
    float min_first;  /* logical-px floor for pane 1; 0 = no floor      */
    float min_second; /* logical-px floor for pane 2; 0 = no floor      */
    float thickness;  /* handle strip thickness, px; 0 = 6              */
} lens_split_opts;

LENS_API bool lens_split_begin(lens *ui, const char *id, lens_split_direction dir,
                               const lens_split_opts *opts);
LENS_API bool lens_split_pane(lens *ui); /* open a pane; fill, then call again */
LENS_API void lens_split_end(lens *ui);
/* Current ratio (for host persistence across restarts). */
LENS_API float lens_split_ratio(const lens *ui, const char *id);

/* ================================================================== */
/*  Virtualized table / data grid (ADR-0042)                          */
/*                                                                    */
/*  A scroll-area-backed grid that builds only the visible window of  */
/*  rows. The full row_count drives the scrollbar; cells come from a  */
/*  pull callback so cost is O(visible rows), not O(row_count).       */
/* ================================================================== */

typedef struct lens_table_column {
    const char *title; /* header text; NULL = untitled column */
    float width;       /* fixed px; 0 = equal flex share of the remainder */
    lens_align align;  /* LENS_START (default), CENTER, END */
} lens_table_column;

typedef const char *(*lens_table_cell_fn)(void *user, int row, int col);

/* Leading glyph for one table cell (ADR-0066): return the icon id for
 * (row, col), or LENS_ICON_INVALID for none. Called for the visible row
 * window only, beside the cell callback. Only LENS_START-aligned columns
 * draw an icon; other alignments resolve text from the column edge and
 * ignore the id. Ids may be built-ins or runtime-registered SVG ids. */
typedef lens_icon_id (*lens_table_icon_fn)(void *user, int row, int col);

/* Host-owned selection query (ADR-0066): report whether `row` is in the
 * host's selection set. Called per visible row when set. */
typedef bool (*lens_table_selected_fn)(void *user, int row);

typedef struct lens_table_opts {
    float row_height; /* px; 0 = font_size + padding */
    bool show_header; /* draw a fixed column-title row */
    bool selectable;  /* click selects a row (persisted); also gates focus */
    bool zebra;       /* alternate-row tint */
    /* Keyboard cursor (ADR-0066): while the table is focused, Up/Down
     * move the cursor one row, Home/End jump to the first/last row, and
     * Return activates the cursor row; the cursor row scrolls into view.
     * Space is left unconsumed for the host (search-as-you-type,
     * Ctrl+Space toggles). Requires `selectable` (only selectable tables
     * take focus). The cursor is a ROW INDEX, -1 = none; from -1 Down
     * lands on the
     * first row, Up on the last. Arrow keys move the table's own cursor,
     * never keyboard focus. */
    bool keyboard;
    /* In/out host-owned cursor (dropdown-style): when non-NULL the table
     * reads the cursor from here at build start and writes it back
     * whenever the effective cursor moves (keys, or the clamp when the
     * model shrank under it). Hosts re-seed the cursor on model resets
     * by owning it here. NULL = retained in per-node widget state. */
    int *cursor;
    /* Cell icon callback; NULL = text-only cells. */
    lens_table_icon_fn icon_fn;
    /* Host-owned selection: when set, the row highlight and the a11y
     * SELECTED flag come from this callback instead of the retained
     * single-select store, and clicks only report `clicked_row` (the
     * retained `selected` stays -1, `selection_changed` stays false).
     * NULL = retained single-select, today's behavior. */
    lens_table_selected_fn selected_fn;
} lens_table_opts;

typedef struct lens_table_result {
    int selected;           /* current selected row, -1 = none (always -1
                               when opts.selected_fn owns the selection) */
    bool selection_changed; /* retained selection changed this frame */
    bool clicked;           /* a row was clicked this frame */
    int cursor;             /* effective cursor row after this frame, -1 = none */
    bool cursor_changed;    /* the effective cursor moved during this frame */
    bool activated;         /* Return on the cursor row (keyboard mode)
                               or an a11y DoAction fired this frame */
    int clicked_row;        /* row clicked this frame, -1 = none */
} lens_table_result;

LENS_API lens_table_result lens_table(lens *ui, const char *id, const lens_table_column *cols,
                                      int col_count, int row_count, lens_table_cell_fn cell,
                                      void *user, lens_table_opts opts);

/* ================================================================== */
/*  Interaction queries (ADR-0029)                                    */
/* ================================================================== */

LENS_API lens_response lens_get_response(const lens *ui); /* last widget */
LENS_API lens_cursor_hint lens_get_cursor_hint(const lens *ui);
LENS_API bool lens_focused(const lens *ui, lens_id id);
LENS_API void lens_set_focus(lens *ui, lens_id id);
LENS_API lens_id lens_active(const lens *ui);

/* ================================================================== */
/*  Clipboard and IME (ADR-0036)                                      */
/* ================================================================== */

/* Caret rect of the focused text widget in UI-space, zero when none.
 * The platform layer forwards this to the IME (e.g. zwp_text_input) so
 * the candidate window can position itself. */
LENS_API flux_rect lens_caret_rect(const lens *ui);

/* Place text on the system clipboard (calls the host lens_clipboard.set_text
 * if any). No-op when no clipboard interface was supplied. */
LENS_API void lens_copy(lens *ui, const char *utf8, size_t len);

/* Ask the host for clipboard text (calls lens_clipboard.request_text). The
 * host later delivers it via lens_paste. */
LENS_API void lens_request_paste(lens *ui);

/* Host-driven delivery of clipboard text into lens. Queues the payload
 * for the focused text widget to consume on the next frame's build. */
LENS_API void lens_paste(lens *ui, const char *utf8, size_t len);

/* Drain a pending paste payload for an app-owned editing surface (one that
 * renders text outside lens widgets and therefore has no focused widget to
 * consume the queue). Copies up to `cap-1` payload bytes into `dst`,
 * NUL-terminates, and returns the payload byte length excluding the NUL.
 * Returns 0 when nothing is pending; a drained payload is consumed.
 * Text widgets keep their own internal drain — app code should only call
 * this while its own surface is the paste target. */
LENS_API uint32_t lens_take_paste(lens *ui, char *dst, uint32_t cap);

/* Report the caret rect of an app-owned editing surface, same consumer as
 * the widget-reported rect: the platform layer forwards it to the IME so
 * the candidate window follows the caret. UI-space logical pixels. */
LENS_API void lens_set_caret_rect(lens *ui, flux_rect r);

/* ================================================================== */
/*  Escape hatch — retained nodes (ADR-0031)                          */
/*                                                                    */
/*  Handles are borrows valid only within the current frame. Re-resolve*/
/*  by lens_id each frame; never cache a node pointer past lens_begin.  */
/* ================================================================== */

LENS_API lens_node *lens_root(lens *ui);
LENS_API lens_node *lens_find(lens *ui, lens_id id);
LENS_API lens_id lens_node_id(const lens_node *n);
LENS_API flux_rect lens_node_bounds(const lens_node *n); /* final_rect */
LENS_API lens_node_phase lens_node_phase_of(const lens_node *n);
LENS_API lens_node *lens_node_parent(const lens_node *n);
LENS_API lens_node *lens_node_first_child(const lens_node *n);
LENS_API lens_node *lens_node_next_sibling(const lens_node *n);
/* Borrow (allocating + zeroing on first touch) persistent per-node state. The
 * same pointer returns every frame the id survives. A node id has one fixed
 * state type: after first allocation, requesting a different byte size returns
 * NULL rather than moving the allocation and invalidating prior borrows. */
LENS_API void *lens_node_state(lens_node *n, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* LENS_H */
