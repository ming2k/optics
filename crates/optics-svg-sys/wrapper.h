/*
 * Bindgen translation unit. Pulls the full public surface of svg.
 * <optics/svg.h> already includes the gfx headers it needs (svg depends on gfx
 * via pkg-config `Requires: gfx`): the canvas, the encoder, and the value
 * types its shapes carry.
 */
#include <optics/svg.h>
