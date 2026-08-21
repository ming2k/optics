#!/usr/bin/env bash
# Format guardrail: every C source in the tree must be byte-identical to
# what the pinned clang-format version produces from the root
# .clang-format. Vendored code is upstream-owned and out of scope; the
# build trees are generated.
#
# The version is pinned (not "whatever apt ships") because formatting
# drifts between clang-format releases — an unpinned check would flake
# across runner image updates and erode trust in the lock. CI installs
# the pinned version from apt.llvm.org; locally, match it or expect
# noise. Usage:
#   tools/check-format.sh [clang-format]        # default: clang-format on PATH
#   tools/check-format.sh --fix [clang-format]  # rewrite files in place
set -euo pipefail
cd "$(dirname "$0")/.."

PINNED_MAJOR=22
FIX=0
if [ "${1:-}" = "--fix" ]; then
  FIX=1
  shift
fi
CF="${1:-clang-format}"

actual=$("$CF" --version | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
actual_major=${actual%%.*}
if [ "$actual_major" != "$PINNED_MAJOR" ]; then
  echo "error: clang-format $PINNED_MAJOR.x is pinned; found $actual" >&2
  echo "       install: apt-get install clang-format-$PINNED_MAJOR (apt.llvm.org)" >&2
  exit 1
fi

# shellcheck disable=SC2046
files=$(find libs examples tests -name '*.c' -o -name '*.h' | grep -v vendor | sort)
[ -n "$files" ] || { echo "error: no C sources found" >&2; exit 1; }

if [ "$FIX" = 1 ]; then
  # shellcheck disable=SC2086
  $CF -i $files
  echo "reformatted in place: $(echo "$files" | wc -l) files"
  exit 0
fi

status=0
# shellcheck disable=SC2086
for f in $($CF --dry-run --Werror $files 2>&1 | grep -oE '^[^:]+\.c?h?:[0-9]+' | cut -d: -f1 | sort -u); do
  echo "needs formatting: $f" >&2
  status=1
done
if [ "$status" = 1 ]; then
  echo "run: tools/check-format.sh --fix" >&2
fi
exit $status
