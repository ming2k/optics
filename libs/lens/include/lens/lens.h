/*
 * lens/lens.h — immediate-mode façade over a retained-mode core.
 *
 * One library (liblens), one umbrella header. You write immediate-mode
 * code each frame; lens reconciles it against a retained tree that owns
 * layout, interaction, animation, and the draw list.
 *
 * Design contract (see docs/adr):
 *   - Public symbols are `lens_*`; library internals use a `lensi_*`
 *     prefix and are not exported (ADR-0008).
 *   - Every widget call computes a stable lens_id from an id stack; the
 *     id keys the retained store (ADR-0003, ADR-0004).
 *   - lens draws only through <flux/canvas.h> (ADR-0002).
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
#define LENS_VERSION_MINOR 1
#define LENS_VERSION_PATCH 0

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
/*  Input snapshot (ADR-0006)                                         */
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
                      guard, ADR-0013) */

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

    /* IME composition in progress this frame; empty when none (ADR-0013).
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
} lens_input;

/* Host clipboard interface (ADR-0013). Supplied in lens_desc; optional.
 * Paste is asynchronous (matches Wayland wl_data_device): a request is
 * answered later by the host calling lens_paste. */
typedef struct lens_clipboard {
    void (*request_text)(void *user); /* -> lens_paste */
    void (*set_text)(const char *utf8, size_t len, void *user);
    void *user;
} lens_clipboard;

/* ================================================================== */
/*  Interaction result (ADR-0006)                                     */
/* ================================================================== */

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
/*  Accessibility semantics (ADR-0012)                                */
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
    LENS_ROLE_MENU,      /* reserved: overlay layer (ADR-0014)        */
    LENS_ROLE_RADIO,
    LENS_ROLE_DIALOG, /* modal dialog window (ADR-0016)            */
} lens_role;

/* State bits for lens_semantics.flags. */
enum {
    LENS_A11Y_FOCUSED = 1u << 0,
    LENS_A11Y_DISABLED = 1u << 1,
    LENS_A11Y_CHECKED = 1u << 2,
    LENS_A11Y_EXPANDED = 1u << 3,
    LENS_A11Y_READONLY = 1u << 4,
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
 * assistive API itself (same host separation as input, ADR-0006). */
typedef void (*lens_a11y_visit_fn)(const lens_semantics *s, flux_rect bounds, lens_id id,
                                   lens_id parent, void *user);
LENS_API void lens_accessibility_walk(const lens *ui, lens_a11y_visit_fn visit, void *user);

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

    /* Semantic text sizes (ADR-0011 extension). 0 = fall back to font_size. */
    float font_size_title;
    float font_size_h1;
    float font_size_h2;
    float font_size_h3;

    /* Font weight tokens. 0 = fall back to 400/700. */
    float font_weight;
    float font_weight_bold;

