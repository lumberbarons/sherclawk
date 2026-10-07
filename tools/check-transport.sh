#!/bin/bash
# Fake OT/crypto boundaries exercise actual patched MacTLS entry points under
# ASan/UBSan. A private staging repo prevents git apply skipping ignored paths.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${CERTAINLY_DIR:-$HERE/../Certainly}"
if [ ! -d "$SOURCE/include" ]; then
    echo "error: Certainly not found at $SOURCE" >&2
    echo "clone it next to this repository (fetch-only):" >&2
    echo "  git clone --recursive --depth 1 https://github.com/minorbug/certainly.git Certainly" >&2
    exit 1
fi
STAGE="$HERE/build/transport-tests"
mkdir -p "$STAGE"
cp -R "$SOURCE/src" "$SOURCE/include" "$STAGE/"
git init -q "$STAGE"
for patch in "$HERE"/patches/*.patch; do (cd "$STAGE" && git apply "$patch"); done
LINK_GC=-Wl,--gc-sections
if [ "$(uname -s)" = Darwin ]; then LINK_GC=-Wl,-dead_strip; fi
"${CC:-cc}" -std=gnu99 -O1 -g -Wall -Wextra \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -ffunction-sections -fdata-sections \
    -I"$HERE/vendor/host-tls/shim" -I"$STAGE/include" -I"$STAGE/src" \
    -I"$SOURCE/bearssl/inc" "$HERE/tests/test_tls_io.c" "$STAGE/src/certainly.c" \
    "$LINK_GC" -o "$STAGE/test-tls-io"
"$STAGE/test-tls-io"
