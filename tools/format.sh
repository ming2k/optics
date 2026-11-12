#!/bin/sh
# Formats all C source files in the repository.
# Usage:
#   ./tools/format.sh          # format in place
#   ./tools/format.sh --check  # check without modifying (exit 1 if diff exists)
set -eu

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

CF="${CLANG_FORMAT:-clang-format}"

if ! command -v "$CF" >/dev/null 2>&1; then
    echo "error: clang-format not found in PATH" >&2
    exit 1
fi

SOURCES=$(git ls-files 'libs/*.c' 'libs/*.h' 'examples/*.c' 'tests/*.c' 'tests/*.h')

if [ "${1:-}" = "--check" ]; then
    echo "$SOURCES" | xargs "$CF" --dry-run --Werror
    echo "All sources match clang-format."
else
    echo "$SOURCES" | xargs "$CF" -i
    echo "Formatting complete."
fi
