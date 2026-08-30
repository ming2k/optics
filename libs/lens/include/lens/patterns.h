/*
 * lens/patterns.h — high-level interaction patterns and compound protocols.
 *
 * Patterns compose primitive widgets into cohesive interaction models:
 *   - Segmented control: contiguous exclusive choice with unified frame.
 *   - Virtual layout math: precise range and padding computation for lists/grids.
 *   - Tab strip: managed tabs with close buttons, elastic item widths, and actions.
 *   - Split pane: interactive divider with clamp and dragging.
 */

#ifndef LENS_PATTERNS_H
#define LENS_PATTERNS_H

#include "lens.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/*  Segmented Control (ADR-Pattern-001)                               */
/* ================================================================== */

typedef struct lens_segmented_item {
    const char *label;     /* text or label string (NULL if icon-only) */
    lens_icon_id icon;     /* LENS_ICON_INVALID if text-only */
    bool disabled;
} lens_segmented_item;

typedef struct lens_segmented_opts {
    float height;          /* 0.0 = resolved theme control height (e.g. 32-38px) */
    float min_item_width;  /* 0.0 = auto intrinsic size per item */
    bool compact;          /* true = compact density */
} lens_segmented_opts;

/* Render a segmented control. Returns true if the active index changed this frame.
 * `selected_index` points to in-out active index. */
LENS_API bool lens_segmented_control(lens *ui,
                                     const char *id_str,
                                     const lens_segmented_item *items,
                                     uint32_t item_count,
                                     uint32_t *selected_index,
                                     const lens_segmented_opts *opts);

/* ================================================================== */
/*  Virtual Layout Computation (ADR-Pattern-002)                      */
/* ================================================================== */

typedef struct lens_virtual_grid_plan {
    uint32_t columns;           /* Resolved number of columns >= 1 */
    float column_width;         /* Actual allocated width per column item */
    float column_gap;           /* Actual horizontal gap between columns */
    float row_pitch;            /* Total vertical step per row (item_height + row_gap) */
    uint32_t total_rows;        /* Total row count */
    uint32_t visible_row_start; /* First visible row index (including overscan) */
    uint32_t visible_row_end;   /* Non-inclusive end visible row index */
    float top_padding;          /* Spacer height before visible range */
    float bottom_padding;       /* Spacer height after visible range */
} lens_virtual_grid_plan;

/* Compute exact virtual grid metrics for a given available width & scroll offset. */
LENS_API lens_virtual_grid_plan lens_virtual_grid_calc(float available_width,
                                                      float viewport_height,
                                                      float scroll_y,
                                                      uint32_t total_items,
                                                      float min_col_width,
                                                      float max_col_width,
                                                      float item_height,
                                                      float target_gap,
                                                      uint32_t overscan_rows);

/* ================================================================== */
/*  Tab Strip Pattern (ADR-Pattern-003)                               */
/* ================================================================== */

typedef struct lens_tab_item {
    const char *title;
    lens_icon_id icon;
    bool closable;
} lens_tab_item;

typedef enum lens_tab_action_kind {
    LENS_TAB_ACTION_NONE = 0,
    LENS_TAB_ACTION_SELECT,
    LENS_TAB_ACTION_CLOSE,
    LENS_TAB_ACTION_NEW,
} lens_tab_action_kind;

typedef struct lens_tab_action {
    lens_tab_action_kind kind;
    uint32_t index; /* relevant for SELECT and CLOSE */
} lens_tab_action;

typedef struct lens_tab_strip_opts {
    float height;          /* 0.0 = default ~36px */
    float min_tab_width;   /* e.g. 100.0 */
    float max_tab_width;   /* e.g. 220.0 */
    bool show_new_button;  /* show '+' trailing button */
    lens_icon_id close_icon; /* optional close icon asset */
    lens_icon_id new_icon;   /* optional new-tab icon asset */
} lens_tab_strip_opts;

LENS_API lens_tab_action lens_tab_strip(lens *ui,
                                        const char *id_str,
                                        const lens_tab_item *tabs,
                                        uint32_t tab_count,
                                        uint32_t active_index,
                                        const lens_tab_strip_opts *opts);

/* ================================================================== */
/*  Split Pane Divider Pattern (ADR-Pattern-004)                      */
/* ================================================================== */

typedef struct lens_split_opts {
    float min_size;        /* Minimum pane size (e.g. 160.0) */
    float max_size;        /* Maximum pane size (e.g. 400.0) */
    float handle_width;    /* Visual thickness, e.g. 1.0 or 2.0 */
    float hit_expand;      /* Invisible hit-test margin on both sides (e.g. 4.0) */
} lens_split_opts;

/* Render an interactive vertical split divider between two panes.
 * `split_offset` is in-out position (e.g. sidebar width in pixels).
 * Returns true if the position changed due to dragging. */
LENS_API bool lens_split_handle_v(lens *ui,
                                  const char *id_str,
                                  float *split_offset,
                                  const lens_split_opts *opts);

#ifdef __cplusplus
}
#endif

#endif /* LENS_PATTERNS_H */
