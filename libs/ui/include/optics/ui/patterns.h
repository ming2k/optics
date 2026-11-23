/*
 * ui/patterns.h — high-level interaction patterns and compound protocols.
 *
 * Patterns compose primitive widgets into cohesive interaction models:
 *   - Segmented control: contiguous exclusive choice with unified frame.
 *   - Virtual layout math: precise range and padding computation for lists/grids.
 *   - Tab strip: managed tabs with close buttons, elastic item widths, and actions.
 *   - Split pane: interactive divider with clamp and dragging.
 */

#ifndef OPTICS_UI_PATTERNS_H
#define OPTICS_UI_PATTERNS_H

#include <optics/ui.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Segmented Control (ADR-Pattern-001)                               */
/* ================================================================== */

typedef struct optics_ui_segmented_item {
    const char *label;      /* text or label string (NULL if icon-only) */
    optics_ui_icon_id icon; /* OPTICS_UI_ICON_INVALID if text-only */
    bool disabled;
} optics_ui_segmented_item;

typedef struct optics_ui_segmented_opts {
    const char *id;                        /* identity seed; nullptr = "segmented" */
    const optics_ui_segmented_item *items; /* required */
    uint32_t item_count;
    uint32_t *selected_index; /* required, in-out: the active item */
    float height;             /* 0.0 = resolved theme control height (e.g. 32-38px) */
    float min_item_width;     /* 0.0 = auto intrinsic size per item */
    bool compact;             /* true = compact density */
} optics_ui_segmented_opts;

/* Render a segmented control. Returns true if the active index changed this frame. */
OPTICS_UI_API bool optics_ui_segmented_control(optics_ui *ui, const optics_ui_segmented_opts *opts);

/* ================================================================== */
/*  Virtual Layout Computation (ADR-Pattern-002)                      */
/* ================================================================== */

typedef struct optics_ui_virtual_grid_plan {
    uint32_t columns;           /* Resolved number of columns >= 1 */
    float column_width;         /* Actual allocated width per column item */
    float column_gap;           /* Actual horizontal gap between columns */
    float row_pitch;            /* Total vertical step per row (item_height + row_gap) */
    uint32_t total_rows;        /* Total row count */
    uint32_t visible_row_start; /* First visible row index (including overscan) */
    uint32_t visible_row_end;   /* Non-inclusive end visible row index */
    float top_padding;          /* Spacer height before visible range */
    float bottom_padding;       /* Spacer height after visible range */
} optics_ui_virtual_grid_plan;

typedef struct optics_ui_virtual_grid_calc_opts {
    float available_width;
    float viewport_height;
    float scroll_y;
    uint32_t total_items;
    float min_col_width;
    float max_col_width;
    float item_height;
    float target_gap;
    uint32_t overscan_rows;
} optics_ui_virtual_grid_calc_opts;

/* Compute exact virtual grid metrics for a given available width & scroll offset. */
OPTICS_UI_API optics_ui_virtual_grid_plan
optics_ui_virtual_grid_calc(const optics_ui_virtual_grid_calc_opts *opts);

/* ================================================================== */
/*  Tab Strip Pattern (ADR-Pattern-003)                               */
/* ================================================================== */

typedef struct optics_ui_tab_item {
    const char *title;
    optics_ui_icon_id icon;
    bool closable;
} optics_ui_tab_item;

typedef enum optics_ui_tab_action_kind : uint32_t {
    OPTICS_UI_TAB_ACTION_NONE = 0,
    OPTICS_UI_TAB_ACTION_SELECT,
    OPTICS_UI_TAB_ACTION_CLOSE,
    OPTICS_UI_TAB_ACTION_NEW,
    OPTICS_UI_TAB_ACTION_MOVE,
} optics_ui_tab_action_kind;

typedef struct optics_ui_tab_action {
    optics_ui_tab_action_kind kind;
    uint32_t index;      /* relevant for SELECT, CLOSE and MOVE (source) */
    uint32_t to;         /* relevant for MOVE: insertion slot AFTER removal of
                            `index` (post-removal coordinates), < previous count */
    bool pressed_on_tab; /* a pointer press landed on an interactive strip
                            element (tab, close or new button) this frame;
                            CSD hosts use it to suppress window dragging so
                            tab drags are not swallowed by the move grab */
} optics_ui_tab_action;

