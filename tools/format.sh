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

# Include tracked and new sources, but skip index entries whose working-tree
# file was deleted (a clean-break refactor must not make the formatter fail).
SOURCES=$(
    git ls-files --cached --others --exclude-standard \
        'libs/*.c' 'libs/*.h' 'examples/*.c' 'tests/*.c' 'tests/*.h' 'tools/*.c' |
    while IFS= read -r file; do
        if [ -f "$file" ]; then
            printf '%s\n' "$file"
        fi
    done
    true
)

if [ -z "$SOURCES" ]; then
    echo "No C sources found."
    exit 0
fi

if [ "${1:-}" = "--check" ]; then
    echo "$SOURCES" | xargs "$CF" --dry-run --Werror
    echo "All sources match clang-format."
else
    echo "$SOURCES" | xargs "$CF" -i
    echo "Formatting complete."
fi
