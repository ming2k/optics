/*
 * icon_svg.h — lightweight, robust C23 SVG icon path and style parser.
 *
 * Designed for immediate-mode icon rendering in Lens. Replaces third-party
 * monolithic parsers with a focused, self-contained implementation.
 */
#ifndef LENSI_ICON_SVG_H
#define LENSI_ICON_SVG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lens/icon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lensi_svg_shape {
    uint32_t color;      /* 0xRRGGBBAA or 0 for currentColor/theme */
    bool filled;         /* true = fill, false = stroke */
    uint32_t first_cmd;  /* index in global cmd buffer */
    uint32_t cmd_count;  /* number of cmds in this shape */
} lensi_svg_shape;

typedef struct lensi_svg_result {
    lens_icon_cmd *cmds;
    uint32_t cmd_count;
    lens_icon_run *runs;
    uint32_t run_count;
    bool has_fill;
    bool has_stroke;
} lensi_svg_result;

/*
 * Parse an SVG string into a normalized 24x24 icon command stream and color runs.
 * Returns true on success, false if the SVG was malformed or empty.
 * Caller owns cmds and runs on success and must free() them.
 */
bool lensi_svg_parse(const char *svg_text, lensi_svg_result *out);

#ifdef __cplusplus
}
#endif

#endif /* LENSI_ICON_SVG_H */