typedef struct optics_ui_tab_strip_opts {
    const char *id;                 /* identity seed; nullptr = "tab_strip" */
    const optics_ui_tab_item *tabs; /* required */
    uint32_t tab_count;
    uint32_t active_index;
    float height;                 /* 0.0 = default ~36px */
    float min_tab_width;          /* e.g. 100.0 */
    float max_tab_width;          /* e.g. 220.0 */
    bool show_new_button;         /* show '+' trailing button */
    optics_ui_icon_id close_icon; /* optional close icon asset */
    optics_ui_icon_id new_icon;   /* optional new-tab icon asset */
} optics_ui_tab_strip_opts;

OPTICS_UI_API optics_ui_tab_action optics_ui_tab_strip(optics_ui *ui,
                                                       const optics_ui_tab_strip_opts *opts);

/* ================================================================== */
/*  Split Pane Divider Pattern (ADR-Pattern-004)                      */
/* ================================================================== */

typedef struct optics_ui_split_opts {
    float min_size;     /* Minimum pane size (e.g. 160.0) */
    float max_size;     /* Maximum pane size (e.g. 400.0) */
    float handle_width; /* Visual thickness, e.g. 1.0 or 2.0 */
    float hit_expand;   /* Invisible hit-test margin on both sides (e.g. 4.0) */
} optics_ui_split_opts;

/* Render an interactive vertical split divider between two panes.
 * `split_offset` is in-out position (e.g. sidebar width in pixels).
 * Returns true if the position changed due to dragging. */
OPTICS_UI_API bool optics_ui_split_handle_v(optics_ui *ui, const char *id_str, float *split_offset,
                                            const optics_ui_split_opts *opts);

/* ================================================================== */
/*  DataGrid Pattern (ADR-Pattern-005)                                */
/* ================================================================== */

typedef enum optics_ui_sort_dir : uint32_t {
    OPTICS_UI_SORT_NONE = 0,
    OPTICS_UI_SORT_ASC = 1,
    OPTICS_UI_SORT_DESC = 2,
} optics_ui_sort_dir;

typedef struct optics_ui_datagrid_col {
    const char *title;
    float width;             /* Column width in pixels (0.0 = default 120.0f) */
    float min_width;         /* Minimum constraint (0.0 = 40.0f) */
    optics_ui_sort_dir sort; /* Current sort state */
    bool sortable;           /* Header click triggers sort */
    bool resizable;          /* Divider handle is draggable */
    optics_ui_align align;   /* Cell text alignment */
} optics_ui_datagrid_col;

typedef enum optics_ui_datagrid_action_kind : uint32_t {
    OPTICS_UI_DATAGRID_ACTION_NONE = 0,
    OPTICS_UI_DATAGRID_ACTION_ROW_CLICKED,
    OPTICS_UI_DATAGRID_ACTION_ROW_DOUBLE_CLICKED,
    OPTICS_UI_DATAGRID_ACTION_SORT_CHANGED,
    OPTICS_UI_DATAGRID_ACTION_COL_RESIZED,
} optics_ui_datagrid_action_kind;

typedef struct optics_ui_datagrid_action {
    optics_ui_datagrid_action_kind kind;
    uint32_t row_index;
    uint32_t col_index;
    optics_ui_sort_dir sort_dir;
} optics_ui_datagrid_action;

typedef struct optics_ui_datagrid_opts {
    const char *id;               /* identity seed; nullptr = "datagrid" */
    optics_ui_datagrid_col *cols; /* required, in-out: drag-resizing writes `.width` */
    uint32_t col_count;
    const char *const *cell_texts; /* row-major: cell_texts[row * col_count + col] */
    uint32_t row_count;
    uint32_t *selected_row; /* optional, in-out: the active row */
    float height;           /* Total container height (0.0 = auto/fill) */
    float header_height;    /* Header row height (0.0 = default 32.0f) */
    float row_height;       /* Body row height (0.0 = default 28.0f) */
    bool striped;           /* Alternating zebra background */
} optics_ui_datagrid_opts;

