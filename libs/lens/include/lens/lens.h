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
 *       flux_canvas_begin_frame(...) -> lens_render(ui, canvas) -> flux_canvas_end_frame(...)
 */

#ifndef LENS_H
#define LENS_H

/* Self-contained: lens.h uses bool, uint32_t, size_t directly and must
 * not rely on flux headers happening to provide them first. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <flux/canvas.h>
#include <flux/core.h>
#include <flux/math.h>
#include <lens/export.h> /* LENS_API — single source of truth */
#include <lens/icon.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Visibility                                                        */
/* ================================================================== */
/* LENS_API is defined in <lens/export.h>, included above. Do not define */
/* it here: two spellings of one export macro drift apart.              */

#define LENS_VERSION_MAJOR 0
#define LENS_VERSION_MINOR 0
#define LENS_VERSION_PATCH 29

/* Stringify helpers used by lens_version_string(); LENS_STRINGIFY_ adds the
 * indirection level required for macro-expansion of literal tokens. */
#define LENS_STRINGIFY_(x) #x
#define LENS_STRINGIFY(x) LENS_STRINGIFY_(x)

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
 * through untouched (custom shortcuts).
 *
 * Printable ASCII (0x20-0x7E) arrives as key events carrying the ASCII
 * codepoint itself (unshifted: 'a' not 'A') — the iris backends
 * guarantee this on every platform, so character shortcuts ('z', ',',
 * space) can be matched straight off lens_key_event.key. Non-ASCII
 * printable input never arrives as a key event; it only enters
 * text_utf8 through the IME/layout commit path. */
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

#define LENS_PREEDIT_MAX 256

typedef struct lens_input {
    uint32_t size; /* sizeof(lens_input); 0 = trust
                      full struct (forward-compat
                      guard, ADR-0036) */

    flux_point cursor; /* UI-space pixels */
    bool mouse_down[LENS_MOUSE_COUNT];
    bool mouse_pressed[LENS_MOUSE_COUNT];
    bool mouse_released[LENS_MOUSE_COUNT];
    float scroll_x, scroll_y; /* wheel-step deltas */

    uint32_t mods;       /* modifier bitmask */
    char text_utf8[256]; /* committed text this frame; sized for
                            full-sentence IME conversion, not just
                            key-at-a-time input */

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

    /* Tablet / pen state this frame (size-guarded append; hosts built
     * against older headers keep working — lens reads these only when
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
    uint32_t state; /* lens_widget_state bits (ADR-0058) */
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
    LENS_ROLE_DIALOG,   /* modal dialog window (ADR-0039)            */
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
    LENS_STYLE_BG = 1u << 0,         /* resting surface           */
    LENS_STYLE_BG_HOVER = 1u << 1,   /* hovered surface           */
    LENS_STYLE_BG_PRESSED = 1u << 2, /* pressed/selected surface  */
    LENS_STYLE_FG = 1u << 3,         /* foreground (text, glyphs) */
    LENS_STYLE_BORDER = 1u << 4,     /* border stroke             */
    LENS_STYLE_ACCENT = 1u << 5,     /* accent (emphasis colour)    */
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
 * by flux_text_measure via the text seam (ADR-0033). */
LENS_API lens_text_metrics lens_text_measure(lens *ui, lens_font *font, const char *utf8,
                                             float size_px);
LENS_API lens_text_metrics lens_text_measure_ex(lens *ui, lens_font *font, const char *utf8,
                                                float size_px, float weight);

/* Release the text engine's high-water scratch and shaping cache (the
 * shared engine's `flux_text_compact`). The engine grows layout/run/
 * codepoint scratch to the largest text ever shaped and keeps it; for a
 * one-off megabyte paste that peak lingers for the session. Call this
 * from the host's idle path (iris does, on the low-power frame cadence)
 * — cheap enough to call every frame, and the next measure/draw
 * reallocates to whatever it actually needs. Null-safe. */
LENS_API void lens_text_compact(lens *ui);

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
    LENS_WIDGET_LABEL = 0,
    LENS_WIDGET_ICON = 1,
    LENS_WIDGET_IMAGE = 2,
    LENS_WIDGET_SEPARATOR = 3,
    LENS_WIDGET_BUTTON = 4,
    LENS_WIDGET_CHECKBOX = 5,
    LENS_WIDGET_SELECTABLE = 6,
    LENS_WIDGET_SLIDER = 7,
    LENS_WIDGET_TEXTEDIT = 8,
    LENS_WIDGET_KIND_COUNT,

    /* ---- Host-reserved range (ADR-0073) --------------------------- */
    LENS_WIDGET_KIND_USER_BASE = 0x40000000u,
} lens_widget_kind;

