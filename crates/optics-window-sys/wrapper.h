/*
 * Bindgen translation unit for the iris bindings.
 *
 * Pulls the full public surface of libiris. iris depends on flux only; its
 * pkg-config `Requires: flux` propagates the flux include path.
 */
#include <optics/window.h>
