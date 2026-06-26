/*
 * Bindgen translation unit for the flux-text bindings.
 *
 * Includes <flux-text/text.h> directly. That header in turn includes
 * <flux/core.h>, <flux/math.h>, <flux/canvas.h> (flux-text is a consumer of
 * the canvas glyph-blit primitive, ADR-0016); pkg-config(flux-text) Requires
 * flux, so flux's include path is on the bindgen command line via Cflags.
 *
 * The allowlist confines output to flux_text_* (and FLUX_TEXT_*). The flux
 * handle types the text API borrows (flux_device, flux_canvas, flux_arena)
 * appear as opaque forward decls; the safe `flux-text` crate casts the
 * ABI-identical pointers from `flux-sys` at the call seam.
 */
#include <flux-text/text.h>