/* The boundary is ABI: a future library version must keep the reserved
 * range reserved (hosts bake these values into their own enums). */
static_assert((uint32_t)LENS_WIDGET_KIND_USER_BASE == 0x40000000u,
              "user widget-kind base is an ABI boundary (ADR-0073)");
static_assert((uint32_t)LENS_WIDGET_KIND_COUNT <= (uint32_t)LENS_WIDGET_KIND_USER_BASE,
              "built-in kinds must never enter the host-reserved range");

/* One wrapped line of text for a LENS_WIDGET_LABEL record (and the visible
 * body lines of a multiline LENS_WIDGET_TEXTEDIT record): the widget owns
 * wrapping / windowing (behaviour), the skin only draws. Strings are
 * borrowed for the frame; coordinates are node-local. */
typedef struct lens_text_line {
    const char *text;
    float x, y;
} lens_text_line;

typedef enum lens_button_variant {
    LENS_BUTTON_DEFAULT = 0,
    LENS_BUTTON_PRIMARY = 1,
    LENS_BUTTON_SUBTLE = 2,
    LENS_BUTTON_LINK = 3,
} lens_button_variant;

typedef enum lens_checkbox_appearance {
    LENS_CHECKBOX_BOX = 0,    /* square checkbox */
    LENS_CHECKBOX_SWITCH = 1, /* toggle switch */
    LENS_CHECKBOX_RADIO = 2,  /* radio circle */
} lens_checkbox_appearance;

