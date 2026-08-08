#!/usr/bin/env bash
# zig-win32-check.sh — compile-check C sources for 64-bit Windows without a
# Windows machine, using `zig cc` (bundled MinGW-w64 headers + clang).
#
# Usage:
#   tools/zig-win32-check.sh [-I<dir> ...] [-D<def> ...] file.c [file2.c ...]
#
# Flags before the first .c file are forwarded to every compile. Each file is
# compiled to an object in a scratch dir (full codegen, since zig's clang
# wrapper mishandles -fsyntax-only). Exits non-zero if any file fails.
#
# Vulkan headers come from the host's /usr/include/vulkan (+ vk_video), which
# are platform-neutral; they are symlinked into an isolated include dir so the
# host's glibc headers never leak into the Windows target compile (mixing them
# breaks uintptr_t / VkFlags64).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INC="$ROOT/.cache/zig-win32/inc"
mkdir -p "$INC"
ln -sfn /usr/include/vulkan "$INC/vulkan"
ln -sfn /usr/include/vk_video "$INC/vk_video"

FLAGS=()
FILES=()
for arg in "$@"; do
    case "$arg" in
        -I*) FLAGS+=("-I$(realpath -m "${arg#-I}")") ;;
        --embed-dir=*) FLAGS+=("--embed-dir=$(realpath -m "${arg#--embed-dir=}")") ;;
        -*) FLAGS+=("$arg") ;;
        *) FILES+=("$arg") ;;
    esac
done

if [ "${#FILES[@]}" -eq 0 ]; then
    echo "usage: $0 [-I<dir> ...] [-D<def> ...] file.c [file2.c ...]" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

status=0
for f in "${FILES[@]}"; do
    abs="$(realpath "$f")"
    obj="$WORK/$(basename "$f").o"
    if (cd "$WORK" && zig cc -target x86_64-windows-gnu -std=c23 -I"$INC" "${FLAGS[@]}" -c "$abs" -o "$obj") 2>"$WORK/err"; then
        echo "OK   $f"
    else
        echo "FAIL $f"
        sed 's/^/     /' "$WORK/err"
        status=1
    fi
done
exit "$status"
