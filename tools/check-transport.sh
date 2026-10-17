#!/bin/bash
# Fake OT/crypto boundaries exercise the actual MacTLS entry points under
# ASan/UBSan.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ ! -d "$SOURCE/include" ]; then
    echo "error: Certainly not found at $SOURCE" >&2
    echo "clone it next to this repository (fetch-only):" >&2
    echo "  git clone --depth 1 https://github.com/lumberbarons/certainly.git Certainly" >&2
    exit 1
fi
STAGE="$HERE/build/transport-tests"
mkdir -p "$STAGE"
export SHERCLAWK_COST_LOG="$STAGE/cost.log"
: > "$SHERCLAWK_COST_LOG"
LINK_GC=-Wl,--gc-sections
if [ "$(uname -s)" = Darwin ]; then LINK_GC=-Wl,-dead_strip; fi
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -I"$HERE/vendor/host-tls/shim" -I"$SOURCE/include" -I"$SOURCE/src" \
    -I"$SOURCE/bearssl/inc" "$HERE/tests/test_tls_io.c" "$SOURCE/src/certainly.c" \
    "$LINK_GC" -o "$STAGE/test-tls-io"
"$STAGE/test-tls-io"
echo
echo "Cost table (host counts; convert to guest ticks with docs/development.md#cost-table-and-guest-calibration)"
printf '%-62s %8s %8s\n' counter measured bound
cat "$SHERCLAWK_COST_LOG"
