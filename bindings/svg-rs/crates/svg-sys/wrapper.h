/*
 * Bindgen translation unit. Pulls the full public surface of svg.
 * <svg/svg.h> already includes the flux headers it needs (svg depends on flux
 * via pkg-config `Requires: flux`): the canvas, the encoder, and the value
 * types its shapes carry.
 */
#include <svg/svg.h>
