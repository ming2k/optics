/*
 * Bindgen translation unit for the iris bindings.
 *
 * Pulls the full public surface of libiris. <iris/iris.h> already
 * includes <lens/lens.h>; iris's pkg-config Requires: lens >= 0.1.0
 * propagates the lens + flux include paths.
 */
#include <iris/iris.h>
