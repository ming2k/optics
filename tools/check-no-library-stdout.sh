#!/usr/bin/env bash
# Library-stdout guardrail: a library's diagnostics must never reach the
# consumer's stdout — the convention flux_console_logger documents. For a
# consumer whose stdout is an IPC wire (observed in production), one
# stray printf is fully buffered on pipes, flushes at process exit after
# the protocol response, and corrupts the wire. Vendored code is
# upstream-owned and out of scope.
set -euo pipefail
cd "$(dirname "$0")/.."

matches=$(grep -RInE '(^|[^A-Za-z_])printf[[:space:]]*\(' \
  libs \
  --include='*.c' --include='*.h' --include='*.m' \
  --exclude-dir=vendor \
  || true)

if [ -n "$matches" ]; then
  echo "error: library source writes to stdout — diagnostics belong on stderr:" >&2
  echo "$matches" >&2
  exit 1
fi
