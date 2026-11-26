/*
 * ui/icon.h — format-agnostic vector icon descriptors and per-context registry.
 *
 * Icons are represented as pure vector path commands in a 24x24 logical box
 * with optional per-shape paint runs. Optics intentionally does not parse SVG
 * or bundle third-party icon packs: callers submit vector geometry directly
 * (generated at build time, built procedurally, or translated from any external
 * format library) and register it per `optics_ui` context.
 *
 * Only four minimal geometric chrome glyphs (directional chevrons for built-in
 * disclosure/sort controls) are built in. `OPTICS_UI_ICON_NONE` is 0 so
 * C23 `{}` zero-initialization always means "no icon".
 */

#ifndef OPTICS_UI_ICON_H
#define OPTICS_UI_ICON_H

#include <optics/gfx/core.h>
#include <optics/gfx/math.h>
#include <optics/ui/export.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct optics_ui optics_ui;

/* Built-in geometric chrome glyphs + sentinel. Runtime-registered icons carry
 * context-scoped generational handles (`>= 0x10000u`). */
typedef enum optics_ui_icon_id : uint32_t {
    OPTICS_UI_ICON_NONE = 0,
    OPTICS_UI_ICON_CHEVRON_UP = 1,
    OPTICS_UI_ICON_CHEVRON_DOWN = 2,
    OPTICS_UI_ICON_CHEVRON_LEFT = 3,
    OPTICS_UI_ICON_CHEVRON_RIGHT = 4,
    OPTICS_UI_ICON_BUILTIN_COUNT = 5,
} optics_ui_icon_id;

/* Primitive vector command verbs in a 24x24 icon coordinate space. */
typedef enum optics_ui_icon_cmd_kind : uint8_t {
    OPTICS_UI_ICON_CMD_MOVE = 0,   /* params: x, y */
    OPTICS_UI_ICON_CMD_LINE = 1,   /* params: x, y */
    OPTICS_UI_ICON_CMD_CUBIC = 2,  /* params: c1x, c1y, c2x, c2y, x, y */
    OPTICS_UI_ICON_CMD_QUAD = 3,   /* params: cx, cy, x, y */
    OPTICS_UI_ICON_CMD_CLOSE = 4,  /* no params */
    OPTICS_UI_ICON_CMD_CIRCLE = 5, /* params: cx, cy, r */
    OPTICS_UI_ICON_CMD_RECT = 6,   /* params: x, y, w, h */
} optics_ui_icon_cmd_kind;

/* How a vector command range is painted. */
typedef enum optics_ui_icon_paint : uint8_t {
    OPTICS_UI_ICON_PAINT_STROKE = 0,
    OPTICS_UI_ICON_PAINT_FILL = 1,
} optics_ui_icon_paint;

typedef struct optics_ui_icon_cmd {
    optics_ui_icon_cmd_kind kind;
    uint8_t _pad[3];
    float params[6];
} optics_ui_icon_cmd;

/* A contiguous sub-range of an icon's command stream painted with its own
 * colour and fill/stroke mode. `color == 0` follows the widget/theme
 * foreground colour; non-zero is a premultiplied `optics_gfx_color`
 * (0xAARRGGBB). */
typedef struct optics_ui_icon_run {
    uint32_t first_cmd;
    uint32_t count;
    optics_gfx_color color;
    optics_ui_icon_paint paint;
    uint8_t _pad[3];
} optics_ui_icon_run;

/* Immutable vector icon description in a 24x24 logical box. When `runs` is
 * nullptr (or `run_count == 0`), the entire command stream is painted as one
 * theme-coloured run using `paint`. */
typedef struct optics_ui_icon_desc {
    const optics_ui_icon_cmd *cmds;
    uint32_t count;
    const optics_ui_icon_run *runs;
    uint32_t run_count;
    optics_ui_icon_paint paint;
    uint8_t _pad[3];
} optics_ui_icon_desc;

/* Register a vector icon on `ui`. Deep-copies `desc->cmds` and `desc->runs`
 * into context-owned memory so the caller's buffers need not outlive the call.
 * Returns `OPTICS_UI_ICON_NONE` on invalid input or allocation failure. */
OPTICS_GFX_NODISCARD OPTICS_UI_API optics_ui_icon_id
optics_ui_icon_register(optics_ui *ui, const optics_ui_icon_desc *desc);

/* Unregister a runtime icon previously returned by `optics_ui_icon_register`,
 * reclaiming its storage and invalidating cached coverage masks. Passing
 * `OPTICS_UI_ICON_NONE`, a built-in id, or an already-unregistered id is a
 * safe no-op. */
OPTICS_UI_API void optics_ui_icon_unregister(optics_ui *ui, optics_ui_icon_id id);

/* Resolve an icon id (built-in or registered on `ui`) to its descriptor, or
 * nullptr if unknown. Built-in chrome ids resolve even when `ui == nullptr`.
 * The returned pointer stays valid until the icon is unregistered or `ui` is
 * released. */
OPTICS_UI_API const optics_ui_icon_desc *optics_ui_icon_info(const optics_ui *ui,
                                                             optics_ui_icon_id id);

#ifdef __cplusplus
}
#endif

#endif /* OPTICS_UI_ICON_H */