/* Per-kind content payload for lens_widget_record. */
typedef struct lens_widget_content {
    const char *label;
    lens_text_metrics text;
    float text_size;
    float text_weight;
    bool compact;
    const lens_text_line *lines;
    int line_count;
    lens_icon_id icon;
    float glyph_size;
    flux_image *image;
    flux_color tint;
    float ratio;
    bool vertical;
    bool error;
    lens_checkbox_appearance appearance;
    lens_button_variant variant;

    /* TEXTEDIT: text-edit payload */
    bool multiline;
    const char *edit_text;
    float edit_text_y;
    bool show_placeholder;
    const flux_rect *sel_rects;
    int sel_rect_count;
    flux_rect caret;
    bool show_caret;
    flux_rect preedit_underline;
    bool has_preedit;
    flux_rect preedit_clause;
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

/* Skin + caller context. The plain lens_skin_fn signature has no closure
 * slot, which forced any stateful skin (a spring indicator carrying its
 * own physics) to reach the state through a process global — the exact
 * hazard ADR-0059 flagged as future work. This form passes `user` back on
 * every emission; register it with lens_set_skin_userdata. */
typedef void (*lens_skin_userdata_fn)(lens *ui, lens_node *node, const lens_widget_record *rec,
                                      void *user);

/* Replace the skin for a widget kind context-wide; NULL restores the
 * built-in default. The context is the single override granularity
 * (ADR-0061 retired the per-call *_skinned forms): for a one-off override,
 * set the skin, build the widget, restore NULL. */
LENS_API void lens_set_skin(lens *ui, lens_widget_kind kind, lens_skin_fn fn);
/* Same, with a closure pointer delivered to the skin on every emission.
 * `user` is stored verbatim and passed back unmodified; the library never
 * dereferences it. NULL fn restores the default (user is then ignored).
 * Registered userdata skins also receive first-touch zeroes from
 * lens_skin_scratch like any other skin. */
LENS_API void lens_set_skin_userdata(lens *ui, lens_widget_kind kind, lens_skin_userdata_fn fn,
                                     void *user);
/* The built-in default skin for a kind — for wrapping: call it from a
 * custom skin to keep the stock chrome, then add your own. */
LENS_API lens_skin_fn lens_default_skin(lens_widget_kind kind);

/* Per-node scratch storage for skins (ADR-0061 item 9): LENS_SKIN_SCRATCH_FLOATS
 * retained floats, zeroed on the node's first touch, living and dying with the
 * node (ADR-0038's GC reclaims the node and the scratch goes with it).
 * This is the inline fast path for the common case (one 1-D spring:
 * position/velocity/target/stamp). Need more — an X/Y pair of springs, a
 * scale+opacity+rotation bundle? Graduate to lens_node_state(node, bytes):
 * arbitrary size, same zero-on-first-touch, same GC lifetime, no external
 * hashtable required. This is mechanism, not animation: the library stores
 * state; it never integrates anything. See libs/anim (ADR-0077) for the
 * math to run on it. */
#define LENS_SKIN_SCRATCH_FLOATS 4
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

/* Emit a skin record for a HOST-RESERVED widget kind (ADR-0073).
 *
 * This is the emission half of the user-widget contract: a composite
 * built from lens_pressable_begin + children calls this with its own
 * user-range kind and the skin registered for that kind fires — identity
 * and re-skinning parity with built-ins, with no library-side
 * measure/interact behavior implied. `kind` outside the reserved range is
 * rejected (built-in kinds emit through their own widget entry points,
 * with their own records; going around them here would bypass their
 * semantics). Content fields default as in lens_widget_record; set what
 * the skin needs via `content`. No-op (not an error) when the node is
 * NULL or no skin is registered for the kind. */
LENS_API void lens_skin_emit_user(lens *ui, lens_node *node, lens_widget_kind kind,
                                  lens_widget_record rec);

/* ================================================================== */
/*  Context lifecycle (ADR-0032)                                      */
/* ================================================================== */

typedef struct lens_desc {
    /* Size guard, same pattern as lens_input/lens_theme (ADR-0032): set
     * to sizeof(lens_desc). 0 = legacy caller (pre-guard layout trusted
     * whole). The library copies only min(caller, library) bytes, so a
     * caller built against an older or newer header degrades cleanly
     * instead of over-reading. Future fields append after `clipboard`. */
    uint32_t size;

    flux_device *device;      /* retained; persistent allocator source.
                                 NULL = use libc malloc (headless/tests,
                                 render unavailable). */
    lens_theme theme;         /* initial token set; zeroed = default */
    size_t arena_bytes;       /* per-frame arena capacity; 0 = default */
    uint32_t store_capacity;  /* initial node-store slots; 0 = default */
    float scale;              /* device-pixel scale; 0 = 1.0          */
    float text_scale;         /* accessibility text scale; 0/1 = 1.0  */
    lens_clipboard clipboard; /* optional host clipboard (ADR-0036)   */
} lens_desc;

FLUX_NODISCARD LENS_API flux_result lens_create(const lens_desc *desc, lens **out);

/* Zero-init + append-safe descriptor preset (matches the FLUX_*_DESC_INIT
 * convention). New code should prefer this over a bare {0}: it pins the
 * size guard so a future field append can't silently leave it stale. */
#define LENS_DESC_INIT /* clang-format off */ \
    { .size = sizeof(lens_desc) } /* clang-format on */

LENS_API void lens_destroy(lens *ui);
LENS_API void lens_set_theme(lens *ui, lens_theme theme);
LENS_API lens_theme lens_get_theme(const lens *ui);

/* Frame-scoped opacity switch (0..1; default 1.0): the single fade knob
 * for enter/exit motion. Every node built while an opacity is in effect
 * carries it as a build-time stamp, and emission bakes it into each draw
 * command's colour alpha — rects, borders, text, icons, host images and
 * scrollbars fade together, with no per-colour work by the caller. Like
 * the style scope stack, the switch resets to 1.0 at every lens_begin, so
 * a forgotten restore cannot dim the next frame; within a frame, set it
 * back after building the faded subtree. Mechanism, not animation: the
 * host owns the clock. */
LENS_API void lens_set_opacity(lens *ui, float opacity);
LENS_API float lens_opacity(const lens *ui);
LENS_API float lens_dt(const lens *ui); /* frame delta, seconds */

/* Ghost replay — the leave-animation render surface (ADR-0078).
 *
 * Call this every frame while an exit animation runs for a subtree the
 * build no longer produces: it re-pins the subtree's snapshot (captured
 * at its last live lens_end) and paints it at `alpha` through the same
 * command emitter and alpha bake as a live subtree (ADR-0068 semantics).
 * No call → the snapshot counts down and expires after
 * LENSI_GHOST_MAX_FRAMES frames. Unknown or never-seen ids are ignored
 * (advisory, never a host crash). Ghosts never hit-test, focus, or enter
 * the accessibility tree; they render after live content in their band.
 * Mechanism, not animation: the host owns the clock and the easing. */
LENS_API void lens_set_ghost(lens *ui, lens_id subtree_root, float alpha);

/* Device-pixel scale (HiDPI). The application reports the compositor /
 * window-system scale here; layout, input, and `lens_input.display_size`
 * stay in *logical* pixels, and `lens_render` scales the canvas
 * transform by this factor so 1 logical pixel maps to `scale` device
 * pixels. Default 1.0. */
LENS_API void lens_set_scale(lens *ui, float scale);
LENS_API float lens_scale(const lens *ui);

/* Accessibility text scale — the OS "make text bigger" preference. A pure
 * multiplier on every font-size token (body, headings, explicit label
 * point sizes) applied at the resolved-style funnel, so glyphs, caret
 * metrics, and every widget height derived from a font token grow
 * together: text scales, boxes follow, nothing clips. Orthogonal to
 * lens_set_scale (device pixels): a 1.25 text scale at 2x DPI renders
 * 1.25x taller glyphs at the same 2x raster density. Pure-px geometry
 * (padding, stroke widths) deliberately stays put. Lens executes the
 * factor; the host owns the policy (iris reads the system preference on
 * all backends). Non-finite and <= 0 are ignored. Default 1.0. Not
 * frame-scoped — persists like scale/reduced_motion. */
LENS_API void lens_set_text_scale(lens *ui, float factor);
LENS_API float lens_text_scale(const lens *ui);

/* ================================================================== */
/*  Frame lifecycle (ADR-0024, frame-lifecycle.md)                    */
/* ================================================================== */

LENS_API void lens_begin(lens *ui, const lens_input *input);
LENS_API void lens_end(lens *ui);
FLUX_NODISCARD LENS_API flux_result lens_render(lens *ui, flux_canvas *canvas);

/* True if the per-frame arena overflowed during the frame just built. */
LENS_API bool lens_overflowed(const lens *ui);

/* True if the same widget id was linked more than once under one parent in
 * the frame just built. The later occurrence is omitted because retained ids
 * name nodes one-to-one; callers building repeated labels must scope them
 * with lens_push_id[_int] or pass an explicit lens_box.id. */
LENS_API bool lens_has_duplicate_ids(const lens *ui);

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
 * widget descriptor. Each field applies to *this* call only; there are
 * no floating "applies to the next/last widget" modifiers, so there is
 * no ordering hazard. Zeroed fields take the widget default. */
typedef struct lens_box {
    const char *id;      /* explicit identity; NULL = derive from label/content */
    float flex;          /* main-axis grow factor; 0 = don't grow       */
    float width;         /* fixed width, logical px;  0 = intrinsic      */
    float height;        /* fixed height, logical px; 0 = intrinsic      */
    float min_width;     /* minimum width constraint; 0 = unconstrained  */
    float min_height;    /* minimum height constraint; 0 = unconstrained */
    float max_width;     /* maximum width constraint; 0 = unconstrained  */
    float max_height;    /* maximum height constraint; 0 = unconstrained */
    bool disabled;       /* non-interactive + dimmed                     */
    bool error;          /* validation-error styling                     */
    const char *tooltip; /* shown while this widget is hovered           */
    lens_style style;    /* per-call style overrides (ADR-0061)          */
} lens_box;

/* ================================================================== */
/*  Containers / layout (ADR-0028)                                    */
/* ================================================================== */

typedef struct lens_layout_opts {
    lens_box box;
    float min_width;    /* minimum resolved width; 0 = no lower bound   */
    float max_width;    /* maximum resolved width; 0 = no upper bound   */
    float min_height;   /* minimum resolved height; 0 = no lower bound  */
    float max_height;   /* maximum resolved height; 0 = no upper bound  */
    float gap;          /* between children, main axis                  */
    float pad;          /* inside the container, all sides              */
    lens_align align;   /* main-axis distribution                       */
    lens_align cross;   /* cross-axis alignment; LENS_STRETCH fills    */
    flux_color bg;      /* background fill; alpha 0 = transparent       */
    float radius;       /* corner radius                                */
    flux_color border;  /* border stroke                                */
    float border_width; /* border width                                 */
} lens_layout_opts;

LENS_API void lens_row_begin(lens *ui, const lens_layout_opts *opts);
LENS_API void lens_row_end(lens *ui);
LENS_API void lens_column_begin(lens *ui, const lens_layout_opts *opts);
LENS_API void lens_column_end(lens *ui);
LENS_API void lens_close(lens *ui); /* closes the innermost open container */

typedef struct lens_grid_opts {
    lens_box box;
    int columns;
    float col_gap;
    float row_gap;
    float pad;
} lens_grid_opts;

LENS_API void lens_grid_begin(lens *ui, const lens_grid_opts *opts);
LENS_API void lens_grid_end(lens *ui);

typedef struct lens_scroll_opts {
    lens_box box;
    float max_height;
    float max_width;
    bool autohide;
} lens_scroll_opts;

LENS_API bool lens_scroll_begin(lens *ui, const lens_scroll_opts *opts);
LENS_API void lens_scroll_end(lens *ui);
LENS_API bool lens_scroll_offset(const lens *ui, const char *id, float *x, float *y);
LENS_API void lens_scroll_to(lens *ui, const char *id, float x, float y);

typedef struct lens_pressable_opts {
    lens_box box;
    const char *label;
    int mouse_button;
    lens_layout_opts layout;
} lens_pressable_opts;

LENS_API lens_response lens_pressable_begin(lens *ui, const lens_pressable_opts *opts);
LENS_API void lens_pressable_end(lens *ui);

/* Layout modifiers */
LENS_API void lens_flex(lens *ui, float grow);
LENS_API void lens_spacer(lens *ui, float size);
LENS_API void lens_space_between(lens *ui);
LENS_API void lens_fit(lens *ui);
LENS_API void lens_size(lens *ui, float width, float height);

/* ================================================================== */
/*  Absolute placement & overlays (ADR-0060)                          */
/* ================================================================== */

typedef enum lens_band : uint32_t {
    LENS_BAND_BASE = 0,
    LENS_BAND_BACKDROP = 1,
    LENS_BAND_CHROME = 2,
    LENS_BAND_POPUP = 3,
    LENS_BAND_MODAL = 4,
    LENS_BAND_TOOLTIP = 5,
    LENS_BAND_NOTIFY = 6,
    LENS_BAND_COUNT = 7,
} lens_band;

typedef enum lens_place_mode {
    LENS_PLACE_EXACT = 0,
    LENS_PLACE_ANCHORED = 1,
    LENS_PLACE_CENTERED = 2,
    LENS_PLACE_TOOLTIP = 3,
    LENS_PLACE_BACKDROP = 4,
} lens_place_mode;

typedef struct lens_place_opts {
    lens_box box;
    lens_band band;
    lens_place_mode mode;
    flux_rect rect;
    flux_rect bounds;
    bool transient;
    bool interactive;
    lens_layout_opts layout;
} lens_place_opts;

LENS_API bool lens_place_begin(lens *ui, const lens_place_opts *opts);
LENS_API void lens_place_end(lens *ui);
LENS_API void lens_place_open(lens *ui, const char *id);
LENS_API void lens_place_close(lens *ui, const char *id);
LENS_API void lens_place_toggle(lens *ui, const char *id);
LENS_API bool lens_place_is_open(const lens *ui, const char *id);
LENS_API void lens_place_close_all(lens *ui);
LENS_API bool lens_place_hovered(const lens *ui, const char *id);

/* ================================================================== */
/*  Minimal Orthogonal Widgets (Single Descriptor API)                */
/* ================================================================== */

/* 1. Label — text rendering */
typedef struct lens_label_opts {
    lens_box box;
    const char *text;
    float size;       /* <= 0 = theme font size */
    float weight;     /* <= 0 = theme regular weight */
    bool wrap;        /* wrap text on max_width or container bounds */
    lens_align align; /* LENS_START, LENS_CENTER, LENS_END */
} lens_label_opts;

LENS_API lens_response lens_label(lens *ui, const lens_label_opts *opts);

/* 2. Icon — vector glyph */
typedef struct lens_icon_opts {
    lens_box box;
    lens_icon_id id;  /* glyph id */
    float size;       /* <= 0 = theme font size */
    flux_color color; /* 0 = theme foreground */
} lens_icon_opts;

LENS_API lens_response lens_icon(lens *ui, const lens_icon_opts *opts);

/* 3. Image — raster texture */
typedef struct lens_image_opts {
    lens_box box;
    flux_image *image;
    float width;
    float height;
    flux_color tint;
} lens_image_opts;

LENS_API lens_response lens_image(lens *ui, const lens_image_opts *opts);

/* 4. Separator — horizontal or vertical dividing line */
typedef struct lens_separator_opts {
    lens_box box;
    lens_axis axis;  /* LENS_ROW (horizontal line) or LENS_COLUMN (vertical line) */
    float thickness; /* <= 0 = 1.0f */
} lens_separator_opts;

LENS_API lens_response lens_separator(lens *ui, const lens_separator_opts *opts);

/* 5. Button — clickable trigger */
typedef struct lens_button_opts {
    lens_box box;
    const char *label;
    lens_icon_id icon;
    flux_image *image;
    lens_button_variant variant;
    bool active;
    int mouse_button;
} lens_button_opts;

LENS_API lens_response lens_button(lens *ui, const lens_button_opts *opts);

/* 6. Checkbox — boolean toggle */
typedef struct lens_checkbox_opts {
    lens_box box;
    const char *label;
    bool *value;
    lens_checkbox_appearance appearance;
} lens_checkbox_opts;

LENS_API lens_response lens_checkbox(lens *ui, const lens_checkbox_opts *opts);

/* 7. Selectable — selectable row item */
typedef struct lens_selectable_opts {
    lens_box box;
    const char *label;
    bool selected;
    lens_icon_id icon;
} lens_selectable_opts;

LENS_API lens_response lens_selectable(lens *ui, const lens_selectable_opts *opts);

/* 8. Slider — scalar value range dragging */
typedef struct lens_slider_opts {
    lens_box box;
    const char *label;
    float *value;
    float min;
    float max;
    float step;
    lens_axis axis;
    const char *format;
} lens_slider_opts;

LENS_API lens_response lens_slider(lens *ui, const lens_slider_opts *opts);

/* 8. Textedit — single-line or multi-line text editor */
typedef struct lens_textedit_opts {
    lens_box box;
    char *buf;
    size_t cap;
    const char *placeholder;
    bool multiline;
    uint32_t rows;
    bool password;
    bool select_all_on_focus;
    bool readonly;
} lens_textedit_opts;

LENS_API lens_response lens_textedit(lens *ui, const lens_textedit_opts *opts);
LENS_API void lens_textedit_set_caret(lens *ui, const char *label, uint32_t caret);
LENS_API void lens_textedit_set_selection(lens *ui, const char *label, uint32_t sel_start,
                                          uint32_t sel_end);

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

/* Surrounding-text and content-hint context of the focused text widget,
 * for the host's IME integration (text-input-v3 set_surrounding_text /
 * set_content_type). Refreshed every frame by the focused text widget
 * alongside the caret rect; `utf8 == NULL` when no text widget is focused.
 * The buffer is BORROWED from the widget — valid until the next
 * lens_begin. `cursor` is the caret byte offset in `utf8`. */
typedef struct lens_text_context {
    const char *utf8;
    uint32_t len;
    uint32_t cursor;
    bool multiline; /* textarea vs single-line textfield */
} lens_text_context;

LENS_API lens_text_context lens_text_context_get(const lens *ui);

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
