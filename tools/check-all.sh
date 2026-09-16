#!/usr/bin/env bash
# check-all.sh — single authoritative verification gate for Optics (ADR-0098).
#
# Runs all machine-enforced architecture and quality guardrails in sequence.
# Fails fast on the first violation with clear diagnostic output.
#
# Usage:
#   tools/check-all.sh         # verify all guardrails
#   tools/check-all.sh --fix   # automatically format code and update symbols
set -euo pipefail
cd "$(dirname "$0")/.."

FIX=0
if [ "${1:-}" = "--fix" ]; then
    FIX=1
fi

echo "=== Optics Architecture & Quality Guardrails ==="

# 1. Format check
echo -n "[1/6] Code formatting (clang-format)... "
if [ $FIX -eq 1 ]; then
    tools/check-format.sh --fix >/dev/null
    echo "fixed"
else
    if tools/check-format.sh >/dev/null 2>&1; then
        echo "OK"
    else
        echo "FAIL"
        echo "error: code formatting check failed; run 'tools/check-all.sh --fix' to format" >&2
        exit 1
    fi
fi

# 2. Material boundary check (ADR-0063)
echo -n "[2/6] Material boundary separation (ADR-0063)... "
if tools/check-material-boundary.sh >/dev/null 2>&1; then
    echo "OK"
else
    echo "FAIL"
    tools/check-material-boundary.sh
    exit 1
fi

# 3. Version lockstep check
echo -n "[3/6] Version lockstep across libraries... "
if tools/check-version-lockstep.sh >/dev/null 2>&1; then
    echo "OK"
else
    echo "FAIL"
    tools/check-version-lockstep.sh
    exit 1
fi

# 4. No library stdout writes
echo -n "[4/6] Library stdout purity (stderr only)... "
if tools/check-no-library-stdout.sh >/dev/null 2>&1; then
    echo "OK"
else
    echo "FAIL"
    tools/check-no-library-stdout.sh
    exit 1
fi

# 5. Public symbols freshness
echo -n "[5/6] Public symbols reference freshness... "
if [ $FIX -eq 1 ]; then
    python3 tools/gen_symbols.py >/dev/null
    echo "updated"
else
    if python3 tools/gen_symbols.py --check >/dev/null 2>&1; then
        echo "OK"
    else
        echo "FAIL"
        echo "error: docs/reference/symbols.md is stale; run 'tools/check-all.sh --fix'" >&2
        exit 1
    fi
fi

# 6. Documentation governance verification
echo -n "[6/6] Documentation governance protocol... "
if tools/verify.sh . >/dev/null 2>&1; then
    echo "OK"
else
    echo "FAIL"
    tools/verify.sh .
    exit 1
fi

echo "All guardrails passed: tree is clean and compliant."
