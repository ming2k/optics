#!/usr/bin/env bash
# Material-boundary guardrail (ADR-0063): flux owns rendering mechanism and
# must not reference any named material. Materials — liquid glass today —
# live in the prism library. A match here means a material concept leaked
# back into flux; move it to prism instead of weakening this check.
set -euo pipefail
cd "$(dirname "$0")/.."

matches=$(grep -RInE 'liquid_glass|LIQUID_GLASS|prism_|PRISM_' \
  libs/flux \
  --include='*.c' --include='*.h' --include='*.comp' \
  --include='*.vert' --include='*.frag' --include='meson.build' \
  || true)

if [ -n "$matches" ]; then
  echo "error: material boundary violation — flux references a material name:" >&2
  echo "$matches" >&2
  exit 1
fi
