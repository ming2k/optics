/*
 * Bindgen translation unit for the prism bindings.
 *
 * <prism/prism.h> is the umbrella; it pulls in <prism/liquid_glass.h>, which
 * itself includes <flux/core.h> and <flux/math.h> (prism depends on flux via
 * pkg-config `Requires: flux >= 0.0.13`, so the flux headers come from the
 * probed flux include paths).
 *
 * The allowlist keeps only prism_* items. The flux types those signatures
 * reference are blocklisted and re-exported from flux-sys instead (see
 * build.rs), so flux_result and the handle types have a single Rust
 * definition across the stack.
 */
#include <prism/prism.h>
