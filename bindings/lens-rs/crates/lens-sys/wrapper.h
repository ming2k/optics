/*
 * Bindgen translation unit. Pulls the full public surface of lens.
 * <lens/lens.h> already includes the flux core/math/canvas headers it needs
 * (lens depends on flux via pkg-config Requires: flux >= 0.1.0); the lens
 * icon header is listed for completeness.
 */
#include <lens/icon.h>
#include <lens/patterns.h>
#include <lens/lens.h>
