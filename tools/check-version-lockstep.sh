#!/bin/sh
# check-version-lockstep.sh — every library's version macros must equal
# meson.project_version().
#
# Why this exists: each library carries its own *_VERSION_MAJOR/MINOR/PATCH
# macros (the consumer compiles against the header and checks the linked
# library at runtime). Those macros drifted in practice:
#
#   - flux-scene-graph's version_string() hard-coded "0.0.29" while the
#     headers said 0.0.36 (four releases of a stale literal);
#   - anim's macros sat at 0.0.1 for seven months.
#
# Both were invisible: nothing parsed the macros mechanically. This
# check is that mechanical parse — it fails the build on any drift, so
# bumping the release version REQUIRES touching every library (which is
# the intended discipline: one version number for the whole stack).

set -e
cd "$(dirname "$0")/.."

# The version from the single source of truth.
PROJECT_VERSION=$(meson introspect . 2>/dev/null | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
    print(data[0]["version"])
except Exception:
    print("")')
if [ -z "$PROJECT_VERSION" ]; then
    # No configured build dir available: read it from meson.build.
    PROJECT_VERSION=$(sed -n "s/^  version : '\([^']*\)'.*/\1/p" meson.build | head -1)
fi
MAJOR=$(echo "$PROJECT_VERSION" | cut -d. -f1)
MINOR=$(echo "$PROJECT_VERSION" | cut -d. -f2)
PATCH=$(echo "$PROJECT_VERSION" | cut -d. -f3)

fail=0

check() {
    header=$1 lib_major=$2 lib_minor=$3 lib_patch=$4 name=$5
    if [ "$lib_major" != "$MAJOR" ] || [ "$lib_minor" != "$MINOR" ] || [ "$lib_patch" != "$PATCH" ]; then
        echo "FAIL $name: ${lib_major}.${lib_minor}.${lib_patch} != project ${MAJOR}.${MINOR}.${PATCH} ($header)"
        fail=1
    fi
}

grab() {
    # grab <header> <PREFIX>: prints "major minor patch" or nothing.
    h=$1 p=$2
    sed -n "s/^#define ${p}_VERSION_MAJOR \([0-9]*\)$/\1/p" "$h" | tail -1
    sed -n "s/^#define ${p}_VERSION_MINOR \([0-9]*\)$/\1/p" "$h" | tail -1
    sed -n "s/^#define ${p}_VERSION_PATCH \([0-9]*\)$/\1/p" "$h" | tail -1
}

for spec in \
    "libs/flux/include/flux/core.h FLUX flux" \
    "libs/lens/include/lens/lens.h LENS lens" \
    "libs/iris/include/iris/app.h IRIS iris" \
    "libs/prism/include/prism/types.h PRISM prism" \
    "libs/transit/include/transit/transit.h TRANSIT transit" \
    "libs/glyph/include/glyph/glyph.h GLYPH glyph" \
    "libs/vista/include/vista/vista.h VISTA vista"
do
    set -- $spec
    header=$1 prefix=$2 name=$3
    if [ ! -f "$header" ]; then
        echo "FAIL $name: header $header missing"
        fail=1
        continue
    fi
    # shellcheck disable=SC2046
    check "$header" $(grab "$header" "$prefix") "$name"
done

# The runtime version strings must be derived from the macros, never a
# literal (the scene-graph "0.0.29" class of bug). Any *_VERSION_STRING
# macro that bakes digits into a library source is a drift waiting to
# happen.
if grep -rn '_VERSION_STRING "[0-9]' libs/*/src libs/*/*/src 2>/dev/null | grep -v Binary; then
    echo "FAIL: hard-coded version literal found (must derive from macros)"
    fail=1
fi

# Rust bindings workspaces and inter-crate internal dependencies must
# stay in lockstep with the C release version.
for cargo_toml in bindings/*-rs/Cargo.toml; do
    [ -f "$cargo_toml" ] || continue
    ws_ver=$(sed -n 's/^version = "\([^"]*\)".*/\1/p' "$cargo_toml" | head -1)
    if [ "$ws_ver" != "${MAJOR}.${MINOR}.${PATCH}" ]; then
        echo "FAIL $cargo_toml: workspace version '$ws_ver' != project ${MAJOR}.${MINOR}.${PATCH}"
        fail=1
    fi
done

for dep_toml in bindings/*-rs/crates/*/Cargo.toml; do
    [ -f "$dep_toml" ] || continue
    # Filter external non-optics crates (e.g. gltf, image)
    bad_deps=$(grep -nE 'path = "[^"]+".*version = "[0-9]+' "$dep_toml" | grep -v "version = \"${MAJOR}.${MINOR}.${PATCH}\"" || true)
    if [ -n "$bad_deps" ]; then
        echo "FAIL $dep_toml: stale inter-crate version dependency:"
        echo "$bad_deps"
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo ""
    echo "Version lockstep broken. Bump every library's *_VERSION_* macros and"
    echo "bindings Cargo.tomls to ${MAJOR}.${MINOR}.${PATCH} (one release, one version, all libraries & bindings)."
    exit 1
fi
echo "version lockstep OK (all libraries and bindings at ${MAJOR}.${MINOR}.${PATCH})"
