/*
 * Bindgen translation unit for the material bindings.
 *
 * <optics/material.h> is the umbrella; it pulls in <optics/material/liquid_glass.h>, which
 * itself includes <optics/gfx/core.h> and <optics/gfx/math.h> (material depends on gfx via
 * pkg-config `Requires: gfx >= 0.0.13`, so the gfx headers come from the
 * probed gfx include paths).
 *
 * The allowlist keeps only optics_material_* items. The gfx types those signatures
 * reference are blocklisted and re-exported from gfx-sys instead (see
 * build.rs), so optics_gfx_result and the handle types have a single Rust
 * definition across the stack.
 */
#include <optics/material.h>
