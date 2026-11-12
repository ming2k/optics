/*
 * Bindgen translation unit. Pulls the full public surface of reticle.
 * <reticle/reticle.h> already includes the flux headers it needs (reticle depends on flux
 * via pkg-config `Requires: flux`): the canvas, the encoder, and the value
 * types its shapes carry.
 */
#include <reticle/reticle.h>