/* Render an interactive data grid with virtualized rows, column resizing,
 * and header sorting. */
OPTICS_UI_API optics_ui_datagrid_action optics_ui_datagrid(optics_ui *ui,
                                                           const optics_ui_datagrid_opts *opts);

/* ================================================================== */
/*  Tree View Pattern (ADR-Pattern-006)                               */
/* ================================================================== */

typedef struct optics_ui_tree_item {
    const char *label;
    optics_ui_icon_id icon; /* 0 or OPTICS_UI_ICON_INVALID = none */
    uint32_t depth;         /* Indentation level (0 = root) */
    bool is_leaf;           /* True if leaf (no expansion chevron) */
    bool expanded;          /* In-out: subtree expanded */
    bool selected;          /* True if row is selected */
    const char *badge;      /* Optional trailing metadata string (NULL if none) */
} optics_ui_tree_item;

typedef enum optics_ui_tree_action_kind : uint32_t {
    OPTICS_UI_TREE_ACTION_NONE = 0,
    OPTICS_UI_TREE_ACTION_SELECT,
    OPTICS_UI_TREE_ACTION_TOGGLE,
    OPTICS_UI_TREE_ACTION_ACTIVATE,
} optics_ui_tree_action_kind;

typedef struct optics_ui_tree_action {
    optics_ui_tree_action_kind kind;
    uint32_t index; /* Item index in caller's array */
} optics_ui_tree_action;

typedef struct optics_ui_tree_opts {
    const char *id;             /* identity seed; nullptr = "tree_view" */
    optics_ui_tree_item *items; /* required, in-out: toggling writes expansion state */
    uint32_t item_count;
    float height;           /* Container height (0.0 = auto/fill) */
    float row_height;       /* Row height (0.0 = default 26.0f) */
    float indent_per_level; /* Indent in pixels per depth (0.0 = default 16.0f) */
} optics_ui_tree_opts;

OPTICS_UI_API optics_ui_tree_action optics_ui_tree_view(optics_ui *ui,
                                                        const optics_ui_tree_opts *opts);

/* ================================================================== */
/*  Cascading Menu & MenuBar Pattern (ADR-Pattern-007)                */
/* ================================================================== */

typedef struct optics_ui_menu_item {
    const char *label;
    optics_ui_icon_id icon;
    const char *shortcut; /* Trailing shortcut label, e.g. "Ctrl+S" (NULL if none) */
    bool disabled;
    bool is_separator;
} optics_ui_menu_item;

typedef struct optics_ui_menu_opts {
    const char *id;                   /* required: the popup's identity */
    optics_gfx_rect anchor;           /* rectangle the menu attaches to */
    const optics_ui_menu_item *items; /* required */
    uint32_t item_count;
    float width;       /* Menu width (0.0 = default 180.0f) */
    float item_height; /* Menu item height (0.0 = default 28.0f) */
} optics_ui_menu_opts;

/* Render an anchored popup menu in OPTICS_UI_BAND_POPUP with auto-flip and transient dismissal.
 * Returns activated item index, or -1 if no item was activated this frame. */
OPTICS_UI_API int32_t optics_ui_popup_menu(optics_ui *ui, const optics_ui_menu_opts *opts);

typedef struct optics_ui_menubar_item {
    const char *title;
    const char *menu_id;
} optics_ui_menubar_item;

typedef struct optics_ui_menubar_opts {
    const char *id;
    const optics_ui_menubar_item *items;
    uint32_t item_count;
    optics_gfx_rect *out_anchor;
} optics_ui_menubar_opts;

/* Render a horizontal menu bar.
 * When an item is clicked, its corresponding menu_id is opened/toggled in OPTICS_UI_BAND_POPUP.
 * Returns index of currently open menu (or -1 if all closed).
 * `opts->out_anchor` receives the bounding rectangle of the active menubar item for popup
 * placement. */
OPTICS_UI_API int32_t optics_ui_menubar(optics_ui *ui, const optics_ui_menubar_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_PATTERNS_H */
