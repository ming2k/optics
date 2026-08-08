/* nanosvg implementation translation unit — pulls the parser body out of
 * the header (see README.md in this directory for origin and licence).
 * Only the parser is vendored; the rasterizer (nanosvgrast.h) is not —
 * lens tessellates the flattened paths itself through flux. */

/* The upstream header is kept byte-identical; its one -Wsign-compare
 * warning under our warning_level=3 is silenced here instead. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif

#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