    /* Optional active-indicator treatment for selectable rows and ghost icon
     * buttons (`lens_icon_button_active`). Width in logical pixels of the
     * left accent rail drawn when active; when > 0 the glyph/text also takes
     * `color_accent`. Set to 0 for the plain tint-only active state (the
     * background fill is drawn regardless). Defaults to 0. */
    float active_indicator_width;

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
/*  Text seam (ADR-0010)                                              */
/* ================================================================== */

typedef struct lens_text_metrics {
    float width;
    float height;
    float baseline; /* from top */
} lens_text_metrics;

/* The only text entry point layout (ADR-0005 pass 1) may call. Backed
 * by a monospace metrics stub until flux core ships canvas text. */
LENS_API lens_text_metrics lens_text_measure(lens *ui, lens_font *font, const char *utf8,
                                             float size_px);
LENS_API lens_text_metrics lens_text_measure_ex(lens *ui, lens_font *font, const char *utf8,
                                                float size_px, float weight);

/* ================================================================== */
/*  Context lifecycle (ADR-0009)                                      */
/* ================================================================== */

typedef struct lens_desc {
    flux_device *device;      /* retained; persistent allocator source.
                                 NULL = use libc malloc (headless/tests,
                                 render unavailable). */
    lens_theme theme;         /* initial token set; zeroed = default */
    size_t arena_bytes;       /* per-frame arena capacity; 0 = default */
    uint32_t store_capacity;  /* initial node-store slots; 0 = default */
    float scale;              /* device-pixel scale; 0 = 1.0          */
    lens_clipboard clipboard; /* optional host clipboard (ADR-0013)   */
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
/*  Frame lifecycle (ADR-0001, frame-lifecycle.md)                    */
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

/* Accessibility reduced-motion switch. When enabled, every eased value in
 * lens resolves to its target within one frame — no fades, slides, or other
 * transitions — and lens_anim_pending stays false. The host owns the policy
 * (user preference); lens executes it. Default false. */
LENS_API void lens_set_reduced_motion(lens *ui, bool reduced);
LENS_API bool lens_reduced_motion(const lens *ui);

/* ================================================================== */
/*  Identity (ADR-0003)                                               */
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
} lens_box;

/* ================================================================== */
/*  Containers / layout (ADR-0005)                                    */
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
/*  Widgets — terse forms (ADR-0008)                                  */
/*                                                                    */
/*  The common case: label doubles as the stable id, no styling. The  */
/*  bool return means: button -> clicked; checkbox/slider/radio/text  */
/*  field/textarea/dropdown -> value changed this frame; collapsing/  */
/*  tab -> currently open/active. For an explicit id, fixed size,     */
/*  flex, disabled/error state, a tooltip, or a placeholder — and for */
/*  the full lens_response — use the matching `*_ex` form below.        */
/* ================================================================== */

LENS_API bool lens_button(lens *ui, const char *label);
/* Inline text action for breadcrumbs and secondary navigation. It has no
 * surface at rest and indicates hover/focus with an accent underline without
 * changing the text's size or weight. */
LENS_API bool lens_link(lens *ui, const char *label);
/* A borderless, full-width list / nav item. Transparent at rest, with a subtle
 * hover fill and a steady `color_active` surface when `selected`. Selection
 * colour is independent from decoration: themes may separately opt into a
 * left accent rail through active_indicator_width. Returns true on the frame
 * it is clicked. Use it for sidebar lists where a stack of filled lens_buttons
 * would read as bordered pills. */
LENS_API bool lens_selectable(lens *ui, const char *label, bool selected);
LENS_API bool lens_selectable_icon(lens *ui, lens_icon_id icon, const char *label, bool selected);
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
LENS_API void lens_scroll_begin(lens *ui, const char *id);
LENS_API void lens_scroll_end(lens *ui);
/* Programmatically position a scroll area identified in the current id scope.
 * Call after its begin/end body in the same frame (or on a later frame). The
 * layout pass clamps both offsets to the resolved content bounds. Unknown ids
 * are ignored, so callers may issue the request while content is appearing. */
LENS_API void lens_scroll_to(lens *ui, const char *id, float x, float y);

typedef enum lens_tab_style {
    /* The compact Lens default: independent tabs with an active underline. */
    LENS_TAB_STYLE_STANDARD = 0,
    /* A shared rail whose active surface joins its neighbours through curved
     * shoulders. Uses the theme's active/accent tokens and adds no close
     * affordance. Intended for views that should read as connected surfaces. */
    LENS_TAB_STYLE_CONNECTED = 1,
    /* A compact strip with a theme-coloured indicator. Its independently
     * sprung edges stretch and settle when selection changes. */
    LENS_TAB_STYLE_INDICATOR = 2,
} lens_tab_style;

typedef struct lens_tabs_opts {
    lens_tab_style style;
    flux_color rail_color;      /* 0 = style default */
    flux_color active_color;    /* 0 = theme color_active */
    flux_color hover_color;     /* 0 = theme color_hover */
    flux_color indicator_color; /* 0 = theme color_accent */
    float radius;               /* <= 0 = theme corner_radius */
    float connector_size;       /* <= 0 = derived from radius */
    float indicator_thickness;  /* <= 0 = 3 logical px */
    float indicator_gap;        /* <= 0 = 2 logical px */
    float indicator_padding;    /* <= 0 = derived from theme padding */
    bool equal_width;           /* divide available width into equal hit targets */
} lens_tabs_opts;

/* Horizontal selection strip. The terse form preserves the standard Lens
 * treatment; use lens_tabs_begin_ex to opt into a presentation variant. Both
 * forms raise an undersized host height to contain the tallest tab content. */
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
/* As lens_icon_button, but `active` shows a steady tint for the selected view.
 * An accent rail and accent glyph are added only when the theme explicitly
 * sets active_indicator_width above 0. */
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
/*  Overlay layer (ADR-0014)                                          */
/*                                                                    */
/*  Floating content that escapes the parent's clip and lays out above*/
/*  the base tree: dropdown, menu, context menu, tooltip, modal.      */
/*                                                                    */
/*  Open state is retained per id; lens_overlay_begin only enters the   */
/*  body when the id is currently open. Click-outside (with a fresh-  */
/*  frame grace) and Escape close the top open overlay.               */
/* ================================================================== */

typedef struct lens_overlay_opts {
    float gap;
    float pad;
    lens_align cross;
    flux_color bg;      /* alpha 0 = no background fill */
    flux_color border;  /* alpha 0 = no border */
    float border_width; /* border stroke width when border alpha > 0 */
    float radius;
    float min_width; /* sets fixed_w on the layer when > 0 */
} lens_overlay_opts;

LENS_API void lens_overlay_open(lens *ui, const char *id);
LENS_API void lens_overlay_close(lens *ui, const char *id);
LENS_API bool lens_overlay_is_open(const lens *ui, const char *id);
/* Whether the last-frame bounds of an open overlay contain the cursor. This
 * complements an owner widget's hovered response for hover-to-open popups. */
LENS_API bool lens_overlay_hovered(const lens *ui, const char *id);

/* Open a floating layer anchored to `anchor` (usually the owner widget's
 * `lens_get_response().rect`). The anchor counts as part of the overlay for
 * click-outside dismissal, allowing an owner trigger to implement a clean
 * press/release toggle. Returns true when the overlay is currently open and
 * the body should build. Pair with lens_overlay_end on true returns. */
LENS_API bool lens_overlay_begin(lens *ui, const char *id, flux_rect anchor,
                                 lens_overlay_opts opts);
LENS_API void lens_overlay_end(lens *ui);

/* ================================================================== */
/*  Floating layers — persistent chrome (companion to ADR-0014)       */
/*                                                                    */
/*  A floating layer is the non-dismissible sibling of an overlay: a  */
/*  positional sub-root that escapes the parent's layout flow and     */
/*  renders above the base tree every frame. Use it for chrome that   */
/*  is always on screen and never auto-dismissed: a dock, a status    */
/*  bar, a notification stack, per-window title bars.                 */
/*                                                                    */
/*  Differences from the overlay layer:                               */
/*    - Always entered: no open/close state, no lens_overlay_open.    */
/*    - Placed exactly at `rect.x`/`rect.y` (no below-anchor drop,    */
/*      no flip); clamped to the display if it would overflow.        */
/*    - Not dismissible: Escape and click-outside leave it alone.     */
/*    - Eclipses base widgets underneath, just like an overlay.       */
/*                                                                    */
/*  `rect.w`/`rect.h` are a minimum: the layer grows to fit its body  */
/*  if the measured content is larger. The same `lens_overlay_opts`   */
/*  type configures background, border, padding, and cross alignment. */
/* ================================================================== */

/* Open a persistent floating layer at `rect`. Always returns true (the
 * body always builds); pair with lens_layer_end. `rect` is the desired
 * placement in UI-space logical pixels; the layer's top-left is anchored
 * at (rect.x, rect.y) and clamped onto the display. */
LENS_API bool lens_layer_begin(lens *ui, const char *id, flux_rect rect, lens_overlay_opts opts);
LENS_API void lens_layer_end(lens *ui);

/* ================================================================== */
/*  Modal dialog (ADR-0016)                                           */
/*                                                                    */
/*  A centered overlay with a dim backdrop that eclipses the base    */
/*  tree, plus a Tab focus trap so keyboard cycling stays inside the */
/*  dialog body. Open with lens_modal_open; build the body between    */
/*  lens_modal_begin (true) and lens_modal_end; close on a button or  */
/*  via lens_modal_close. Escape and click-outside close only when    */
/*  `dismissable` is true (default).                                  */
/* ================================================================== */

typedef struct lens_modal_opts {
    const char *title;   /* optional heading drawn at the top; NULL = none */
    flux_color backdrop; /* dim colour over the base tree; 0 = default 0x80000000 */
    float min_width;     /* content minimum width, logical px; 0 = 240       */
    bool dismissable;    /* Escape / click-outside close it; default true    */
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
/*  Menus — menu bar, context menu, submenu (ADR-0017)                */
/*                                                                    */
/*  Built on the overlay layer. A menu bar is a horizontal row of     */
/*  triggers with click-then-drag switching; a context menu opens at  */
/*  the cursor on right-click; a submenu nests to the side of its     */
/*  parent item. Items carry an optional shortcut, check/radio mark,  */
/*  and disabled state.                                               */
/* ================================================================== */

/* Flags for lens_menu_item_flags (OR'd). */
enum {
    LENS_MENU_DISABLED = 1u << 0,
    LENS_MENU_CHECKED = 1u << 1,
    LENS_MENU_RADIO = 1u << 2,
};

LENS_API bool lens_menubar_begin(lens *ui, const char *id);
LENS_API void lens_menubar_end(lens *ui);

/* A menu trigger in a bar: opens its overlay on click, switches on hover
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
/*  Resizable split panel (ADR-0018)                                  */
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
/*  Virtualized table / data grid (ADR-0019)                          */
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

typedef struct lens_table_opts {
    float row_height; /* px; 0 = font_size + padding */
    bool show_header; /* draw a fixed column-title row */
    bool selectable;  /* click selects a row (persisted) */
    bool zebra;       /* alternate-row tint */
} lens_table_opts;

typedef struct lens_table_result {
    int selected;           /* current selected row, -1 = none */
    bool selection_changed; /* selection changed this frame */
    bool clicked;           /* a row was clicked this frame */
} lens_table_result;

LENS_API lens_table_result lens_table(lens *ui, const char *id, const lens_table_column *cols,
                                      int col_count, int row_count, lens_table_cell_fn cell,
                                      void *user, lens_table_opts opts);

/* ================================================================== */
/*  Interaction queries (ADR-0006)                                    */
/* ================================================================== */

LENS_API lens_response lens_get_response(const lens *ui); /* last widget */
LENS_API lens_cursor_hint lens_get_cursor_hint(const lens *ui);
LENS_API bool lens_focused(const lens *ui, lens_id id);
LENS_API void lens_set_focus(lens *ui, lens_id id);
LENS_API lens_id lens_active(const lens *ui);

/* ================================================================== */
/*  Clipboard and IME (ADR-0013)                                      */
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

/* ================================================================== */
/*  Escape hatch — retained nodes (ADR-0008)                          */
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
